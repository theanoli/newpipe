/* Use this to run tests with arbitrary protocols; functions
   to establish/tear down connections and to send/receive packets
   will be protocol-specific and will be defined in other files.
*/

#include "harness.h"

#define PRINT_RETRANSMITS 0

extern char *optarg;

// Initialize these here so they are accessible to signal handler
ProgramArgs args;
FILE *out;          /* Output data file                          */

int 
main (int argc, char **argv)
{
    int c, i;
    pthread_t recorder_tid;

    /* Initialize vars that may change from default due to arguments */
    args.no_record = 0;

    // Default initialization to server (not transmitter)
    args.tr = 0;

    signal (SIGINT, SignalHandler);
    signal (SIGTERM, SignalHandler);

    // Machine ID
    args.machineid = (char *)malloc (128);
    gethostname (args.machineid, 128);
    args.machineid = strsep (&args.machineid, ".");

    // Thread-specific arguments
    args.host = NULL;
    args.port = DEFPORT;

    args.program_state = startup;

    /* Parse the arguments. See Usage for description */
    while ((c = getopt (argc, argv, "no:d:H:T:c:P:p:u:li")) != -1)
    {
        switch (c)
        {
            case 'n': args.no_record = 1;
                      break; 

            case 'o': args.outfile = optarg;
                      break;

            case 'd': args.outdir = optarg;
                      break;

            case 'H': args.tr = 1;       /* -H implies transmit node */
                      args.host = (char *) malloc (strlen (optarg) + 1);
                      strcpy (args.host, optarg);
                      break;

            // How many threads to spin up
            case 'T': args.nthreads = atoi (optarg);
                      break;

            // Overloading to be #clients (for server) and #servers for (clients)
    	    case 'c': args.ncli = atoi (optarg);
		              break;

            case 'P': args.port = atoi (optarg);
                      break;

            case 'p': args.nports = atoi (optarg);
                      if (args.nports > MAXNPORTS) {
                          printf ("Number of client connections exceeds max!"
                                  " Exiting...\n");
                          exit (-1);
                      }
                      break;

            case 'u': args.expduration = atoi (optarg);
                      break;

            case 'i': args.pin_threads = 1;
                      break;

            case 'l': args.collect_stats = 1;
                      break; 

            default: 
                     printf ("Got unrecognized option: %c. Exiting!\n", c);
                     exit (-12);
       }
    }
    
    // Initialize variables and whatnot
    printf ("[%s] Initializing...\n", args.machineid);
    Init (&args, &argc, &argv);   

    /* Spin up the specified number of threads, set up network connections */
    LaunchThreads (&args);

    if (args.tr) {
        // Clients (experiment launcher is responsible for waiting long enough
        // for the server threads to start before trying to connect
        // During STARTUP, client threads will be connecting to the server.
        sleep (STARTUP + 2);

        // Throughput recorder thread
        pthread_create (&recorder_tid, NULL, ThroughputRecorder, (void *)&args);

        UpdateProgramState (experiment); // start sending
        sleep (WARMUP + args.expduration + COOLDOWN);
        UpdateProgramState (end);

    } else {
        // Servers

        // Wait a few seconds to let clients come online
        printf ("Waiting for clients to start up and connect...\n"); 
        fflush (stdout);
        sleep (STARTUP);

        printf ("Entering warmup period...\n"); 
        fflush (stdout);

        // Start a recorder thread for throughput
        pthread_create (&recorder_tid, NULL, ThroughputRecorder, (void *)&args);

        // Send epoll or sockfd around to other server threads
        if (!strcmp (whichproto, "TCP")) {
            UpdateFds (1);  // use epollfd
        } else if (!strcmp (whichproto, "UDP")) {
            UpdateFds (0); // use sockfd
        }

        UpdateProgramState (warmup);
        sleep (WARMUP);
        
        printf ("Starting experiment period...\n");
        fflush (stdout);
        UpdateProgramState (experiment);
        sleep (args.expduration);

        printf ("Experiment over, cooling down...\n");
        fflush (stdout);
        UpdateProgramState (cooldown);
        sleep (COOLDOWN);

        printf ("All done!\n");
        fflush (stdout);
        UpdateProgramState (end);
    }

    pthread_join (recorder_tid, NULL);

    for (i = 0; i < args.nthreads; i++) {
        pthread_join (args.tids[i], NULL);
    }

    return 0;
}


// Entry point for new threads. Actually do the work.
void *
ThreadEntry (void *vargp)
{
    ThreadArgs *p = (ThreadArgs *)vargp;
    int ret = 0;
    
    Setup (p);

    if (p->tr) {
        ret = TimestampTxRx (p);
    } else {
        ret = Echo (p);
        id_print (p, "Received %" PRIu64 " packets this thread.\n", 
                    p->counter);
        fflush (stdout);
    }

    if (ret < 0) {
        UpdateProgramState (end);
    }
    CleanUp (p);
    return 0;
}


void *
ThroughputRecorder (void *vargp)
{
    ProgramArgs *args = (ProgramArgs *)vargp;
    
    if ((out = fopen (args->thread_data[0].tput_outfile, "wb")) == NULL) {
        fprintf (stderr, "Can't open throughput file for output!\n");
        return NULL;
    }

    // Do this every (interval) seconds
    while (args->program_state != end) {
        record_throughput (args, out);
        if ((PRINT_RETRANSMITS) && args->tr) {
            aggregate_retransmits (args);
        }
        sleep (1);
    }

    fclose (out);

    return 0;
}


void
aggregate_retransmits (ProgramArgs *args)
{
    int i; 
    int total_retransmits = 0;

    for (i = 0; i < args->nthreads; i++) {
        total_retransmits += args->thread_data[i].retransmits; 
    }
    printf ("[%s] %d retransmits so far\n", args->machineid, total_retransmits);
    fflush (stdout);
}


void
record_throughput (ProgramArgs *args, FILE *out)
{
    // Currently this doesn't do anything if there is an error opening
    // the fd or writing to the file. TODO do we care?
    int i;
    uint64_t total_npackets = 0;
    char buf[32];
    int n; 
    struct timespec now = PreciseWhen ();

    for (i = 0; i < args->nthreads; i++) {
        total_npackets += args->thread_data[i].counter;
    }

    memset (buf, 0, 32);
    snprintf (buf, 32, "%lld,%.9ld,%"PRIu64"\n", (long long) now.tv_sec, 
            now.tv_nsec, total_npackets);
    n = fwrite (buf, 1, strlen (buf), out);
    if (n < strlen (buf)) {
        printf ("Error writing throughput to file!\n");
    }
}


void
UpdateProgramState (ProgramState state)
{
    int i;
    if ((args.collect_stats) && (!args.tr) && (state == warmup)) {
        CollectStats (&args);
    }

    args.program_state = state;
    for (i = 0; i < args.nthreads; i++) {
        args.thread_data[i].program_state = state;
    }
}


void 
UpdateFds (int do_ep)
{
    int i, fd;
    if (do_ep) {
        fd = args.thread_data[0].ep;
        printf ("Updating everyone to ep %d\n", fd);

        for (i = 1; i < args.nthreads; i++) {
            args.thread_data[i].ep = fd;
        }
    } else {
        fd = args.thread_data[0].commfd;
        printf ("Updating everyone to commfd %d\n", fd);

        for (i = 1; i < args.nthreads; i++) {
            args.thread_data[i].commfd = fd;
        }
    }
}

void
setup_filenames (ThreadArgs *targs)
{
    // Caller is responsible for creating the directory
    char s[FNAME_BUF];
    char s2[FNAME_BUF];

    memset (&s, 0, FNAME_BUF);
    memset (&s2, 0, FNAME_BUF);
    memset (&targs->latency_outfile, 0, FNAME_BUF);
    memset (&targs->tput_outfile, 0, FNAME_BUF);

    snprintf (s, FNAME_BUF, "%s/%s_%s.%d-latency.dat", args.outdir, args.outfile, args.machineid, 
            targs->threadid);
    snprintf (s2, FNAME_BUF, "%s/%s_%s-throughput.dat", args.outdir, args.outfile, args.machineid);

    memcpy (targs->latency_outfile, s, FNAME_BUF);
    memcpy (targs->tput_outfile, s2, FNAME_BUF);
}


void
write_latency_data (ThreadArgs *p, FILE *out, char *pbuf, struct timespec *recvtime)
{
    int m; 

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
                (long long) recvtime->tv_sec, recvtime->tv_nsec);
        p->lbuf_offset += m;
    }
}


void
debug_print (ThreadArgs *p, int debug_id, const char *format, ...)
{
    if (debug_id) {
        va_list valist; 
        va_start (valist, format);
        if (p != NULL) {
            printf ("%s ", p->threadname);
        }
        vfprintf (stdout, format, valist);
        va_end (valist);
        fflush (stdout);
    }
}

void
id_print (ThreadArgs *p, const char *format, ...)
{
    va_list valist;
    va_start (valist, format);
    printf ("%s ", p->threadname);
    vfprintf (stdout, format, valist);
    va_end (valist);
    fflush (stdout);
}

void
CollectStats (ProgramArgs *p)
{
    int pid = fork ();

    if (pid == 0) {
        printf ("[server] Launching collectl...\n");
        fflush (stdout);
        char nsamples[128];
        char outfile[128];
        
        snprintf (nsamples, 128, "-c%d", p->expduration);
        snprintf (outfile, 128, "%s/%s-collectl", p->outdir, p->outfile);

        // else save results to file
        char *argv[8];

        argv[0] = "collectl";
        argv[1] = "-P";
        argv[2] = "-f";
        argv[3] = outfile;
        argv[4] = "-sc";
        argv[5] = nsamples;
        argv[6] = "-oaz";
        argv[7] = NULL;
        execvp ("collectl", argv);
    }
}


void
SignalHandler (int signum) {
    if (signum == SIGINT) {
        printf ("Got a SIGINT...\n");
        exit (0);
    } else if (signum == SIGTERM) {
        printf ("Oooops got interrupted...\n");
        exit (0);
    }
}


void
WriteLatencyData (ThreadArgs *p, FILE *out, char *pbuf, struct timespec *recvtime)
{
    int m; 

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
                (long long) recvtime->tv_sec, recvtime->tv_nsec);
        p->lbuf_offset += m;
    }
}


double
When (void)
{
    // Low-resolution timestamp for coarse-grained measurements
    // Returns current time in seconds
    struct timeval tp;
    gettimeofday (&tp, NULL);
    return ((double) tp.tv_sec + (double) tp.tv_usec * 1e-6);
}


struct timespec
PreciseWhen (void)
{
    // More precise timestamping function; uses machine time instead of
    // clock time to avoid sync issues
    struct timespec spec;

    clock_gettime (CLOCK_MONOTONIC, &spec);
    return spec;
}

void
PrintPreciseTime (void)
{
    struct timespec spec;
    spec = PreciseWhen ();
    printf ("%lld,%.9ld: ", (long long) spec.tv_sec, spec.tv_nsec);
}


struct timespec
diff_timespecs (struct timespec start, struct timespec end)
{
    struct timespec temp;
    if ((end.tv_nsec - start.tv_nsec) < 0) {
        temp.tv_sec = end.tv_sec - start.tv_sec - 1;
        temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec - start.tv_sec;
        temp.tv_nsec = end.tv_nsec - start.tv_nsec;
    }
    return temp;
}

