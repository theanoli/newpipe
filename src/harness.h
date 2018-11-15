#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h> 
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h> 
#include <time.h>
#include <unistd.h> 

#define PSIZE   32
#define NSERVERCORES 16
#define DEFPORT 8000
#define MAXEVENTS 8192

// For throughput experiments: time to wait before
// starting measurements
#define DEBUG 0
#define STARTUP 2
#define WARMUP 3
#define COOLDOWN 5

// TCP-specific
#if defined(TCP)
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>

  typedef struct protocolstruct ProtocolStruct;
  struct protocolstruct
  {
      struct sockaddr_in      sin1,   /* socket structure #1              */
                              sin2;   /* socket structure #2              */
      int                     nodelay;  /* Flag for TCP nodelay           */
      struct hostent          *addr;    /* Address of host                */
      int                     sndbufsz, /* Size of TCP send buffer        */
                              rcvbufsz; /* Size of TCP receive buffer     */
  };
#else 
  // For now, we only have one option; will have more when we define our own
  // protocol TODO
  #error "TCP must be defined during compilation!"
#endif

// Global data structures
typedef enum program_state {
    startup,
    warmup,
    experiment,
    cooldown,
    end
} ProgramState;

// Thread-specific data structures
typedef struct threadargs ThreadArgs;
struct threadargs 
{
    /* This is the common information that is needed for all tests           */
    int     tr;         /* Transmit and Recv flags, or maybe neither    */
    char *  machineid; /* Machine ID   */ 
    int     threadid;       /* The thread number                            */
    char    threadname[128];    /* MachineID.threadID for printing          */

    int     servicefd,     /* File descriptor of the network socket         */
            commfd;        /* Communication file descriptor                 */
    short   port;          /* Port used for connection                      */
    ProtocolStruct prot;   /* Protocol-depended stuff                       */

    char    *host;          /* Name of receiving host                       */
    char    tput_outfile[512];
    char    latency_outfile[512];       /* Where results go to die                      */
    int     latency;        /* 1 if this is a latency experiment            */
    int     ncli;           /* #server threads if tr; #client threads per 
                               server thread if rcv                         */
    int     nrtts; 
    int     no_record;

    int     bufflen;       /* Length of transmitted buffer                  */

    char    *lbuff;          /* For saving latency measurements */

    // for throughput measurements
    uint64_t counter;       /* For counting packets!                        */
    double  duration;       /* Measured time over which packets are blasted */
    int ep;                 /* For epoll file descriptor                    */
    uint64_t retransmits;   /* Only useful for unreliable transports        */

    // timer data 
    volatile ProgramState program_state;
    double t0;
    double pps;
    int tput_done;

};

typedef struct programargs ProgramArgs;
struct programargs
{
    volatile ProgramState    program_state;
    char *  machineid;      /* Machine id */
    int     latency;        /* Measure latency (1) or throughput (0)        */
    int     expduration;    /* How long to count packets                    */
    char    *host;          /* Name of receiving host                       */
    short   port;
    int     collect_stats;  /* Collect stats on resource usage              */
    int     tr;             /* Is this a client? */ 
    int     nthreads;       /* How many threads to launch                   */
    int     no_record;      /* Flag: record results or not                  */

    char    *outdir;
    char    *outfile;

    pthread_t *tids;        /* Thread handles                               */
    ThreadArgs *thread_data;    /* Array of per-thread data structures      */

    // Possibly obsolete
    int     ncli;           /* For throughput: number of clients in exp     */
};

typedef struct data Data;
struct data
{
    double t;
    double bps;
    double variance;
    int    bits;
    int    repeat;
};


void InterruptThreads ();
void UpdateProgramState (ProgramState state);
double When ();
struct timespec PreciseWhen ();
void Init (ProgramArgs *p, int* argc, char*** argv);
void Setup (ThreadArgs *p);
void establish (ThreadArgs *p);
int setsock_nonblock (int fd);
void SendData (ThreadArgs *p);
void LaunchThreads (ProgramArgs *p);
void *ThreadEntry (void *vargp);
void TimestampTxRx (ThreadArgs *p);
void Echo (ThreadArgs *p);
void CleanUp (ThreadArgs *p);
void PrintUsage();
void SignalHandler (int signum);
void CollectStats (ProgramArgs *p);
int getopt( int argc, char * const argv[], const char *optstring);
void setup_filenames (ThreadArgs *targs);
void record_throughput (ThreadArgs *targs);
void debug_print (int debug_id, const char *format, ...);
