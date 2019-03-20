#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#define MAXEVENTS 16
#define MAXEVENTS_CLI 16
#define MAXNPORTS 128
#define FNAME_BUF 256
#define LBUFSIZE 512

#define CLIENT_DEBUG 1
#define SERV_DEBUG 0

// For throughput experiments: time to wait before
// starting measurements
#define STARTUP 4
#define WARMUP 10
#define COOLDOWN 10

typedef struct protocolstruct ProtocolStruct;

// TCP-specific
#if defined(TCP)
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>

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
#elif defined(ERPC)
  #include <cstring>
  #include "rpc.h"
  #include "util/numautils.h"

  struct protocolstruct {
      erpc::Nexus *nexus;
      std::string myip;
      erpc::Rpc<erpc::CTransport> *rpc;
      int session_num;
      erpc::MsgBuffer req_msgbuf;
      erpc::MsgBuffer resp_msgbuf;
      short sm_port;
  };
#elif defined(TCATS)
  #include "state.h"
  #include "common.h"

  #define TX_CORE 1
  #define RX_CORE 5
  
  #define NB_MBUF 65535  // should be 2**n - 1
  
  #define MAX_TX_BURST 16
  #define MAX_RX_BURST 32
  
  #define MAX_PKTS_PER_SEC 14800000ULL
  
  #define MEMPOOL_CACHE_SIZE 256
  
  #define MAX_RX_QUEUE_PER_LCORE 16
  
  #define RTE_TEST_RX_DESC_DEFAULT 512
  #define RTE_TEST_TX_DESC_DEFAULT 512
  
  #define TIMER_RESOLUTION 10  // in us // destroy this?
  
  #define PKT_TIMEOUT 1000000 //1000000 // in us
  #define EPOCH 1000 //1000  // The epoch duration in us; should be ~1RTT
  
  #define GC_PERIOD_USEC 10000 //750000 // 500 ms, .5 s
  
  #define SEQ_HEARTBEAT_TO 750000 //5*TIMER_RESOLUTION
  
  #define BATCH_SIZE_COUNTER_INCREMENT 50
  #define BATCH_SIZE_COUNTER_ARRAY_SIZE 127

  struct protocolstruct {

  }
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

    volatile ProgramState program_state;
    FILE *out;
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

#ifdef ERPC
    std::vector<std::thread> tids;
#else
    pthread_t *tids;        /* Thread handles                               */
#endif

    ThreadArgs *thread_data;    /* Array of per-thread data structures      */

    // Possibly obsolete
    int     ncli;           /* For throughput: number of clients in exp     */
    
    uint8_t pin_threads;
    uint8_t done;
    ProtocolStruct prot;   /* Protocol-depended stuff                       */
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
void UpdateFds (int do_ep);
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
void record_throughput (ProgramArgs *targs, FILE *out);
void debug_print (ThreadArgs *p, int debug_id, const char *format, ...);
void id_print (ThreadArgs *p, const char *format, ...);
void WriteLatencyData (ThreadArgs *p, FILE *out, char *pbuf, struct timespec *recvtime);
void aggregate_retransmits (ProgramArgs *p);
