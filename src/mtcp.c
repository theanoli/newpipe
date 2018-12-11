#include "harness.h"

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#define DEBUG 0
#define WARMUP 3
#define COOLDOWN 5

int doing_reset = 0;
int ep = -1;

mctx_t mctx = NULL;

int mtcp_write_helper (mctx_t, int, char *, int);
int mtcp_read_helper (mctx_t, int, char *, int);

int
mtcp_accept_helper (mctx_t mctx, int sockid, struct sockaddr *addr,
                    socklen_t *addrlen)
{
    int nevents, i;
    struct mtcp_epoll_event events[MAXEVENTS];
    struct mtcp_epoll_event event;

    event.events = MTCP_EPOLLIN;
    event.data.sockid = sockid;

    if (mtcp_epoll_ctl (mctx, ep, MTCP_EPOLL_CTL_ADD, sockid, &event) == -1) {
        perror ("epoll_ctl");
        exit (1);
    }

    while (1) {
        nevents = mtcp_epoll_wait (mctx, ep, events, MAXEVENTS, -1);
        if (nevents < 0) {
            if (errno != EINTR) {
                perror ("mtcp_epoll_wait");
            }
            exit (1);
        }

        // Wait for an incoming connection; return when we get one
        for (i = 0; i < nevents; i++) {
            if (events[i].data.sockid == sockid) {
                // New connection incoming...
                mtcp_epoll_ctl (mctx, ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
                return mtcp_accept (mctx, sockid, addr, addrlen);
            } else {
                printf ("Socket error!\n");
                exit (1);
            }
        }
    }
}


void
Init (ProgramArgs *pargs, int *pargc, char ***pargv)
{
    ThreadArgs *p = (ThreadArgs *)calloc (pargs->nthreads, 
            sizeof (ThreadArgs));
    
    pargs->thread_data = p;

    pargs->tids = (pthread_t *)calloc (pargs->nthreads, sizeof (pthread_t));
    if (pargs->tids == NULL) {
        printf ("Failed to malloc space for tids!\n");
        exit (-82);
    }

    struct mtcp_conf mcfg;
    mtcp_getconf (&mcfg);
    mcfg.num_cores = 1;  // TODO check this
    mtcp_setconf (&mcfg);
	mtcp_init ("mtcp.conf");
}


void
LaunchThreads (ProgramArgs *pargs)
{
    int i, ret;
    cpu_set_t cpuset;
    int nprocs_onln;

    ThreadArgs *targs = pargs->thread_data;

    for (i = 0; i < pargs->nthreads; i++) {
        targs[i].machineid = pargs->machineid;
        targs[i].threadid = i;
        snprintf (targs[i].threadname, 128, "[%s.%d]", pargs->machineid, i);

        // All clients will connect to the same server port
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
        fflush (stdout);

        pthread_create (&pargs->tids[i], NULL, ThreadEntry, (void *)&targs[i]);
        
        // Always pin cores
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


void 
TimestampTxRx (ThreadArgs *p)
{
    // Send and then receive an echoed timestamp.
    // Return a pointer to the stored timestamp. 
    char pbuf[PSIZE];  // for packets
    char wbuf[PSIZE * 2];  // for send,rcv time, to write to file
    int n, m;
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

    while (p->program_state != experiment) {
    }
    
    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {
        sendtime = PreciseWhen ();
        snprintf (pbuf, PSIZE, "%lld,%.9ld%-31s",
                (long long) sendtime.tv_sec, sendtime.tv_nsec, ",");

        n = mtcp_write (mctx, p->commfd, pbuf, PSIZE - 1);
        if (n < 0) {
            perror ("client write");
            exit (1);
        }

        debug_print (p, DEBUG, "pbuf: %s, %d bytes written\n", pbuf, n);

        memset (pbuf, 0, PSIZE);
        
        n = mtcp_read (mctx, p->commfd, pbuf, PSIZE);
        if (n < 0) {
            perror ("client read");
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

        debug_print (p, DEBUG, "Got timestamp: %s, %d bytes read\n", pbuf, n);
    }
}


void
Echo (ThreadArgs *p)
{
    // Server-side only!
    // Loop through for expduration seconds and count each packet you send out
    // Start counting packets after a few seconds to stabilize connection(s)
    int j, n, i, done;
    struct mtcp_epoll_event events[MAXEVENTS];

    p->counter = 0;

    while (p->program_state == startup) {
            // spin
    }

    id_print (p, "Entering packet receive mode...\n");

    while (p->program_state != end) {
        n = mtcp_epoll_wait (mctx, p->ep, events, MAXEVENTS, 1);

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
                    !(events[i].events & EPOLLIN)) {
                printf ("epoll error!\n");
                close (events[i].data.sockid);
                if (p->latency) {
                    exit (1);
                } else {
                    continue;
                }
            } else if (events[i].data.sockid == p->servicefd) {
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

                while ((j = mtcp_read (mctx, events[i].data.sockid, q, PSIZE)) > 0) {
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
                    
                    while ((j = mtcp_write (mctx, events[i].data.sockid, 
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

                    if (!done) {
                        (p->counter)++;
                    } 
                }

                if (done) {
                    printf ("Got an error!\n");
                    close (events[i].data.sockid);
                }
            }
        }
    }

    p->tput_done = 1;
}


// TODO stopped here
void
Setup (ThreadArgs *p)
{
    // Initialize connections: create socket, bind, listen (in establish)
    // Also creates mtcp_epoll instance
    int sockfd;
    struct sockaddr_in *lsin1, *lsin2;
    char *host;

    struct addrinfo hints;
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
    sprintf (portno, "%d", p->port);

    memset ((char *) lsin1, 0, sizeof (*lsin1));
    memset ((char *) lsin2, 0, sizeof (*lsin2));

    if (!mctx) {
        mtcp_core_affinitize (0);
        mctx = mtcp_create_context (0);
        p->ep = mtcp_epoll_create (mctx, MAXEVENTS);
    }

    if (!mctx) {
        printf ("tester: can't create mTCP socket!");
        exit (-4);
    }

    if ((sockfd = mtcp_socket (mctx, socket_family, MTCP_SOCK_STREAM, 0)) < 0) {
        printf ("tester: can't open stream socket!\n");
        exit (-4);
    }

    mtcp_setsock_nonblock (mctx, sockfd);

    if (!(proto = getprotobyname ("tcp"))) {
        printf ("tester: protocol 'tcp' unknown!\n");
        exit (555);
    }

    if (p->tr) {
        if (atoi (host) > 0) {
            lsin1->sin_family = AF_INET;
            lsin1->sin_addr.s_addr = inet_addr (host);
        } else {
            if ((addr = gethostbyname (host)) == NULL) {
                printf ("tester: invalid hostname '%s'\n", host);
                exit (-5);
            }

            lsin1->sin_family = addr->h_addrtype;
            memcpy (addr->h_addr, (char *) &(lsin1->sin_addr.s_addr), addr->h_length);
        }

        struct timeval tv;
        tv.tv_sec = READTO;
        tv.tv_usec = 0;
        if (mtcp_setsockopt (mctx, sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,
                    sizeof tv) < 0) {
            printf ("tester: client: SO_RCVTMEO failed! errno=%d\n", errno);
            exit (-7);
        }

        lsin1->sin_port = htons (p->port);
        p->commfd = sockfd;

    } else {
        // TODO REUSEADDR/REUSEPORT stuff needs to get done here? 
        memset ((char *) lsin1, 0, sizeof (*lsin1));
        lsin1->sin_family       = AF_INET;
        lsin1->sin_addr.s_addr  = htonl (INADDR_ANY);
        lsin1->sin_port         = htons (p->port);

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
    double t0, duration;
    int connections = 0;

    clen = (socklen_t) sizeof (p->prot.sin2);
    
    event.events = MTCP_EPOLLIN;
    event.data.sockid = p->servicefd;

    if (mtcp_epoll_ctl (mctx, ep, MTCP_EPOLL_CTL_ADD, p->servicefd, &event) == -1) {
        perror ("epoll_ctl");
        exit (1);
    }

    mtcp_listen (mctx, p->servicefd, 1024);

    id_print (p, "Starting loop to wait for connections...\n");

    t0 = When ();

    while (p->program_state == startup) {
        duration = STARTUP - (When() - t0);

        nevents = mtcp_epoll_wait (mctx, ep, events, MAXEVENTS, duration); 
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
                    if (mtcp_setsock_nonblock (mctx, p->commfd) < 0) {
                        printf ("Error setting socket to non-blocking!\n");
                        continue;
                    }

                    // Add descriptor to epoll instance
                    event.data.sockid = p->commfd;
                    event.events = MTCP_EPOLLIN | MTCP_EPOLLET;  
                    if (mtcp_epoll_ctl (mctx, ep, MTCP_EPOLL_CTL_ADD, p->commfd, &event) < 0) {
                        perror ("epoll_ctl");
                        exit (1);
                    }

                    connections++;
                    if (!(connections % 50)) {
                        printf ("%d connections so far...\n", connections);
                    }
                }
            } else {
                // Clear the pipe; why is this necessary for mTCP and not TCP?
                if (events[i].events & MTCP_EPOLLIN) {
                    int nread;
                    char buf[128];

                    nread = mtcp_read (mctx, events[i].data.sockid, buf, PSIZE);
                    nread = mtcp_write (mctx, events[i].data.sockid, buf, PSIZE);
                    if (nread) {
                    }
                }
            } 
        }
    }

    printf ("Setup complete... getting ready to start experiment\n");
}


void
CleanUp (ThreadArgs *p)
{
   if (p->tr) {
      mtcp_close (mctx, p->commfd);

   } else {
      mtcp_close (mctx, p->commfd);
      mtcp_close (mctx, p->servicefd);

   }
}

