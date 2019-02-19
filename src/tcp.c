#include "harness.h"

#define READTO 0

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
        targs[i].nports = pargs->nports;
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


int
TimestampTxRx (ThreadArgs *p)
{
    // Send and then receive an echoed timestamp.
    // Return a negative value if error, else return 0.
    int i, j, n;
    struct epoll_event events[MAXEVENTS_CLI];
    int nread; 
    int written;
    char pbuf[PSIZE];  // for packets

    struct timespec sendtime, recvtime;
    FILE *out;

    // Open file for recording latency data
    if (p->tr && (!p->no_record)) {
        if ((out = fopen (p->latency_outfile, "wb")) == NULL) {
            fprintf (stderr, "Can't open %s for output!\n", p->latency_outfile);
            return -1;
        }
    } else {
        out = stdout;
    }

    while (p->program_state != experiment) {
    }
    
    // Send initial message to get things started
    sendtime = PreciseWhen ();
    snprintf (pbuf, PSIZE, "%lld,%.9ld",
            (long long) sendtime.tv_sec, sendtime.tv_nsec);
    for (i = 0; i < p->nports; i++) {
        j = write (p->nports_array[i], pbuf, PSIZE - 1);
        if (j < 0) {
            perror ("client write");
            return -1;
        }

        n = setsock_nonblock (p->nports_array[i]);
        if (n < 0) {
            id_print (p, "Couldn't setsock to nonblock!\n");
            return -1;
        }
    }
    
    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {
        n = epoll_wait (p->ep, events, MAXEVENTS_CLI, 1);

        if (n < 0) {
            perror ("epoll_wait");
            exit (-1);
        }

        for (i = 0; i < n; i++) {
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    (events[i].events & !EPOLLIN)) {
                // Error; exit
                id_print (p, "epoll error!\n");
                return -1;
            } else if (events[i].events & EPOLLIN) {
                // Read data and dump to file if needed
                nread = 0;
                memset (pbuf, 0, PSIZE);
                
                while (1) {
                    j = read (events[i].data.fd, pbuf + nread, 
                            PSIZE - nread);
                    if (j <= 0) {
                        break;
                    }
                    nread += j;
                }

                if (j == 0) {
                    // Other side is disconnected
                    perror ("client read");
                    return -1;
                } else if (j < 0) {
                    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {

                        recvtime = PreciseWhen ();
                        (p->counter)++;

                        if ((!p->no_record) && (p->counter % 1000 == 0)) {
                            WriteLatencyData (p, out, pbuf, &recvtime);
                        }

                        debug_print (p, DEBUG, 
                                "Got timestamp: %s, %d bytes read\n", pbuf, nread);
                    } else {
                        perror ("client read2");
                        return -1;
                    }
                }
                
                // Send a timestamp
                written = 0;
                memset (pbuf, 0, PSIZE);
                sendtime = PreciseWhen ();

                snprintf (pbuf, PSIZE, "%lld,%.9ld",
                        (long long) sendtime.tv_sec, sendtime.tv_nsec);

                while (1) {
                    j = write (events[i].data.fd, pbuf + written, 
                            PSIZE - written - 1);
                    if (j < 0) {
                        if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
                            perror ("client write");
                            return -1;
                        }
                        break;
                    }
                    written += j;
                    if (written == PSIZE - 1) {
                        break;
                    }
                }
                if (written == 0) {
                    id_print (p, "written\n");
                    return -1;
                }

                debug_print (p, DEBUG, "pbuf: %s, %d bytes written\n", pbuf, written);
            }
        }
    }
    return 0;
}

int
Echo (ThreadArgs *p)
{
    // Server-side only!
    int i, j, n;
    struct epoll_event events[MAXEVENTS];
    int to_write;
    int written;
    char rcv_buf[PSIZE];

    while (p->program_state == startup) {
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
        
        debug_print (p, SERV_DEBUG, "Got %d events\n", n);

        for (i = 0; i < n; i++) {
            // Check for errors 
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    (events[i].events & !EPOLLIN)) {
                printf ("epoll error!\n");
                return -1;
            } else if (events[i].data.fd == p->servicefd) {
                // Someone is trying to connect; ignore. All clients should have
                // connected already.
                continue;
            } else if (events[i].events & EPOLLIN) {
                // There's data to be read
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
                    return -1;
                }

                if (j == 0) {
                    // The other side is disconnected
                    perror ("server read");
                    return -1;
                } else if (j < 0) {
                    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                        // We've read all the data; echo it back to the client
                        written = 0;

                        while (1) {
                            j = write (events[i].data.fd, rcv_buf + written, to_write - written);
                            if (j < 0) {
                                if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
                                    perror ("server write");
                                    return -1;
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
    }
    return 0;
}


void
Setup (ThreadArgs *p)
{
    // Only want to do this for the 0th thread on server
    if (!p->tr && (p->threadid != 0)) {
        return;
    }

    int sockfd; 
    struct sockaddr_in *lsin;
    
    int ret, i, port;

    int flags;

    struct epoll_event event;

    lsin = &(p->prot.sin1);
    memset ((char *) lsin, 0, sizeof (*lsin));
    
    if (p->tr) {
        p->ep = epoll_create (1);

        // Shut up the compiler
        lsin->sin_family = AF_INET;
        lsin->sin_addr.s_addr = inet_addr (p->host);
        lsin->sin_port = htons (port);

        for (i = 0; i < p->nports; i++) {
            lsin->sin_family = AF_INET;
            lsin->sin_addr.s_addr = inet_addr (p->host);
            lsin->sin_port = htons (p->port);

            sockfd = socket (AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                id_print (p, "Failed to create socket!\n");
                exit (-10);
            }

            ret = connect (sockfd, (struct sockaddr *)lsin,
                    sizeof (struct sockaddr));
            if (ret < 0) {
                if (errno != EINPROGRESS) {
                    perror ("connect");
                    exit (-12);
                }
            }

            struct timeval tv;
            tv.tv_sec = READTO;
            tv.tv_usec = 0;
            if (setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
                printf ("tester: client: SO_RCVTIMEO failed! errno=%d\n", errno);
                exit (-7);
            }
            
            event.events = EPOLLIN | EPOLLET;
            event.data.fd = sockfd;
            ret = epoll_ctl (p->ep, EPOLL_CTL_ADD, sockfd, &event);
            if (ret < 0) {
                perror ("epoll_ctl in setup");
                exit (-4);
            }

            memset ((char *) lsin, 0, sizeof (*lsin));
            p->nports_array[i] = sockfd;
        }

    } else {
        lsin->sin_family       = AF_INET;
        lsin->sin_addr.s_addr  = htonl (INADDR_ANY);
        lsin->sin_port         = htons (p->port);  // the port server will bind to

        p->ep = epoll_create (1);

        flags = SOCK_STREAM | SOCK_NONBLOCK;

        if ((sockfd = socket (AF_INET, flags, 0)) < 0) {
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

        if (bind (sockfd, (struct sockaddr *) lsin, sizeof (*lsin)) < 0) {
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


