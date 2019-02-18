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
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/time.h> 
#include <time.h>
#include <unistd.h> 

#define PSIZE   32
#define DEFPORT 8000
#define MAXEVENTS 16
#define MAXEVENTS_CLI 16
#define MAXNPORTS 64
#define FNAME_BUF 256
#define LBUFSIZE 512

#define DEBUG 0
#define CLIENT_DEBUG 1
#define SERV_DEBUG 0

// For throughput experiments: time to wait before
// starting measurements
#define STARTUP 4
#define WARMUP 10
#define COOLDOWN 10
#define READTO 0

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
#elif defined(MTCP)
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
    
  #include <mtcp_api.h>
  #include <mtcp_epoll.h>

  typedef struct protocolstruct ProtocolStruct;
  struct protocolstruct
  {
      struct sockaddr_in      sin1,   /* socket structure #1              */
                              sin2;   /* socket structure #2              */
      int                     nodelay;  /* Flag for TCP nodelay           */
      struct hostent          *addr;    /* Address of host                */
      int                     sndbufsz, /* Size of TCP send buffer        */
                              rcvbufsz; /* Size of TCP receive buffer     */
      mctx_t                  mctx;
  };
#else 
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
    uint8_t     tr;         /* Transmit and Recv flags, or maybe neither    */
    char *  machineid; /* Machine ID   */ 
    uint16_t     threadid;       /* The thread number                            */
    char    threadname[128];    /* MachineID.threadID for printing          */

    int     servicefd;     /* File descriptor of the network socket         */
    int        commfd;        /* Communication file descriptor                 */
    short   port;          /* Port used for connection/listening */
    short   nports;         /* How many connections to open on client */
    short nports_array[MAXNPORTS];
    ProtocolStruct prot;   /* Protocol-depended stuff                       */

    char    *host;          /* Name of receiving host                       */
    char    tput_outfile[FNAME_BUF];
    char    latency_outfile[FNAME_BUF];       /* Where results go to die                      */
    uint16_t     ncli;           /* #server threads if tr; #client threads per 
                               server thread if rcv                         */
    uint8_t     no_record;
    char *      lbuf;               /* buffer for latency 
                                       measurements */
    uint16_t     lbuf_offset;             /* where to write next
                                       latency value */

    // for throughput measurements
    volatile uint64_t counter;       /* For counting packets!                        */
    volatile int ep;                 /* For epoll file descriptor                    */
    uint64_t retransmits;   /* Only useful for unreliable transports        */

    // timer data 
    volatile ProgramState program_state;
    uint8_t *done;
};

typedef struct programargs ProgramArgs;
struct programargs
{
    volatile ProgramState    program_state;
    char *  machineid;      /* Machine id */
    int     expduration;    /* How long to count packets                    */
    char    *host;          /* Name of receiving host                       */
    short   port;
    int     collect_stats;  /* Collect stats on resource usage              */
    int     tr;             /* Is this a client? */ 
    int     nthreads;       /* How many threads to launch                   */
    int     no_record;      /* Flag: record results or not                  */
    short   nports;         /* How many connections to open on client */

    char    *outdir;
    char    *outfile;

    pthread_t *tids;        /* Thread handles                               */
    ThreadArgs *thread_data;    /* Array of per-thread data structures      */

    // Possibly obsolete
    int     ncli;           /* For throughput: number of clients in exp     */
    
    uint8_t pin_threads;
    uint8_t done;
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
void UpdateEpFds ();
double When ();
void SignalHandler (int signum);
struct timespec PreciseWhen ();
void PrintPreciseTime ();
void Init (ProgramArgs *p, int* argc, char*** argv);
void Setup (ThreadArgs *p);
void establish (ThreadArgs *p);
int setsock_nonblock (int fd);
void SendData (ThreadArgs *p);
void LaunchThreads (ProgramArgs *p);
void *ThreadEntry (void *vargp);
void *ThroughputRecorder (void *vargp);
int TimestampTxRx (ThreadArgs *p);
int Echo (ThreadArgs *p);
void CleanUp (ThreadArgs *p);
void PrintUsage();
void CollectStats (ProgramArgs *p);
int getopt( int argc, char * const argv[], const char *optstring);
void setup_filenames (ThreadArgs *targs);
void record_throughput ();
void debug_print (ThreadArgs *p, int debug_id, const char *format, ...);
void id_print (ThreadArgs *p, const char *format, ...);
void write_latency_data (ThreadArgs *p, FILE *out, char *pbuf, struct timespec *recvtime);
