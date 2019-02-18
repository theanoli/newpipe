#include "harness.h"

#define DEBUG_PORTS 0

void
Init (ProgramArgs *pargs, int *pargc, char ***pargv)
{
    ThreadArgs *p = (ThreadArgs *)calloc (pargs->nthreads, 
            sizeof (ThreadArgs));

    if (p == NULL) {
        printf ("Error malloc'ing space for thread args!\n");
        exit (-10);
    }
    
    pargs->thread_data = p;

    pargs->tids = (pthread_t *)calloc (pargs->nthreads, sizeof (pthread_t));
    if (pargs->tids == NULL) {
        printf ("Failed to malloc space for tids!\n");
        exit (-82);
    }

    struct mtcp_conf mcfg;
    mtcp_getconf (&mcfg);
    mcfg.num_cores = pargs->nthreads;
    mtcp_setconf (&mcfg);
	mtcp_init ("mtcp.conf");
}


void
LaunchThreads (ProgramArgs *pargs)
{
    int i;
    // int nprocs_onln;
    // cpu_set_t cpuset;

    ThreadArgs *targs = pargs->thread_data;

    for (i = 0; i < pargs->nthreads; i++) {
        targs[i].machineid = pargs->machineid;
        targs[i].threadid = i;
        snprintf (targs[i].threadname, 128, "[%s.%d]", pargs->machineid, i);

        // All clients will connect to the same server port
        targs[i].host = pargs->host;
        targs[i].tr = pargs->tr;
        targs[i].program_state = startup;

        // For servers, this is how many clients from which to expect connections.
        // For clients, this is how many total server threads there are. 
        targs[i].ncli = pargs->ncli;
        targs[i].ep = -1;
        targs[i].no_record = pargs->no_record;
        targs[i].counter = 0;

        // Each server thread gets a listening port, and each client will be
        // assigned a server port 
        if (!pargs->tr) {
            targs[i].port = pargs->port + (i % pargs->nthreads);
        } else {
            targs[i].port = pargs->port + (i % pargs->ncli);
        }
        targs[i].nports = pargs->nports;

        debug_print (&targs[i], DEBUG_PORTS, "connecting to port %d\n", targs[i].port);

        if (pargs->no_record) {
            printf ("Not recording measurements to file.\n");
        } else {    
            setup_filenames (&targs[i]);
            if (pargs->tr) {
                targs[i].lbuf = (char *) malloc (LBUFSIZE);
                if (targs[i].lbuf == NULL) {
                    printf ("Couldn't allocate space for latency buffer! Exiting...\n");
                    exit (-14);
                }
                targs[i].lbuf_offset = 0;
                memset (targs[i].lbuf, 0, LBUFSIZE);
            }
        }

        // printf ("[%s] Launching thread %d, ", pargs->machineid, i);
        // printf ("connecting to portno %d\n", targs[i].port);
        fflush (stdout);

        pthread_create (&pargs->tids[i], NULL, ThreadEntry, (void *)&targs[i]);
        
        // Always pin cores
        // nprocs_onln = get_nprocs ();
        // CPU_ZERO (&cpuset);
        // CPU_SET (i % nprocs_onln, &cpuset);
        // ret = pthread_setaffinity_np (pargs->tids[i], sizeof (cpu_set_t), &cpuset);
        // if (ret != 0) {
        //     printf ("[%s] Couldn't pin thread %d to core!\n", pargs->machineid, i);
        //     exit (-14);
        // }
    }
}

int write_helper (ThreadArgs *p, int sockfd)
{
    struct timespec sendtime;
    int written; 
    char pbuf[PSIZE];
    int j;
    mctx_t mctx = p->prot.mctx;

    sendtime = PreciseWhen ();
    snprintf (pbuf, PSIZE, "%lld,%.9ld",
            (long long) sendtime.tv_sec, sendtime.tv_nsec);
    written = 0;
    while (1) {
        j = mtcp_write (mctx, sockfd, pbuf + written, 
                PSIZE - 1 - written);
        if (j < 0) {
            if (errno != EAGAIN) {
                perror ("client write");
                return -1;
            }
        }

        written += j;
        if (written >= PSIZE - 1) {
            break;
        }
    }

    debug_print (p, DEBUG_PORTS, "Wrote %s, %d bytes to server\n", pbuf, written);

    if (written < PSIZE - 1) {
        id_print (p, "written: only wrote %d bytes\n", written);
        return -1;
    }
    return 0;
}

int 
TimestampTxRx (ThreadArgs *p)
{
    // Send and then receive an echoed timestamp.
    // Return a pointer to the stored timestamp. 
    char pbuf[PSIZE];  // for packets
    int i, j, n; 
    struct mtcp_epoll_event events[MAXEVENTS];
    int nread; 
    double packet_timestamp __attribute__((unused));

    struct timespec recvtime;
    FILE *out;
    mctx_t mctx = p->prot.mctx;

    // Open file for recording latency data
    if (p->tr && (!p->no_record)) {
        if ((out = fopen (p->latency_outfile, "wb")) == NULL) {
            fprintf (stderr, "Can't open %s for output!\n", p->latency_outfile);
            exit (1);
        }
    } else {
        out = stdout;
    }

    while (p->program_state != experiment) {
    }

    for (i = 0; i < p->nports; i++) {
        j = write_helper (p, p->nports_array[i]);
        if (j < 0) {
            return j;
        }
    }
    
    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {
        // And now wait for response to arrive
        n = mtcp_epoll_wait (mctx, p->ep, events, MAXEVENTS, 100);
        if (n < 0) {
            perror ("epoll_wait");
            return -1;
        }

        for (i = 0; i < n; i++) {
            if ((events[i].events & MTCP_EPOLLERR) ||
                    (events[i].events & MTCP_EPOLLHUP) ||
                    (events[i].events & !MTCP_EPOLLIN)) {
                id_print (p, "epoll error!\n");
                int error = 0;
                socklen_t errlen = sizeof (error);
                if (mtcp_getsockopt (mctx, events[i].data.sockid, SOL_SOCKET, 
                            SO_ERROR, (void *)&error, &errlen) == 0) {
                    printf (": %s\n", strerror (error));
                }
                return -1;
            } else if (events[i].events & MTCP_EPOLLIN) {
                // Read data
                nread = 0;
                memset (pbuf, 0, PSIZE);

                while (1) {
                    j = mtcp_read (mctx, events[i].data.sockid, pbuf + nread, 
                            PSIZE - nread);
                    if (j < 0) {
                        break;
                    }

                    nread += j;
                }
                
                if ((j < 0) && errno == EAGAIN) {
                    recvtime = PreciseWhen ();
                    (p->counter)++;

                    if ((!p->no_record) && (p->counter % 1000 == 0)) {
                        write_latency_data (p, out, pbuf, &recvtime);
                    }

                    debug_print (p, DEBUG_PORTS, 
                            "Got timestamp: %s, %d bytes read\n", pbuf, nread);
                } else {
                    perror ("client read2");
                    return -1;
                }

                // Send a timestamp
                j = write_helper (p, events[i].data.sockid);
                if (j < 0) {
                    return j;
                }
            }
        }
    }
    return 0;
}


int
Echo (ThreadArgs *p)
{
    // Server-side only!
    // Loop through for expduration seconds and count each packet you send out
    // Start counting packets after a few seconds to stabilize connection(s)
    int j, n, i;
    int nconnections = p->ncli;
    struct mtcp_epoll_event events[MAXEVENTS];
    int to_write;
    int written;
    char rcv_buf[PSIZE];
    mctx_t mctx = p->prot.mctx;

    // Add a few-second delay to let clients stabilize...
    while (p->program_state == startup) {
        // Should never be in this status in this function, 
        // but just in case
    }

    // id_print (p, "Entering packet receive mode...\n");

    while (p->program_state != end) {
        n = mtcp_epoll_wait (mctx, p->ep, events, MAXEVENTS, 100);

        if (n < 0) {
            perror ("epoll_wait");
            return -1;
        }
        
        debug_print (p, DEBUG_PORTS, "Got %d events\n", n);

        for (i = 0; i < n; i++) {
            debug_print (p, DEBUG_PORTS, "Servicing event from %d\n", events[i].data.sockid);
            // Check for errors
            if ((events[i].events & MTCP_EPOLLERR) ||
                    (events[i].events & MTCP_EPOLLHUP) ||
                    (events[i].events & !MTCP_EPOLLIN)) {
                id_print (p, "epoll error!\n");
                int error = 0;
                socklen_t errlen = sizeof (error);
                if (mtcp_getsockopt (mctx, events[i].data.sockid, SOL_SOCKET, 
                            SO_ERROR, (void *)&error, &errlen) == 0) {
                    printf (": %s\n", strerror (error));
                }
                return -1;
            } else if (events[i].data.sockid == p->servicefd) {
                // Someone is trying to connect; ignore. All clients should have
                // connected already.
                debug_print (p, DEBUG_PORTS, "connection attempted\n");
                continue;
            } else {
                // There's data to be read
                to_write = 0;
                memset (rcv_buf, 0, PSIZE);

                while (1) {
                    j = mtcp_read (mctx, events[i].data.sockid, rcv_buf + to_write,
                            PSIZE - to_write);
                    if (j < 0) {
                        break;
                    }

                    to_write += j;
                }

                debug_print (p, DEBUG_PORTS, "Got %s, %d bytes from %d\n", 
                        rcv_buf, to_write, events[i].data.sockid);
                // Just in case
                if (to_write == 0) {
                    id_print (p, "towrite: read 0 bytes\n");
                    return -1;
                } 

                if ((j < 0) && errno == EAGAIN) {
                    // We read everything; echo back to the client
                    written = 0;
            
                    while (1) {
                        j = mtcp_write (mctx, events[i].data.sockid,
                                rcv_buf + written, to_write - written);
                        if (j < 0) {
                            if (errno != EAGAIN) {
                                perror ("server write");
                                return -1;
                            }
                            break;
                        }
                        written += j;
                        if (to_write == written) {
                            break;
                        }
                        id_print (p, "Had to loop, to_write=%d, written=%d...\n",
                                to_write, written);
                    }

                    debug_print (p, DEBUG_PORTS, "Wrote %s, %d bytes to %d\n",
                            rcv_buf, written, events[i].data.sockid);
                    // Just in case
                    if (written < to_write) {
                        id_print (p, "written: had %d, only wrote %d\n", to_write, written);
                        return -1;
                    }

                    (p->counter)++;
                } else {
                    perror ("server read2");
                    return -1;
                }
            }
        }
        if (nconnections <= 0) {
            printf ("No more connections left! Exiting...\n");
            return -1;
        }
    }
    return 0;
}


void
Setup (ThreadArgs *p)
{
    int sockfd;
    struct sockaddr_in *lsin;

    int i, ret;
    char portno[7]; 

    struct mtcp_epoll_event event;

    lsin = &(p->prot.sin1);
    memset ((char *) lsin, 0, sizeof (*lsin));
    sprintf (portno, "%d", p->port);

    int core = p->threadid % get_nprocs ();

    if (p->tr) {
        if (!p->prot.mctx) {
            mtcp_core_affinitize (core);
            p->prot.mctx = mtcp_create_context (core);
        }

        if (!p->prot.mctx) {
            printf ("tester: can't create mTCP socket!");
            exit (-4);
        }

        mctx_t mctx = p->prot.mctx;

        p->ep = mtcp_epoll_create (mctx, MAXNPORTS);

        lsin->sin_family = AF_INET;
        lsin->sin_addr.s_addr = inet_addr (p->host);
        lsin->sin_port = htons (p->port);

        for (i = 0; i < p->nports; i++) {
            sockfd = mtcp_socket (mctx, AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                id_print (p, "Failed to create socket!\n");
                exit (-10);
            }
            
            // I think this is obligatory...
            mtcp_setsock_nonblock (mctx, sockfd);
            int j = 0;
            ret = mtcp_setsockopt (mctx, sockfd, SOL_SOCKET, 
                    TCP_QUICKACK, &j, sizeof (j));
            if (ret < 0) {
                perror ("tcp_quickack");
                exit (-1);
            }

            ret = mtcp_connect (mctx, sockfd, (struct sockaddr *)lsin,
                    sizeof (struct sockaddr));
            if (ret < 0) {
                if (errno != EINPROGRESS) {
                    perror ("connect");
                    exit (-12);
                }
            }

            event.data.sockid = sockfd;
            event.events = MTCP_EPOLLIN | MTCP_EPOLLET;
            if (mtcp_epoll_ctl (mctx, p->ep, MTCP_EPOLL_CTL_ADD,
                        sockfd, &event) < 0) {
                perror ("setup epoll_ctl");
                exit (-1);
            }

            p->nports_array[i] = sockfd;
        }

    } else {
        // Every server should open a listening socket; any connected client will
        // only communicate with that server
        lsin->sin_family       = AF_INET;
        lsin->sin_addr.s_addr  = htonl (INADDR_ANY);
        lsin->sin_port         = htons (p->port);

        if (!p->prot.mctx) {
            mtcp_core_affinitize (core);
            p->prot.mctx = mtcp_create_context (core);
        }

        if (!p->prot.mctx) {
            printf ("tester: can't create mTCP context!");
            exit (-4);
        }

        mctx_t mctx = p->prot.mctx;

        p->ep = mtcp_epoll_create (mctx, MAXNPORTS);

        if ((sockfd = mtcp_socket (mctx, AF_INET, SOCK_STREAM, 0)) < 0) {
            printf ("tester: can't open stream socket!\n");
            exit (-4);
        }

        mtcp_setsock_nonblock (mctx, sockfd);

        if (mtcp_bind (mctx, sockfd, (struct sockaddr *) lsin, sizeof (*lsin)) < 0) {
            printf ("tester: server: bind on local address failed! errno=%d\n", errno);
            exit (-6);
        }

        p->servicefd = sockfd;

        establish (p);
    }
}


void
establish (ThreadArgs *p)
{
    // TODO this FD needs to get added to an epoll instance
    socklen_t clen;
    int nevents, i;
    struct mtcp_epoll_event events[MAXEVENTS];
    struct mtcp_epoll_event event;
    int connections = 0;
    int sockfd;
    mctx_t mctx = p->prot.mctx;

    clen = (socklen_t) sizeof (p->prot.sin2);
    
    event.events = MTCP_EPOLLIN;
    event.data.sockid = p->servicefd;

    if (mtcp_epoll_ctl (mctx, p->ep, MTCP_EPOLL_CTL_ADD, p->servicefd, &event) == -1) {
        perror ("epoll_ctl");
        exit (1);
    }

    mtcp_listen (mctx, p->servicefd, 1024);

    id_print (p, "Starting loop to wait for connections...\n");

    while (p->program_state == startup) {
        nevents = mtcp_epoll_wait (mctx, p->ep, events, MAXEVENTS, 0); 
        if (nevents < 0) {
            if (errno != EINTR) {
                perror ("epoll_wait");
            }
            exit (1);
        }

        for (i = 0; i < nevents; i++) {
            if (events[i].data.sockid == p->servicefd) {
                while (1) {
                    sockfd = mtcp_accept (mctx, p->servicefd, 
                            (struct sockaddr *) &(p->prot.sin2), &clen);

                    if (sockfd == -1) {
                       if (errno == EAGAIN) {
                           break;
                       } else {
                           perror ("accept");
                           break;
                       }
                    }

                    // Set socket to nonblocking
                    if (mtcp_setsock_nonblock (mctx, sockfd) < 0) {
                        printf ("Error setting socket to non-blocking!\n");
                        continue;
                    }

                    // Add descriptor to epoll instance
                    event.data.sockid = sockfd;
                    event.events = MTCP_EPOLLIN;  
                    if (mtcp_epoll_ctl (mctx, p->ep, MTCP_EPOLL_CTL_ADD, 
                                sockfd, &event) < 0) {
                        perror ("epoll_ctl");
                        exit (1);
                    }

                    connections++;
                    if (!(connections % 50)) {
                        printf ("%d connections so far...\n", connections);
                    }
                }
            }
        }
    }

    p->ncli = connections;
    id_print (p, "Got %d connections\n", connections);
}


void
CleanUp (ThreadArgs *p)
{
    id_print (p, "Cleaning up!\n"); 
    mctx_t mctx = p->prot.mctx;

    if (!p->tr) {
        mtcp_close (mctx, p->servicefd);
    }

    id_print (p, "Closed sockets\n");

    // mtcp_destroy_context (mctx);
    id_print (p, "Finished cleaning up\n");
}
