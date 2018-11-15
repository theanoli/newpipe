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

        // if (pargs->tr) {
        //     // E.g., if there are 8 server threads but only 4 cores, and 16
        //     // client threads, then want to cycle through portnos 8000-8003 only.
        //     // If there are 2 server threads with 4 cores, and 16 client threads, 
        //     // cycle through portnos 8000-8001 only.
        //     targs[i].port = pargs->port + i % pargs->ncli % NSERVERCORES; 
        // } else {
        //     targs[i].port = pargs->port + i % NSERVERCORES;
        // }
        targs[i].port = pargs->port;
        targs[i].host = pargs->host;
        targs[i].tr = pargs->tr;
        targs[i].latency = pargs->latency;

        // For servers, this is how many clients from which to expect connections.
        // For clients, this is how many total server threads there are. 
        targs[i].ncli = pargs->ncli;
        targs[i].ep = -1;
        targs[i].no_record = pargs->no_record;

        if (pargs->no_record) {
            printf ("Not recording measurements to file.\n");
        } else {    
            setup_filenames (&targs[i]);
        }

        printf ("[%s] Launching thread %d, ", pargs->machineid, i);
        printf ("connecting to portno %d\n", targs[i].port);

        pthread_create (&pargs->tids[i], NULL, ThreadEntry, (void *)&targs[i]);
        
        // Always pin cores
        // This is fragile; better to get available cores programmatically
        // instead of using hardcoded macro value NSERVERCORES
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


// Entry point for new threads. Actually do the work.
void *
ThreadEntry (void *vargp)
{
    ThreadArgs *p = (ThreadArgs *)vargp;
    
    if (p->tr) {
        Setup (p);
        TimestampTxRx (p);
    } else {
        if (p->threadid == 0) {
            Setup (p);
        }

        Echo (p);

        printf ("\n++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
        printf ("%s Received %" PRIu64 " packets in %f seconds\n", 
                    p->threadname, p->counter, p->duration);
        printf ("Throughput is %f pps\n", p->counter/p->duration);
        printf ("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");

        p->pps = p->counter/p->duration;
        if (!p->no_record) {
            record_throughput (p);
        }

        CleanUp (p);
    }

    return 0;
}


void 
TimestampTxRx (ThreadArgs *p)
{
    // Send and then receive an echoed timestamp.
    // Return a pointer to the stored timestamp. 
    char pbuf[PSIZE];  // for packets
    char wbuf[PSIZE * 2];  // for send,rcv time, to write to file
    int n, m;
    int i;
    uint64_t count = 0;  // packet counter for sampling
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

    printf ("[%s] Getting ready to send from thread %d, ", p->machineid, i);
    for (i = 0; i < p->nrtts; i++) {
        sendtime = PreciseWhen ();
        snprintf (pbuf, PSIZE, "%lld,%.9ld%-31s",
                (long long) sendtime.tv_sec, sendtime.tv_nsec, ",");

        n = write (p->commfd, pbuf, PSIZE - 1);
        if (n < 0) {
            perror ("write");
            exit (1);
        }

        debug_print (DEBUG, "pbuf: %s, %d bytes written\n", pbuf, n);

        memset (pbuf, 0, PSIZE);
        
        n = read (p->commfd, pbuf, PSIZE);
        if (n < 0) {
            perror ("read");
            exit (1);
        }

        recvtime = PreciseWhen ();        

        if ((!p->no_record) && (count % 100 == 0)) {
            memset (wbuf, 0, PSIZE * 2);
            m = snprintf (wbuf, PSIZE, "%s", pbuf);
            snprintf (wbuf + m, PSIZE, "%lld,%.9ld\n", 
                    (long long) recvtime.tv_sec, recvtime.tv_nsec);

            fwrite (wbuf, strlen (wbuf), 1, out);
        }

        debug_print (DEBUG, "Got timestamp: %s, %d bytes read\n", pbuf, n);
    }

}


void
Echo (ThreadArgs *p)
{
    // Server-side only!
    // Loop through for expduration seconds and count each packet you send out
    // Start counting packets after a few seconds to stabilize connection(s)
    int j, n, i, done;
    struct epoll_event events[MAXEVENTS];

    p->counter = 0;

    // Add a few-second delay to let the clients stabilize
    while (p->program_state == startup) {
        // Should never be in this status in this function, 
        // but just in case...
        // sppppiiiinnnnnn
    }

    printf ("%s Entering packet receive mode...\n", p->threadname);

    while (p->program_state != end) {
        n = epoll_wait (p->ep, events, MAXEVENTS, 1);

        if (n < 0) {
            if (n == EINTR) {
                perror ("epoll_wait: echo intr:");
            }
            // exit (1);
        }
        
        debug_print (DEBUG, "Got %d events\n", n);

        for (i = 0; i < n; i++) {
            // Check for errors
            if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    !(events[i].events & EPOLLIN)) {
                printf ("epoll error!\n");
                close (events[i].data.fd);
                if (p->latency) {
                    exit (1);
                } else {
                    continue;
                }
            } else if (events[i].data.fd == p->servicefd) {
                // Someone is trying to connect; ignore. All clients should have
                // connected already.
                continue;
            } else {
                // There's data to be read
                done = 0;
                int to_write;
                int written;
                char rcv_buf[PSIZE];
                char *q;

                // This is dangerous because rcv_buf is only PSIZE bytes long
                // TODO figure this out
                q = rcv_buf;

                while ((j = read (events[i].data.fd, q, PSIZE)) > 0) {
                    q += j;
                }
                
                if (errno != EAGAIN) {
                    if (j < 0) {
                        perror ("server read");
                    }
                    done = 1;  // Close this socket
                } else {
                    // We've read all the data; echo it back to the client
                    to_write = q - rcv_buf;
                    written = 0;
                    
                    while ((j = write (events[i].data.fd, 
                                    rcv_buf + written, to_write) + written) < to_write) {
                        if (j < 0) {
                            if (errno != EAGAIN) {
                                perror ("server write");
                                done = 1;
                                break;
                            }
                        }
                        written += j;
                        printf ("Had to loop...\n");
                    }

                    if ((p->program_state == experiment) && !done) {
                        (p->counter)++;
                    } 
                }

                if (done) {
                    printf ("Got an error!\n");
                    close (events[i].data.fd);
                }
            }
        }
    }

    p->tput_done = 1;
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

    lsin1 = &(p->prot.sin1);
    lsin2 = &(p->prot.sin2);
    
    memset ((char *) lsin1, 0, sizeof (*lsin1));
    memset ((char *) lsin2, 0, sizeof (*lsin2));
    sprintf (portno, "%d", p->port);
    
    p->ep = epoll_create (MAXEVENTS);

    flags = SOCK_STREAM;
    if (!p->tr) {
        flags |= SOCK_NONBLOCK;
    } 

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

        freeaddrinfo (result);
        lsin1->sin_port = htons (p->port);
        p->commfd = sockfd;

    } else {
        if ((sockfd = socket (socket_family, flags, 0)) < 0) {
            printf ("tester: can't open stream socket!\n");
            exit (-4);
        }

        int enable = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
            printf ("tester: server: SO_REUSEADDR failed! errno=%d\n", errno);
            exit (-7);
        }
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0) {
            printf ("tester: server: SO_REUSEPORT failed! errno=%d\n", errno);
            exit (-7);
        }

        memset ((char *) lsin1, 0, sizeof (*lsin1));
        lsin1->sin_family       = AF_INET;
        lsin1->sin_addr.s_addr  = htonl (INADDR_ANY);
        lsin1->sin_port         = htons (p->port);

        if (bind (sockfd, (struct sockaddr *) lsin1, sizeof (*lsin1)) < 0) {
            printf ("tester: server: bind on local address failed! errno=%d\n", errno);
            exit (-6);
        }

        p->servicefd = sockfd;
    }

    establish (p);    
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
    double t0, duration;
    int connections = 0;
    int report_connections = 1; 

    clen = (socklen_t) sizeof (p->prot.sin2);
    
    if (!p->tr) {
        // Server waits for incoming connections
        event.events = EPOLLIN;
        event.data.fd = p->servicefd;

        if (epoll_ctl (p->ep, EPOLL_CTL_ADD, p->servicefd, &event) == -1) {
            perror ("epoll_ctl");
            exit (1);
        }

        listen (p->servicefd, 1024);

        printf ("\tStarting loop to wait for connections...\n");

        t0 = When ();
        while (p->program_state == startup) {
            duration = STARTUP - (When() - t0);
            if ((connections == p->ncli) && report_connections)  {
                printf ("++++++Got all the connections...\n");
                report_connections = 0;
            }

            nevents = epoll_wait (p->ep, events, MAXEVENTS, duration); 
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
                        event.events = EPOLLIN;  // TODO check this

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
    }

    printf ("Setup complete... getting ready to start experiment\n");
}


void
CleanUp (ThreadArgs *p)
{
    printf ("%s Quitting!\n", p->threadname);
   
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


