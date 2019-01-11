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
}


void
LaunchThreads (ProgramArgs *pargs)
{
    int i, ret;
    int nprocs_onln;
    cpu_set_t cpuset;

    ThreadArgs *targs = pargs->thread_data;

    for (i = 0; i < pargs->nthreads; i++) {
        targs[i].machineid = pargs->machineid;
        targs[i].threadid = i;
        snprintf (targs[i].threadname, 128, "[%s.%d]", pargs->machineid, i);

        // All clients will connect to the same server port
        targs[i].port = pargs->port;
        targs[i].host = pargs->host;
        targs[i].tr = pargs->tr;
        targs[i].program_state = startup;

        // For servers, this is how many clients from which to expect connections.
        // For clients, this is how many total server threads there are. 
        targs[i].ncli = pargs->ncli;
        targs[i].ep = -1;
        targs[i].no_record = pargs->no_record;
        targs[i].counter = 0;

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
        
        if (pargs->pin_threads) {
            nprocs_onln = get_nprocs ();
            CPU_ZERO (&cpuset);
            CPU_SET (i % nprocs_onln, &cpuset);
            ret = pthread_setaffinity_np (pargs->tids[i], sizeof (cpu_set_t), &cpuset);
            if (ret != 0) {
                printf ("[%s] Couldn't pin thread %d to core!\n", pargs->machineid, i);
                exit (-14);
            }
        }
    }
}


void 
TimestampTxRx (ThreadArgs *p)
{
    // Send and then receive an echoed timestamp.
    // Return a pointer to the stored timestamp. 
    char pbuf[PSIZE];  // for packets
    int n, m;
    struct timespec sendtime, recvtime;
    FILE *out;

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
    
    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {
        sendtime = PreciseWhen ();
        snprintf (pbuf, PSIZE, "%lld,%.9ld",
                (long long) sendtime.tv_sec, sendtime.tv_nsec);

        n = write (p->commfd, pbuf, PSIZE - 1);
        if (n < 0) {
            perror ("client write");
            exit (1);
        }

        debug_print (p, DEBUG, "pbuf: %s, %d bytes written\n", pbuf, n);

        memset (pbuf, 0, PSIZE);
        
        n = read (p->commfd, pbuf, PSIZE);
        if (n < 0) {
            perror ("client read");
            exit (1);
        }

        recvtime = PreciseWhen ();        
        (p->counter)++;

        if ((!p->no_record) && (p->counter % 1000 == 0)) {
            if (p->lbuf_offset > (LBUFSIZE - 41)) {
                // Flush the buffer to file
                // 41 is longest possible write
                fwrite (p->lbuf, 1, p->lbuf_offset, out);
                memset (p->lbuf, 0, LBUFSIZE);
                p->lbuf_offset = 0;
            } else {
                // There's more capacity in the buffer
                m = snprintf (p->lbuf + p->lbuf_offset, PSIZE*2, "%s,%lld,%.9ld\n", 
                        pbuf,
                        (long long) recvtime.tv_sec, recvtime.tv_nsec);
                p->lbuf_offset += m;
            }
        }

        debug_print (p, DEBUG, "Got timestamp: %s, %d bytes read\n", pbuf, n);
    }
}


void
Echo (ThreadArgs *p)
{
    // Server-side only!
    int j, n, i, done;
    int nconnections = p->ncli; 
    struct epoll_event events[MAXEVENTS];
    int to_write;
    int written;
    char rcv_buf[PSIZE];

    // Add a few-second delay to let the clients stabilize
    while (p->program_state == startup) {
        // Should never be in this status in this function, 
        // but just in case...
        // sppppiiiinnnnnn
    }

    // id_print (p, "Entering packet receive mode...\n");

    while (p->program_state != end) {
        n = epoll_wait (p->ep, events, MAXEVENTS, 1);

        if (n < 0) {
            if (n == EINTR) {
                perror ("epoll_wait: echo intr:");
            }
            // exit (1);
        }
        
        debug_print (p, DEBUG, "Got %d events\n", n);

        for (i = 0; i < n; i++) {
            // Check for errors 
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    (events[i].events & !EPOLLIN)) {
                printf ("epoll error!\n");
                close (events[i].data.fd);
            } else if (events[i].data.fd == p->servicefd) {
                // Someone is trying to connect; ignore. All clients should have
                // connected already.
                continue;
            } else if (events[i].events & EPOLLIN) {
                // There's data to be read
                done = 0;
                to_write = 0;
                memset (rcv_buf, 0, PSIZE);

                while (1) {
                    j = read (events[i].data.fd, rcv_buf + to_write, 
                                PSIZE - to_write);
                    if (j <= 0) {
                        break;
                    }
                    to_write += j;
                }

                // Just in case
                if (to_write == 0) {
                    printf ("towrite\n");
                    exit (-122);
                }

                if (j == 0) {
                    // The other side is disconnected
                    perror ("server read");
                    done = 1;  // Close this socket
                    nconnections--; 
                } else if (j < 0) {
                    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                        // We've read all the data; echo it back to the client
                        written = 0;

                        while (1) {
                            j = write (events[i].data.fd, rcv_buf + written, to_write - written);
                            if (j < 0) {
                                if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
                                    perror ("server write");
                                    done = 1;
                                    nconnections--;
                                }
                                break;
                            }
                            written += j;
                            if (to_write == written) {
                                break;
                            }
                            printf ("Had to loop, to_write=%d, written=%d...\n", to_write, written);
                        }

                        // Just in case
                        if (written == 0) {
                            printf ("written\n");
                            exit (-122);
                        }

                        if (!done) {
                            (p->counter)++;
                        } 
                    } else {
                        perror ("server read2");
                        done = 1; 
                        nconnections--;
                    }
                }

                if (done) {
                    close (events[i].data.fd);
                    if (nconnections <= 0) {
                        printf ("No more connections left! Exiting...\n");
                        exit (-122);
                    }
                }
            }
        }
    }
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
    int flags;

    host = p->host;

    // To resolve a hostname
    memset (&hints, 0, sizeof (struct addrinfo));
    hints.ai_family     = socket_family;
    hints.ai_socktype   = SOCK_STREAM;

    lsin1 = &(p->prot.sin1);  // my info
    lsin2 = &(p->prot.sin2);  // remote info
    
    memset ((char *) lsin1, 0, sizeof (*lsin1));
    memset ((char *) lsin2, 0, sizeof (*lsin2));

    sprintf (portno, "%d", p->port);  // the port client will connect to

    
    if (!(proto = getprotobyname ("tcp"))) {
        printf ("tester: protocol 'tcp' unknown!\n");
        exit (555);
    }

    if (p->tr) {
        s = getaddrinfo (host, portno, &hints, &result);
        if (s != 0) {
            printf ("%s\n", gai_strerror (s)); 
            exit (-10);
        }

        for (rp = result; rp != NULL; rp = rp->ai_next) {
            sockfd = socket (rp->ai_family, rp->ai_socktype,
                    rp->ai_protocol);
            if (sockfd == -1) {
                continue;
            }

            if (connect (sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
                break;
            }

            close (sockfd);
        }

        if (rp == NULL) {
            printf ("Invalid address %s and/or portno %s! Exiting...\n", host, portno);
            exit (-10);
        }

        struct timeval tv;
        tv.tv_sec = READTO;
        tv.tv_usec = 0;
        if (setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
            printf ("tester: client: SO_RCVTIMEO failed! errno=%d\n", errno);
            exit (-7);
        }
        
        freeaddrinfo (result);
        lsin1->sin_port = htons (p->port);
        p->commfd = sockfd;

    } else {
        lsin1->sin_family       = AF_INET;
        lsin1->sin_addr.s_addr  = htonl (INADDR_ANY);
        lsin1->sin_port         = htons (p->port);  // the port server will bind to

        p->ep = epoll_create (MAXEVENTS);

        flags = SOCK_STREAM | SOCK_NONBLOCK;

        if ((sockfd = socket (socket_family, flags, 0)) < 0) {
            printf ("tester: can't open stream socket!\n");
            exit (-4);
        }

        int enable = 1;
        if (setsockopt (sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
            printf ("tester: server: SO_REUSEADDR failed! errno=%d\n", errno);
            exit (-7);
        }
        if (setsockopt (sockfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0) {
            printf ("tester: server: SO_REUSEPORT failed! errno=%d\n", errno);
            exit (-7);
        }

        if (bind (sockfd, (struct sockaddr *) lsin1, sizeof (*lsin1)) < 0) {
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
    struct epoll_event events[MAXEVENTS];
    struct epoll_event event;
    int connections = 0;

    clen = (socklen_t) sizeof (p->prot.sin2);
    
    // Server waits for incoming connections
    event.events = EPOLLIN;
    event.data.fd = p->servicefd;

    if (epoll_ctl (p->ep, EPOLL_CTL_ADD, p->servicefd, &event) == -1) {
        perror ("epoll_ctl");
        exit (1);
    }

    listen (p->servicefd, 1024);

    id_print (p, "Starting loop to wait for connections...\n");

    while (p->program_state == startup) {
        nevents = epoll_wait (p->ep, events, MAXEVENTS, 0); 
        if (nevents < 0) {
            if (errno != EINTR) {
                perror ("establish: epoll_wait");
            }
            exit (1);
        }

        for (i = 0; i < nevents; i++) {
            if (events[i].data.fd == p->servicefd) {
                while (1) {
                    char hostbuf[NI_MAXHOST], portbuf[NI_MAXSERV];
                    
                    p->commfd = accept (p->servicefd, 
                            (struct sockaddr *) &(p->prot.sin2), &clen);

                    if (p->commfd == -1) {
                       if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
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
                    if (setsock_nonblock (p->commfd) < 0) {
                        printf ("Error setting socket to non-blocking!\n");
                        continue;
                    }

                    // Add descriptor to epoll instance
                    event.data.fd = p->commfd;
                    event.events = EPOLLIN | EPOLLET;  // TODO check this

                    if (epoll_ctl (p->ep, EPOLL_CTL_ADD, p->commfd, &event) < 0) {
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

    // Record the actual number of successful connections
    p->ncli = connections;
    id_print (p, "Got %d connections\n", connections);
}


void
CleanUp (ThreadArgs *p)
{
    close (p->commfd);

    if (!p->tr) {
        close (p->servicefd);
        close (p->ep);
    }
}


int
setsock_nonblock (int fd)
{
    int flags;

    flags = fcntl (fd, F_GETFL, 0);
    if (flags == -1) {
        perror ("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;
    if (fcntl (fd, F_SETFL, flags) == -1) {
        perror ("fcntl");
        return -1;
    }

    return 0;
}


