#include "harness.h"

#define DEBUG 0
#define DEBUG_RECVMMSG 0
#define READTO 10000  // in usec

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

        targs[i].port = pargs->port;
        targs[i].host = pargs->host;
        targs[i].tr = pargs->tr;

        targs[i].no_record = pargs->no_record;

        if (pargs->no_record) {
            printf ("Not recording measurements to file.\n");
        } else {
            setup_filenames (&targs[i]);
            if (pargs->tr) {
                // Create a buffer for latency measurements; will dump
                // to file when we've collected a batch
                targs[i].lbuf = (char *) malloc (LBUFSIZE);
                if (targs[i].lbuf == NULL) {
                    printf ("Couldn't allocate space for latency buffer!"
                            "Exiting...\n");
                    exit (-13);
                }
                targs[i].lbuf_offset = 0;
                memset (targs[i].lbuf, 0, LBUFSIZE);
            }
        }

        fflush (stdout);

        pthread_create (&pargs->tids[i], NULL, ThreadEntry, (void *)&targs[i]);
        
        if (pargs->pin_threads) {
            nprocs_onln = get_nprocs ();
            CPU_ZERO (&cpuset);
            CPU_SET (i % nprocs_onln, &cpuset);
            ret = pthread_setaffinity_np (pargs->tids[i], sizeof (cpu_set_t), 
                    &cpuset);
            if (ret != 0) {
                printf ("[%s] Couldn't pin thread %d to core!\n", 
                        pargs->machineid, i);
                exit (-14);
            }
        }
    }
}


int 
TimestampTxRx (ThreadArgs *p)
{
    // Send then receive an echoed timestamp in a loop until 
    // we encounter an error or the experiment period ends. 
    // Return a negative number if error, else 0.
    
    int n;
    char sbuf[PSIZE];  // for sending
    char rbuf[PSIZE];  // for receiving

    struct timespec sendtime, recvtime;
    FILE *out;
    struct sockaddr_in *remote = &(p->prot.sin1);
    socklen_t len = sizeof (*remote);

    if (p->tr && !p->no_record) {
        if ((out = fopen (p->latency_outfile, "wb")) == NULL) {
            fprintf (stderr, "Can't open %s for output!\n", p->latency_outfile);
            return -1;
        }
    } else {
        out = stdout;
    }

    while (p->program_state != experiment) {
    }

    while (p->program_state == experiment) { 
        memset (sbuf, 0, PSIZE);
        memset (rbuf, 0, PSIZE);

        sendtime = PreciseWhen ();
        snprintf (sbuf, PSIZE, "%lld,%.9ld",
                (long long) sendtime.tv_sec, sendtime.tv_nsec);

        n = sendto (p->commfd, sbuf, PSIZE, 0,
            (struct sockaddr *) remote, len); 
        if (n < 0) {
            perror ("client write");
            return -1;
        }

        debug_print (p, DEBUG, "sbuf: %s, %d bytes written\n", sbuf, n);
        
        // If the recvfrom times out (ret. -1), resend the packet and go 
        // through the loop again 
        while ((n = recvfrom (p->commfd, rbuf, PSIZE, 0, 
                (struct sockaddr *) remote, &len)) == -1) {
            p->retransmits++;
            n = sendto (p->commfd, sbuf, PSIZE, 0, 
                    (struct sockaddr *) remote, len);
            if (n < 0) {
                perror ("client write");
                return -1;
            }
        }
        recvtime = PreciseWhen ();        
        p->counter++;

        if (n == 0) {
            id_print (p, "Peer disconnected! Exiting...\n");
            return -1;
        }

        if ((!p->no_record) && (p->counter % 1000 == 0)) {
            WriteLatencyData (p, out, rbuf, &recvtime);
        }
        debug_print (p, DEBUG, "Got timestamp: %s, %d bytes read\n", rbuf, n);
    }

    return 0;
}


int
Echo (ThreadArgs *p)
{
    // Server-side only!
    // Loop through for expduration seconds and count each packet you send out
    // Start counting packets after a few seconds to stabilize connection(s)
    int i, m, n; 

    struct mmsghdr msgs[MAXEVENTS];      // headers
    struct iovec iovecs[MAXEVENTS];        // packet info
    char bufs[MAXEVENTS][PSIZE + 1];            // packet buffers
    struct sockaddr_in addrs[MAXEVENTS];    // return addresses

    for (i = 0; i < MAXEVENTS; i++) {
        iovecs[i].iov_base          = bufs[i];
        iovecs[i].iov_len           = PSIZE;
        msgs[i].msg_hdr.msg_iov     = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen  = 1;
        msgs[i].msg_hdr.msg_name    = &addrs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof (addrs[i]);
    }
    
    p->counter = 0;

    // Wait for state to get updated to warmup
    while (p->program_state == startup) {
        // Sppppinnnnnnn
    }

    // Max time to wait for messages to accumulate: 1ms
    struct timespec timeout = { .tv_nsec = 1000 * 1000UL, };
    id_print (p, "Entering packet receive mode, sockfd %d...\n",
            p->commfd);

    while (p->program_state != end) { 
        // Read data from client; m is number of messages received 
        m = recvmmsg (p->commfd, msgs, MAXEVENTS, MSG_WAITFORONE, &timeout);
        debug_print (p, DEBUG_RECVMMSG, "Got %d messages.\n", m);

        if (m < 0) {
            perror ("server read");
            exit (1);
        }

        // Process each of the messages in msgvec
        for (i = 0; i < m; i++) {
            // Echo data back to client
            // bufs[i][msgs[i].msg_len] = 0;
            debug_print (p, DEBUG_RECVMMSG, "Got a packet! Contents: %s, len %d\n", 
                    bufs[i], msgs[i].msg_len);
            n = sendto (p->commfd, bufs[i], PSIZE, 0, 
                    (struct sockaddr *) &addrs[i], msgs[i].msg_hdr.msg_namelen);
            if (n < 0) {
                perror ("server write");
                exit (1);
            }
        }

        p->counter += m;
    }

    return 0;
}


// Entry point for new threads
void
Setup (ThreadArgs *p)
{
    if (!p->tr && p->threadid != 0) {
        return;
    }

    int sockfd;
    struct sockaddr_in *lsin;

    int ret;

    lsin = &(p->prot.sin1);
    memset ((char *) lsin, 0, sizeof (*lsin));

    if (p->tr) {
        lsin->sin_family = AF_INET;
        lsin->sin_addr.s_addr = inet_addr (p->host);
        lsin->sin_port = htons (p->port);

        sockfd = socket (AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            id_print (p, "Failed to create socket!\n");
            exit (-10);
        }

        ret = connect (sockfd, (struct sockaddr *)lsin,
                sizeof (struct sockaddr));
        if (ret < 0) {
            if (errno != EINPROGRESS) {
                perror ("client connect");
                exit (-1);
            }
        }

        id_print (p, "successfully made contact on port %d!\n", p->port);

        struct timeval read_timeout;
        read_timeout.tv_sec = 0;
        read_timeout.tv_usec = READTO;
        setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout,
                sizeof (read_timeout));

        p->commfd = sockfd;

    } else {
        // Receiver side
        sockfd = socket (AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            printf ("server: can't open stream socket!\n");
            exit (-4);
        }

        lsin->sin_family       = AF_INET;
        lsin->sin_addr.s_addr  = htonl (INADDR_ANY);
        lsin->sin_port         = htons (p->port);

        ret = bind (sockfd, (struct sockaddr *) lsin, 
                sizeof (*lsin));
        if (ret < 0) {
            perror ("server: bind on local address failed!"
                    "errno");
            exit (-6);
        }

        p->commfd = sockfd;
        
        // For UDP, this will let multiple processes to bind to the same port;
        // here we want it so we can immediately reuse the same socket
        int one = 1;
        if (setsockopt (p->commfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (int))) {
            perror ("tester: server: unable to setsockopt!");
            exit (557);
        }

        id_print (p, "Established a connection, sockfd %d...\n", p->commfd);
        fflush (stdout);

        while (p->program_state == startup) {
            // spin
        }
    }
}


void 
CleanUp (ThreadArgs *p)
{

   printf ("%s Quitting!\n", p->threadname);

   close (p->commfd);

}

