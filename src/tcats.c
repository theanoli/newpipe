#include "harness.h"

void
Init (ProgramArgs *pargs, int *pargc, char ***pargv)
{
    // Allocate memory for per-thread data structures
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
    // Copy relevant data to thread data structures and launch
    // threads
    int i, ret;
    int nprocs_onln;
    cpu_set_t cpuset;

    ThreadArgs *targs = pargs->thread_data;

    for (i = 0; i < pargs->nthreads; i++) {
        targs[i].machineid = pargs->machineid;
        targs[i].threadid = i;
        snprintf (targs[i].threadname, 128, "[%s.%d]", pargs->machineid, i);

        // All clients will connect to the same server port
        targs[i].host = pargs->host;
        targs[i].tr = pargs->tr;
        targs[i].program_state = startup;

        // For servers, this is how many clients from which to expect connections.
        // For clients, this is how many total server threads there are. 
        targs[i].ncli = pargs->ncli;
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
    }
}


void
GenerateTimestamp (char *pbuf)
{
    struct timespec sendtime;
    sendtime = PreciseWhen ();
    snprintf (pbuf, PSIZE, "%lld,%.9ld",
            (long long) sendtime.tv_sec, sendtime.tv_nsec);
}


void
HandleReceivedTimestamp (ThreadArgs *p, FILE *out, char *pbuf)
{
    struct timespec recvtime;
    recvtime = PreciseWhen ();
    (p->counter)++;

    if ((!p->no_record) && (p->counter % 1000 == 0)) {
        WriteLatencyData (p, out, pbuf, &recvtime);
    }

    debug_print (p, DEBUG, 
            "Got timestamp: %s\n", pbuf);
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
    GenerateTimestamp (pbuf);

    
    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {

    }
    return 0;
}

int
Echo (ThreadArgs *p)
{
    while (p->program_state == startup) {
        // sppppiiiinnnnnn
    }

    // Receive and echo back packets
    while (p->program_state != end) {

    }
    return 0;
}


void
Setup (ThreadArgs *p)
{
    int ret;
    int i;
    uint64_t hz;

    // Initialize EAL
    ret = rte_eal_init(argc, argv);
    if (ret < 0) { 
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    }

    argc -= ret;
    argv += ret;

    rte_timer_subsystem_init ();

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);

    // Set up whatever is needed for communication
    if (p->tr) {
        // TODO what is happening here? 
        // newvar: struct ether_addr *mymac
        // newvar: struct ether_addr *hostmac (for client only)
        rte_eth_macaddr_get(seqcli_ethport, &eth_addr);

        hz = rte_get_tsc_hz ();
        core_frequency = rte_get_tsc_hz();

        // TODO newvar: uint64_t pkt_timeout_tsc
        pkt_timeout_tsc = usec_to_cycles (hz, PKT_TIMEOUT);  // in us

        // Initialize request structs (ring elements)
        printf ("\nInitializing request structs... \n");
        printf ("\tSize of packet: %lu\n", sizeof (seq_pkt_t));

        // umm this is janky, but sequencer conncetion should never access its index
        // hopefully this will cause an error if it does
        init_cc_state(&sequencer_state, SEQUENCER, READY, &sequencer_addr, 0xff);

        // TODO newvar: batch_ring
        memset (&batch_ring, 0, sizeof (batch_ring));

        for (i = 0; i < BATCH_RING_SIZE; i++) {
            batch_ring[i].batch_status = BATCH_FULFILLED;
            batch_ring[i].batch_ring_idx = i;
        }

        // Last three batch descriptors are reserved for persisting un-piggybacked
        // sequence numbers
        for (; i < BATCH_RING_SIZE + 3; i++) {
            batch_ring[i].batch_status = BATCH_DT_PERSISTING;
            batch_ring[i].batch_ring_idx = i;
        }

        /** Client setup stuff here **/
        // Set up the client template header
        populate_header (&client_packet_template, NULL);
        client_packet_template.uip.udp.dgram_len = htons (sizeof (client_payload_t) +
                sizeof (struct udp_hdr));
        client_packet_template.uip.ip.total_length = htons (sizeof (client_pkt_t)); 
        client_packet_template.uip.ip.src_addr = 0;
        client_packet_template.uip.ip.dst_addr = 0;

        // setup the MinPQ for clients
        minPQ = PQueue_new(MAX_CLIENTS, pfCompare, pfFree);
        memset(client_pq_elems, 0, MAX_CLIENTS * sizeof(pqElem_t));
        for (i = 0; i < nlogical_clients; i++) {
            client_pq_elems[i].client_ring_index = i;
        }

        // we need these to be able to grow, use calloc so we can grow later
        received_seq_nums = (uint8_t *) calloc(INIT_N_BLOCKS * BLOCK_SIZE, sizeof(uint8_t));
        if (received_seq_nums == NULL) {
            printf("Could not allocate memory for the bitmap!\n");
            exit(1);
        }
        counts = (uint64_t *) calloc(INIT_N_BLOCKS, sizeof(uint64_t));
        if (counts == NULL) {
            printf("Could not allocate memory for the counts array!\n");
            exit(1);
        }

        // set up the timer for sequencer heartbeats
        timer_init(&seq_alive_timer, usec_to_cycles(hz, SEQ_HEARTBEAT_TO));

#if(TEST_SEQUENCER == 1)
            seq_test_data = allocate_test_datastrs (proxy_id, "seq");
            if (seq_test_data == NULL) {
                return -1;
            }
#endif
#if(TEST_DEPTRACKER == 1)
            dt_test_data = allocate_test_datastrs (proxy_id, "dt");
            if (dt_test_data == NULL) {
                return -1;
            }
#endif

        // todo use if() or #if() ?
        if (TRACK_REQUESTS) {
            track_client_req_id = 0;
            track_last_timestamp = 0;
            track_batch_idx = 0;// this batch exists but using track_client to check
            track_added_to_batch = 0;

            // file to write to
            track_file = fopen("tracked_requests", "w");
            fprintf(track_file, "frequency: %"PRIu64"\n", core_frequency);
        }
        
        printf ("\nSetting up RTE...\n");
        setup_rte ();

        printf ("Port %u, MAC address ", (unsigned) seqcli_ethport);
        print_mac (eth_addr);
        printf ("\n");

        printf ("Locking memory to avoid swapping out...");
        if (mlockall (MCL_CURRENT | MCL_FUTURE) < 0) {
            perror ("failed to lock memory :-( \n");
        } else {
            printf ("memory locked :-) \n");
        }

        // todo setup stat tracking stuff
        // todo check link status?

        printf("Success! Starting sending packets...\n\n");
        fflush (stdout);

        unsigned lcore_id;
        RTE_LCORE_FOREACH_SLAVE (lcore_id) {
            if (rte_eal_wait_lcore(lcore_id) < 0) {
                ret = -1;
                break;
            }
        }


    } else {

        establish (p);    
    }
}


void
CleanUp (ThreadArgs *p)
{
    // Might be empty?
}


