#include "harness.h"


static constexpr uint16_t kReqType = 2;


// Session management handler
void sm_handler(int, erpc::SmEventType, erpc::SmErrType, void *) {}


// Echo (server-side) handler
void echo_handler(erpc::ReqHandle *req_handle, void *_p) {
    auto *p = static_cast<ThreadArgs *>(_p);

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    erpc::Rpc<erpc::CTransport>::resize_msg_buffer(
            &req_handle->pre_resp_msgbuf, PSIZE);
    req_handle->pre_resp_msgbuf.buf[0] = req_msgbuf->buf[0];
    p->prot.rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf);
}


void
Init (ProgramArgs *pargs, int *pargc, char ***pargv)
{
    pargs->thread_data = (ThreadArgs *)calloc (sizeof (ThreadArgs), pargs->nthreads);
    if (pargs->thread_data == NULL) {
        printf ("Error malloc'ing space for thread args!\n");
        exit (-10);
    }
    pargs->tids = std::vector<std::thread>(pargs->nthreads);

    // Create Nexus object and register handlers (if server)
    pargs->prot.nexus = new erpc::Nexus (pargs->prot.myip, 0, 0);
    if (!pargs->tr) {
        pargs->prot.nexus->register_req_func(kReqType, echo_handler);
    }
}


void
LaunchThreads (ProgramArgs *pargs)
{
    int i, ret;

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
        // Clients should spawn *at least* 1 thread per server thread
        targs[i].ncli = pargs->ncli;
        targs[i].no_record = pargs->no_record;
        targs[i].counter = 0;
        targs[i].prot = pargs->prot;

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

        pargs->tids[i] = std::thread(ThreadEntry, &targs[i]);
        erpc::bind_to_core(pargs->tids[i], 0, i);

        fflush (stdout);
    }
}


void timestamp_cont_func(void *_p, void *tag) {
    auto *p = static_cast<ThreadArgs *>(_p);
    
    // Get current timestamp
    struct timespec sendtime, recvtime;
    recvtime = PreciseWhen();
    (p->counter)++;
    
    // Write to file
    if (!p->no_record && (p->counter % 1000)) {
       WriteLatencyData(p, p->out, reinterpret_cast<char *>(p->prot.resp_msgbuf.buf), &recvtime); 
    }

    // Send another request
    // Send initial message to get things started
    sendtime = PreciseWhen ();
    std::snprintf (reinterpret_cast<char *>(p->prot.req_msgbuf.buf), PSIZE, "%lld,%.9ld",
            (long long) sendtime.tv_sec, sendtime.tv_nsec);
    p->prot.rpc->enqueue_request(p->prot.session_num, kReqType, &(p->prot.req_msgbuf), 
            &(p->prot.resp_msgbuf), timestamp_cont_func, nullptr);
}


int
TimestampTxRx (ThreadArgs *p)
{
    // Send and then receive an echoed timestamp.
    // Return a negative value if error, else return 0.
    struct timespec sendtime, recvtime;

    // Open file for recording latency data
    if (p->tr && (!p->no_record)) {
        if ((p->out = fopen (p->latency_outfile, "wb")) == NULL) {
            fprintf (stderr, "Can't open %s for output!\n", p->latency_outfile);
            return -1;
        }
    } else {
        p->out = stdout;
    }

    while (p->program_state != experiment) {
        // Wait 
    }
    
    // Send initial message to get things started
    sendtime = PreciseWhen ();
    std::snprintf (reinterpret_cast<char *>(p->prot.req_msgbuf.buf), PSIZE, "%lld,%.9ld",
            (long long) sendtime.tv_sec, sendtime.tv_nsec);
    p->prot.rpc->enqueue_request(p->prot.session_num, kReqType, &(p->prot.req_msgbuf), 
            &(p->prot.resp_msgbuf), timestamp_cont_func, nullptr);
    
    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {
        // Run event loop, check program_state every second
        p->prot.rpc->run_event_loop(1000);
    }

    return 0;
}


int
Echo (ThreadArgs *p)
{
    while (p->program_state == startup) {
        // sppppiiiinnnnnn
    }

    while (p->program_state != end) {
       p->prot.rpc->run_event_loop(1000);
    }
    return 0;
}


void
Setup (ThreadArgs *p)
{
    // Create RPC endpoints and (client only) connect to other endpoint
    p->prot.rpc = new erpc::Rpc<erpc::CTransport>(p->prot.nexus, 
            static_cast<void *>(p),  // Thread context
            static_cast<uint8_t>(p->threadid),  // ThreadID
            sm_handler);  // Session management handler

    p->prot.req_msgbuf = p->prot.rpc->alloc_msg_buffer_or_die(PSIZE);
    p->prot.resp_msgbuf = p->prot.rpc->alloc_msg_buffer_or_die(PSIZE);

    if (!p->tr) {
        return;
    }

    std::string server_uri = (std::string)p->host + ":" + std::to_string(p->port);
    p->prot.session_num = p->prot.rpc->create_session(server_uri, 0);
    if (p->prot.session_num < 0) {
        id_print(p, "Failed to create session!\n");
        exit(-1);
    }

    while (!p->prot.rpc->is_connected(p->prot.session_num)) {
        p->prot.rpc->run_event_loop_once();
    }
}

void
CleanUp (ThreadArgs *p)
{

}
