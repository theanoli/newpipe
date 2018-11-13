/* Use this to run tests with arbitrary protocols; functions
   to establish/tear down connections and to send/receive packets
   will be protocol-specific and will be defined in other files.
*/

#include "harness.h"

#define NRTTS 1000

extern char *optarg;

// Initialize these here so they are accessible to signal handler
ProgramArgs args;
FILE *out;          /* Output data file                          */

int 
main (int argc, char **argv)
{
    int sleep_interval __attribute__((__unused__)); /* How long to sleep b/t latency pings (usec) */

    int c, i;

    /* Initialize vars that may change from default due to arguments */
    sleep_interval = 0;
    args.latency = 1;  // Default to do latency; this is arbitrary
    args.no_record = 0;

    // Default initialization to server (not transmitter)
    args.tr = 0;

    signal (SIGINT, SignalHandler);
    signal (SIGALRM, SignalHandler);
    signal (SIGTERM, SignalHandler);

    // Machine ID
    args.machineid = (char *)malloc (128);
    gethostname (args.machineid, 128);
    args.machineid = strsep (&args.machineid, ".");

    // Thread-specific arguments
    args.host = NULL;
    args.port = DEFPORT;  // The first port; if more than one server thread, 
                          // will need to open DEFPORT + 1, ... 

    args.program_state = startup;

    /* Parse the arguments. See Usage for description */
    while ((c = getopt (argc, argv, "o:d:H:T:r:c:P:ps:tu:lw:h")) != -1)
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

            case 't': args.latency = 0;
                      break;

            case 'u': args.expduration = atoi (optarg);
                      break;

            case 'l': args.collect_stats = 1;
                      break; 

            case 'h': PrintUsage ();
                      exit (0);

            default: 
                     PrintUsage (); 
                     exit (-12);
       }
    }
    
    // Initialize variables and whatnot
    printf ("[%s] Initializing...\n", args.machineid);
    Init (&args, &argc, &argv);   

    /* Spin up the specified number of threads, set up network connections */
    LaunchThreads (&args);

    if (args.tr) {
        // Clients
        sleep (1);  // TODO why is this needed?
        UpdateProgramState (experiment); // start sending

    } else {
        // Servers
        // Wait a few seconds to let clients come online
        printf ("Waiting for clients to start up...\n");
        sleep (STARTUP);

        printf ("Assuming all clients have come online!");
        printf (" Entering warmup period...\n");
        UpdateProgramState (warmup);
        sleep (WARMUP);
        
        printf ("Starting counting packets...\n");
        UpdateProgramState (experiment);
        sleep (args.expduration);

        printf ("Experiment over, stopping counting packets...\n");
        UpdateProgramState (cooldown);
        sleep (COOLDOWN);

        UpdateProgramState (end);
    }

    for (i = 0; i < args.nthreads; i++) {
        pthread_join (args.tids[i], NULL);
    }

    return 0;
}


void
InterruptThreads (void)
{
    int i;
    for (i = 0; i < args.nthreads; i++) {
        if (!args.thread_data[i].tput_done) {
            pthread_kill (args.tids[i], SIGTERM);
        }
    }
}


void
UpdateProgramState (ProgramState state)
{
    int i;
    if ((args.collect_stats) && (!args.tr) && (state == experiment)) {
        CollectStats (&args);
    }

    for (i = 0; i < args.nthreads; i++) {
        args.thread_data[i].program_state = state;
        if ((state == experiment) && (!args.tr)) {
            args.thread_data[i].t0 = When ();
        }

        if ((state == cooldown) && (!args.tr)) {
            args.thread_data[i].duration = When () - args.thread_data[i].t0;
        }
    }
}


void
setup_filenames (ThreadArgs *targs)
{
    // Caller is responsible for creating the directory
    char s[512];
    char s2[512];

    memset (&s, 0, 512);
    memset (&s2, 0, 512);
    memset (&targs->latency_outfile, 0, 512);
    memset (&targs->tput_outfile, 0, 512);

    snprintf (s, 512, "%s/%s_%s.%d-latency.dat", args.outdir, args.outfile, args.machineid, 
            targs->threadid);
    snprintf (s2, 512, "%s/%s_%s-throughput.dat", args.outdir, args.outfile, args.machineid);

    memcpy (targs->latency_outfile, s, 512);
    memcpy (targs->tput_outfile, s2, 512);

    printf ("Results going into file");
    printf (" %s\n", targs->latency ? targs->latency_outfile : targs->tput_outfile);
}


void
record_throughput (ThreadArgs *targs)
{
    // Currently this doesn't do anything if there is an error opening
    // the fd or writing to the file. TODO do we care?
    int tputfd = open (targs->tput_outfile, O_CREAT | O_RDWR,
            S_IRWXU | S_IRWXG | S_IRWXO);
    if (tputfd < 0) {
        fprintf (stderr, "Can't open throughput file for output!\n");
        return;
    }

    char buf[20];
    memset (buf, '\n', 20);
    snprintf (buf, 20, "%f\n", targs->pps);
    int ret = pwrite (tputfd, buf, 20, 20 * targs->threadid);
    if (ret < 0) {
        perror ("throughput write: ");
    }

    close (tputfd);
}


void
debug_print (int debug_id, const char *format, ...)
{
    if (debug_id) {
        va_list valist; 
        va_start (valist, format);
        vfprintf (stdout, format, valist);
        va_end (valist);
    }
}


void
PrintUsage (void)
{
    printf ("\n");
    printf ("To run server, run '[sudo] ./NPxyz'\n");
    printf ("To run client, run '[sudo] ./NPxyz -H server-hostname ...'\n");
    printf ("(mTCP is the only transport that needs sudo [for now])\n\n");
    printf ("Options (client only unless otherwise specified):\n");
    printf ("\t-o\twhere to write latency output; default\n"
            "\t\tresults/timestamp+opts\n");
    printf ("\t-H\tIP/hostname of receiver\n");
    printf ("\t-r\tnumber of latency measurements to collect\n"
            "\t\t(ping packets to send); default 1000\n");
    printf ("\t-P\t(client AND server) port number, default 8000\n");
    printf ("\t-s\tsleeptime for throttling client send rate\n"
            "(default 0)\n");
    printf ("\t-t\tmeasure throughput (default latency)\n");
    printf ("\t-h\t(client or server) print usage\n");
    printf ("\n");
    exit (0);
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
    } else if (signum == SIGALRM) {
        // We only need to set a new alarm if this is a throughput experiment
        // Otherwise just let the clock run; latency duration is measured by 
        // number of packets (-r option).
        int i; 
        if (!args.latency) {
            if (args.program_state == warmup) {
                printf ("Starting to count packets for throughput...\n");
                args.program_state = experiment; 
                if (args.collect_stats) {
                    CollectStats(&args);
                }

                for (i = 0; i < args.nthreads; i++) {
                    args.thread_data[i].t0 = When();
                }

                alarm (args.expduration);
            } else if (args.program_state == experiment) {
                // Experiment has completed; let it keep running without counting
                // packets to allow other servers to finish up
                args.program_state = cooldown;
                for (i = 0; i < args.nthreads; i++) {
                    args.thread_data[i].duration = When () - args.thread_data[i].t0;
                }
                printf ("Experiment over, stopping counting packets...\n");
                alarm (COOLDOWN);
            } else if (args.program_state == cooldown) {
                // The last signal; end the experiment by setting p->tput_done
                args.program_state = end;
            }
        }
    } else if (signum == SIGTERM) {
        printf ("Oooops got interrupted...\n");
        exit (0);
    }
}


double
When (void)
{
    // Low-resolution timestamp for coarse-grained measurements
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


struct timespec
diff (struct timespec start, struct timespec end)
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

