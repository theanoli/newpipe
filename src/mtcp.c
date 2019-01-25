#include "harness.h"

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
        targs[i].done = &pargs->done;

        // For servers, this is how many clients from which to expect connections.
        // For clients, this is how many total server threads there are. 
        targs[i].ncli = pargs->ncli;
        targs[i].ep = -1;
        targs[i].no_record = pargs->no_record;
        targs[i].counter = 0;

        // Each server thread gets a listening port, and each client will be
        // assigned a server port 
        if (!pargs->tr) {
            targs[i].port = (pargs->port + i) % pargs->nthreads;
        } else {
            targs[i].port = (pargs->port + i) % pargs->ncli;
        }

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


int 
TimestampTxRx (ThreadArgs *p)
{
    // Send and then receive an echoed timestamp.
    // Return a pointer to the stored timestamp. 
    char pbuf[PSIZE];  // for packets
    int j, n, m, i, read;
    struct mtcp_epoll_event events[1];
    struct mtcp_epoll_event event;
    struct timespec sendtime, recvtime;
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

    event.data.sockid = p->commfd;
    event.events = MTCP_EPOLLIN | MTCP_EPOLLET;
    if (mtcp_epoll_ctl (mctx, p->ep, MTCP_EPOLL_CTL_ADD,
                p->commfd, &event) < 0) {
        perror ("epoll_ctl");
        exit (1);
    }

    while (p->program_state != experiment) {
    }
    
    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {
        sendtime = PreciseWhen ();

        snprintf (pbuf, PSIZE, "%lld,%.9ld",
                (long long) sendtime.tv_sec, sendtime.tv_nsec);

        // This might be overkill---will write ever block?
        while ((j = mtcp_write (mctx, p->commfd, pbuf, PSIZE - 1)) < 0) {
            if (j < 0) {
                if (errno != EWOULDBLOCK) {
                    perror ("client write");
                    CleanUp (p);
                    exit (1);
                }
            }
        }

        debug_print (p, DEBUG, "pbuf: %s, %d bytes written\n", pbuf, j);
        memset (pbuf, 0, PSIZE);
        
        // And now wait for response to arrive
        n = mtcp_epoll_wait (mctx, p->ep, events, 1, 1);
        if (n < 0) {
            perror ("epoll_wait");
            return -1;
        }

        if (n > 1) {
            debug_print (p, CLIENT_DEBUG, "Got more than 1 event!\n");
        }
        for (i = 0; i < n; i++) {
            if ((events[0].events & MTCP_EPOLLERR) ||
                    (events[0].events & MTCP_EPOLLHUP) ||
                    (events[0].events & !MTCP_EPOLLIN)) {
                id_print (p, "epoll error!\n");
                return -1;
            } else {
                // Read data
                read = 0;
                while (1) {
                    j = mtcp_read (mctx, events[0].data.sockid, pbuf + read, PSIZE);
                    if (j <= 0) {
                        break;
                    }
                    read += j;
                }
                
                if ((j == 0) || 
                        ((j < 0) && (errno != EAGAIN)) || 
                        (read > PSIZE))  {
                    // Other side is disconnected, an error occurred, 
                    // or we've read too much
                    perror ("client read");
                    return -1;
                }
            }

            recvtime = PreciseWhen ();        
            (p->counter)++;

            debug_print (p, DEBUG, "Got timestamp: %s, %d bytes read\n", pbuf, n);
            fflush (stdout);

            if ((!p->no_record) && (p->counter % 100 == 0)) {
                if (p->lbuf_offset > (LBUFSIZE - 41)) {
                    debug_print (p, DEBUG, "%s", p->lbuf, p->lbuf_offset);
                    // Flush buffer to file
                    // 41 is the longest possible write
                    fwrite (p->lbuf, 1, p->lbuf_offset, out);
                    memset (p->lbuf, 0, LBUFSIZE);
                    p->lbuf_offset = 0;
                } else {
                    // More capacity in the buffer
                    m = snprintf (p->lbuf + p->lbuf_offset, PSIZE*2, "%s,%lld,%.9ld\n",
                            pbuf,
                            (long long) recvtime.tv_sec, recvtime.tv_nsec);
                    p->lbuf_offset += m;
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
        n = mtcp_epoll_wait (mctx, p->ep, events, MAXEVENTS, 1);

        if (n < 0) {
            perror ("epoll_wait");
            return -1;
        }
        
        debug_print (p, DEBUG, "Got %d events\n", n);

        for (i = 0; i < n; i++) {
            // Check for errors
            if ((events[i].events & MTCP_EPOLLERR) ||
                    (events[i].events & MTCP_EPOLLHUP) ||
                    (events[i].events & !MTCP_EPOLLIN)) {
                id_print (p, "epoll error!\n");
                return -1;
            } else if (events[i].data.sockid == p->servicefd) {
                // Someone is trying to connect; ignore. All clients should have
                // connected already.
                continue;
            } else {
                // There's data to be read
                to_write = 0;
                memset (rcv_buf, 0, PSIZE);

                while (1) {
                    j = mtcp_read (mctx, events[i].data.sockid, rcv_buf + to_write,
                            PSIZE - to_write);
                    if (j <= 0) {
                        break;
                    }
                    to_write += j;
                }

                // Just in case
                if (to_write == 0) {
                    printf ("towrite\n");
                    return -1;
                }

                if (j == 0) {
                    // Other side is disconnected
                    perror ("server read");
                    return -1;
                } else if (j < 0) {
                    if (errno == EAGAIN) {
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
                            printf ("Had to loop, to_write=%d, written=%d...\n",
                                    to_write, written);
                        }

                        // Just in case
                        if (written == 0) {
                            printf ("written\n");
                            return -1;
                        }

                        (p->counter)++;
                    } else {
                        perror ("server read2");
                        return -1;
                    }
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
    struct sockaddr_in *lsin1, *lsin2;
    char *host;

    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s;
    char portno[7]; 

    struct protoent *proto;
    int socket_family = AF_INET;

    host = p->host;

    // To resolve a hostname
    memset (&hints, 0, sizeof (struct addrinfo));
    hints.ai_family     = socket_family;
    hints.ai_socktype   = SOCK_STREAM;

    lsin1 = &(p->prot.sin1);
    lsin2 = &(p->prot.sin2);

    memset ((char *) lsin1, 0, sizeof (*lsin1));
    memset ((char *) lsin2, 0, sizeof (*lsin2));

    sprintf (portno, "%d", p->port);

    if (!(proto = getprotobyname ("tcp"))) {
        printf ("tester: protocol 'tcp' unknown!\n");
        exit (555);
    }

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

        p->ep = mtcp_epoll_create (mctx, 1);

        s = getaddrinfo (host, portno, &hints, &result);
        if (s != 0) {
            printf ("getaddrinfo: %s\n", gai_strerror (s));
            exit (-10);
        }

        for (rp = result; rp != NULL; rp = rp->ai_next) {
            sockfd = mtcp_socket (mctx, rp->ai_family, rp->ai_socktype, 
                    rp->ai_protocol);
            if (sockfd == -1) {
                continue;
            }

            if (mtcp_connect (mctx, sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
                break;
            }

            mtcp_close (mctx, sockfd);
        }

        if (rp == NULL) {
            printf ("Invalid address %s and/or portno %s! Exiting...\n",
                    host, portno);
            mtcp_destroy_context (mctx);
            exit (-10);
        }

        // I think this is obligatory...
        mtcp_setsock_nonblock (mctx, sockfd);

        struct timeval tv;
        tv.tv_sec = READTO;
        tv.tv_usec = 0;
        if (mtcp_setsockopt (mctx, sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,
                    sizeof tv) < 0) {
            printf ("tester: client: SO_RCVTMEO failed! errno=%d\n", errno);
            exit (-7);
        }

        freeaddrinfo (result);
        lsin1->sin_port = htons (p->port);
        p->commfd = sockfd;
    } else {
        // Every server should open a listening socket; any connected client will
        // only communicate with that server
        lsin1->sin_family       = AF_INET;
        lsin1->sin_addr.s_addr  = htonl (INADDR_ANY);
        lsin1->sin_port         = htons (p->port);

        if (!p->prot.mctx) {
            printf ("core %d\n", core);
            mtcp_core_affinitize (core);
            p->prot.mctx = mtcp_create_context (core);
        }

        if (!p->prot.mctx) {
            printf ("tester: can't create mTCP context!");
            exit (-4);
        }

        mctx_t mctx = p->prot.mctx;

        p->ep = mtcp_epoll_create (mctx, MAXEVENTS);

        if ((sockfd = mtcp_socket (mctx, socket_family, MTCP_SOCK_STREAM, 0)) < 0) {
            printf ("tester: can't open stream socket!\n");
            exit (-4);
        }

        // Must be done manually
        mtcp_setsock_nonblock (mctx, sockfd);

        if (mtcp_bind (mctx, sockfd, (struct sockaddr *) lsin1, sizeof (*lsin1)) < 0) {
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
    struct protoent *proto;
    int nevents, i;
    struct mtcp_epoll_event events[MAXEVENTS];
    struct mtcp_epoll_event event;
    int connections = 0;
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
                    char hostbuf[NI_MAXHOST], portbuf[NI_MAXSERV];
                    
                    p->commfd = mtcp_accept (mctx, p->servicefd, 
                            (struct sockaddr *) &(p->prot.sin2), &clen);

                    if (p->commfd == -1) {
                       if (errno == EAGAIN) {
                           break;
                       } else {
                           perror ("accept");
                           break;
                       }
                    }

                    getnameinfo ((struct sockaddr *) &p->prot.sin2, 
                            clen, hostbuf, sizeof (hostbuf),
                            portbuf, sizeof (portbuf),
                            NI_NUMERICHOST | NI_NUMERICSERV);
                                        
                    if (!(proto = getprotobyname ("tcp"))) {
                        printf ("unknown protocol!\n");
                        exit (555);
                    }

                    // Set socket to nonblocking
                    if (mtcp_setsock_nonblock (mctx, p->commfd) < 0) {
                        printf ("Error setting socket to non-blocking!\n");
                        continue;
                    }

                    // Add descriptor to epoll instance
                    event.data.sockid = p->commfd;
                    event.events = MTCP_EPOLLIN | MTCP_EPOLLET;  
                    if (mtcp_epoll_ctl (mctx, p->ep, MTCP_EPOLL_CTL_ADD, 
                                p->commfd, &event) < 0) {
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

    if (p->tr) {
        mtcp_close (mctx, p->commfd);
    } else {
        mtcp_close (mctx, p->servicefd);
    }

    id_print (p, "Closed sockets\n");

    // mtcp_destroy_context (mctx);
    id_print (p, "Finished cleaning up\n");
}
