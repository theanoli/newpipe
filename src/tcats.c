#include "harness.h"

#define CWND_MULTIPLIER 250

bool force_quit = 0;

// Do I need to do this? 
ThreadArgs targs;
ThreadArgs *p = &targs;

/* mask of enabled ports */
static uint32_t seqcli_enabled_port_mask = 1;
static unsigned int seqcli_rx_queue_per_lcore = 1;

uint64_t timer_period = 1; // seconds to print out stats

struct rte_mempool *seqcli_rx_pktmbuf_pool = NULL;
struct rte_mempool *seqcli_tx_pktmbuf_pool = NULL;
uint8_t proxy_id;
uint64_t total_pkts_tx = 0;
uint64_t total_pkts_rx = 0; 
uint64_t total_pkts_dropped = 0;
uint64_t client_retx = 0;
uint64_t slow_retx = 0;
uint64_t fast_retx = 0;

uint64_t total_requests_served = 0;

uint8_t seqcli_ethport = 0;
struct ether_addr eth_addr;

// Timer stuff
uint64_t pkt_timeout_tsc;
int timer_resolution_us;

// Configurable number of RX/TX ring descriptors todo configure these
uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

struct rte_eth_dev_tx_buffer *tx_buffer;
struct rte_eth_dev_rx_buffer *rx_buffer;

cc_state_t sequencer_state;

uint64_t core_frequency = 0;

// Use parse_mac_address_from_string with this
struct ether_addr __attribute__((unused)) sequencer_addr;  // Sequencer


void
parse_mac_address_from_string (char *str_addr, struct ether_addr *mac_address_struct)
{
    char tmp[18];

    strncpy (tmp, str_addr + 18, 17); 
    tmp[17] = '\0';

    sscanf(tmp, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &mac_address_struct->addr_bytes[0],
           &mac_address_struct->addr_bytes[1],
           &mac_address_struct->addr_bytes[2],
           &mac_address_struct->addr_bytes[3],
           &mac_address_struct->addr_bytes[4],
           &mac_address_struct->addr_bytes[5]
           );
}


/*--- From packet_handling.c ---------------------------------------------------*/
// constructs a single packet; desc is a seq_pkt_t
inline void
pkt_ctor(struct rte_mbuf *mbuf, seq_hdr_t *pkt_template, void *payload, 
        size_t pkt_size, struct ether_addr *dest_addr) {
    mbuf->packet_type = IPPROTO_UDP;

    // This is not exactly correct (not all pkts are seq_pkts) but avoids
    // having to cast to seq_hdr_t a bunch of times below
    seq_pkt_t *dst = (seq_pkt_t *)rte_pktmbuf_append (mbuf, pkt_size); 

    // In the mbuf
    struct ether_hdr    *eth = (struct ether_hdr *) &dst->hdr.eth;
    struct ipv4_hdr     *ip = (struct ipv4_hdr *) &dst->hdr.uip.ip;
    struct udp_hdr      *udp = (struct udp_hdr *) &dst->hdr.uip.udp;

    // Copy template and then payload
    rte_memcpy ((void *) dst, (uint8_t *) pkt_template, sizeof (seq_hdr_t));
    rte_memcpy ((void *) &dst->payload, (uint8_t *) payload, 
            pkt_size - sizeof (seq_hdr_t));

    // This MUST be done again for completely non-obvious reasons
    ether_addr_copy(dest_addr, &eth->d_addr);
    ether_addr_copy(&eth_addr, &eth->s_addr);

    udp->dgram_cksum = 0;
    ip->hdr_checksum = 0;

    // Update the checksums in the mbuf
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
    ip->hdr_checksum = rte_ipv4_cksum(ip);
}

// Pre-fill all packet header fields; dest address may be NULL (e.g., for
// populating the client header template)
void
populate_header (seq_hdr_t *hdr, struct ether_addr *addr)
{
    struct ether_hdr    *eth = (struct ether_hdr *) &hdr->eth;
    struct ipv4_hdr     *ip = (struct ipv4_hdr *) &hdr->uip.ip;
    struct udp_hdr      *udp = (struct udp_hdr *) &hdr->uip.udp;

    // Construct the ethernet header
    if (addr != NULL) {
        ether_addr_copy (addr, &eth->d_addr);
    }
    ether_addr_copy (&eth_addr, &eth->s_addr);
    eth->ether_type = htons (ETHER_TYPE_IPv4);

    // Construct the IPv4 header
    ip->version_ihl = (4 << 4) | (sizeof(struct ipv4_hdr) / IPV4_IHL_MULTIPLIER); // version_ihl
    ip->time_to_live = 8; // time_to_live, not going far...
    // todo looks like I didn't use hton here, don't think I need to
    ip->next_proto_id = IPPROTO_UDP;

    // these don't really matter here? probably only eth routing
    ip->src_addr = htonl(IPv4(10, 1, 1, 2)); // same
    ip->dst_addr = htonl(IPv4(10, 1, 1, 3)); // same

	// needs to be set to zero for the layer 4 checksum done after this call
	// probably should be done here, then reset to zero, I think this would be a better structure
    ip->hdr_checksum = 0;
    udp->dgram_cksum = 0;
    udp->dgram_len = htons(sizeof (seq_pkt_t)
            - sizeof (struct ipv4_hdr)
            - sizeof (struct ether_hdr));

    if (addr != NULL) {
        if (is_same_ether_addr_custom (addr, &sequencer_addr)) {
            udp->dst_port = (uint16_t) SEQUENCER;
        } else {
            force_quit = 1;
            printf ("Invalid packet addr!\n");
            return;
        }
    }

    ip->total_length = htons(sizeof (seq_pkt_t) - sizeof (struct ether_hdr));
}    


void
receive_packets (void)
{
    uint16_t nb_rx; 
    uint16_t i = 0;
    struct rte_mbuf *rx_pkts[MAX_RX_BURST]; 
    int ret __attribute__((unused));

    nb_rx = rte_eth_rx_burst(0, 0, rx_pkts, MAX_RX_BURST);
    for (i = 0; i < nb_rx; i++) {
        if (force_quit) {
            printf("Force quitting in receive_packets\n");
            return;
        }
         
        seq_pkt_t *pkt = rte_pktmbuf_mtod (rx_pkts[i], seq_pkt_t *);
        machine_id_t sender_id = get_sender_id (pkt);

        debug_print(DEBUG, "Received a packet: sender_id: %d\n", sender_id);

        switch (sender_id) {
            case SEQUENCER:
                process_seq_pkt (rx_pkts[i]);
                break;

            default:
                printf ("Unidentified sender!\n");
                force_quit = 1;
                return;
        }

        // free the packet to allow dpdk to use it again
        rte_pktmbuf_free (rx_pkts[i]);
        total_pkts_rx++;
    }
}


// Callback function for retx on TO
void
pkt_timer_cb (struct rte_timer *tim, void *arg)
{
    desc_t *desc = (desc_t *) arg;
    handle_event (TIMEOUT, desc->cc_state);
    slow_retx++;
    retransmit (tim, desc);
}


// Callback function for retx on TO or fast recovery
void
retransmit (struct rte_timer *tim, desc_t *desc)
{
    int idx = desc->payload.ring_idx;
    cc_state_t *state = desc->cc_state;
    seq_hdr_t *packet_template = &state->packet_template;
    payload_t *payload = &state->request_ring[idx].payload;

    struct rte_mbuf *mbuf = rte_pktmbuf_alloc (seqcli_tx_pktmbuf_pool);
    if (mbuf == NULL) {
        printf ("retransmit: Couldn't allocate an mbuf! Exiting...\n");
        force_quit = 1;
        return;
    }

    if (DEBUG) {
        printf ("Retransmitting ring_idx %d, machine %d\n", idx,
                state->machine_id);
    }
    payload->rpc_status = RETX;

    pkt_ctor (mbuf, packet_template, (void *) payload, sizeof (seq_pkt_t), 
            state->receiver);

    rte_eth_tx_buffer(seqcli_ethport, 0, tx_buffer, mbuf);
    rte_eth_tx_buffer_flush(seqcli_ethport, 0, tx_buffer);

    rte_timer_reset_sync(tim, pkt_timeout_tsc,
                         SINGLE, rte_lcore_id(), &pkt_timer_cb, desc);

    total_pkts_tx++;
}


inline void
send_packets (cc_state_t *state)
{
    int ret;
    struct rte_mbuf *mbufs[MAX_TX_BURST];
    uint16_t i;
    int idx __attribute__((unused));
    int nsent;
    desc_t *request_desc;
    payload_t *request_payload;

    // grab nburst pkts from the pool
    ret = rte_pktmbuf_alloc_bulk (seqcli_tx_pktmbuf_pool, mbufs, MAX_TX_BURST);
    if (ret < 0) {
        printf ("Couldn't allocate any mbufs! Exiting...\n");
        force_quit = 1;
        return;
    }

    machine_id_t receiver = state->machine_id;

    for (i = 0; i < MAX_TX_BURST; i++) {
        request_desc = get_next_free_descriptor (state);
        if (request_desc == NULL) {
            break;
        } 

        // TODO add timestamp here?
        request_payload = &request_desc->payload;
        prepare_request_for_sending (state, request_desc, batch_desc);

        pkt_ctor (mbufs[i], &state->packet_template, (void *) request_payload, 
                sizeof (seq_pkt_t), state->receiver);

        ret = rte_eth_tx_buffer (seqcli_ethport, 0, tx_buffer, mbufs[i]);
        total_pkts_tx += ret;

        send_list[i] = request_payload->ring_idx;
        state->next_pkt_id++;
    }

    nsent = i;
    for (; i < MAX_TX_BURST; i++) {
        rte_pktmbuf_free (mbufs[i]);
    }
        
    ret = rte_eth_tx_buffer_flush (seqcli_ethport, 0, tx_buffer);
    total_pkts_tx += ret;

    for (i = 0; i < nsent; i++) {
        idx = send_list[i];
        rte_timer_reset_sync (&state->pkt_timers[idx], pkt_timeout_tsc,
                SINGLE, 
                rte_lcore_id(), &pkt_timer_cb, &state->request_ring[idx]);
    }
    memset (&send_list, 0, sizeof (send_list));
}


// TODO can remove everything about batching here?
inline void
prepare_request_for_sending (cc_state_t *state, desc_t *request_desc, batch_desc_t *batch_desc)
{
    // update local descriptors and CC data before sending out packet
    // to sequencer or dependency tracker
    payload_t *request_payload = &request_desc->payload;
    uint16_t request_idx = request_payload->ring_idx;
    machine_id_t receiver = state->machine_id;

    // For CC: Update highest sent packet ID
    if (request_payload->pkt_id > state->highest_sent_pkt_id) {
        state->highest_sent_pkt_id = request_payload->pkt_id;
    }
}
/*--- END From packet_handling.c -----------------------------------------------*/

/*--- From sequencer_comm.c ----------------------------------------------------*/
int
process_seq_pkt (struct rte_mbuf *mbuf) 
{
    uint16_t idx;
    int i;
    client_desc_t *client_desc;
    uint64_t seq_num;

    seq_pkt_t *pkt = rte_pktmbuf_mtod (mbuf, seq_pkt_t *);

    struct ether_hdr    *eth = (struct ether_hdr *) &pkt->hdr.eth;
    payload_t           *payload = (payload_t *) &pkt->payload;

    // filter out lldp packets and packets from other clients 
    if (eth->ether_type == 0xCC88) {
        return 0;
    }

    // recovery stuff
    // always mark this as the time we have received something from the sequencer
    // (should we do this at the end of this function?)
    // todo check if this is tracking liveness of new sequencer now
    timer_reset(&seq_alive_timer);

    // todo check if this is a heartbeat packet (used to check if the
    // sequencer is alive when we haven't heard a response but also have
    // not recently sent something to the sequencer)
    if (payload->rpc_status == HEARTBEAT) {
        printf("was heartbeat, returning\n");
        have_pinged_sequencer = 0;
        return 0;
    }

//    // todo I think we don't need this anymore?
//    if (payload->rpc_status == SEND_BITMAP_AND_DEPS) {
//        printf("in sending bitmap switch\n");
//        recovery_send_bitmap_and_deps();
//        printf("SENT THE TEST MESSAGE\n");
//        force_quit = 1;
//        return 0;
//    }

    idx = payload->ring_idx;

    // Find the corresponding sequencer ring descriptor, which contains
    // info about which client send this request
    payload_t *seq_desc_payload = &sequencer_state.request_ring[idx].payload;
    batch_desc_t *batch_desc = &batch_ring[sequencer_state.request_ring[idx].batch_ring_idx];

    // the first && expression checks if we have already received a response to 
    // this descriptor; second checks that this is the correct request_id
    if ((seq_desc_payload->rpc_status != READY) &&
        (seq_desc_payload->pkt_id == payload->pkt_id)) {

        rte_timer_stop_sync (&sequencer_state.pkt_timers[idx]);
        if ((payload->pkt_id % 10000) == 0) {
            printf ("[proxy%d] Got seqnum %"PRIu64" for pkt_id %"PKT_ID_FMT", "
                    "status %d\n", proxy_id, payload->seq_num, 
                    payload->pkt_id, payload->rpc_status); 
        }

        seq_desc_payload->rpc_status = READY;
        seq_desc_payload->seq_num = payload->seq_num;

        // keep a max sn seen, and put that into the DT packet
        if (max_sequence_number < payload->seq_num) {
            max_sequence_number = payload->seq_num;
        }

        if (TRACK_REQUESTS) {
            // todo is this unlikely???
            if (track_client_req_id && track_added_to_batch && track_batch_idx == batch_desc->batch_ring_idx) { //track_client_req_id == client_desc->payload.req_id) {
                track_now_timestamp = rte_rdtsc();
                fprintf(track_file, "process_seq_pkt: %"PRIu64" %Lf\n",
                        track_now_timestamp, cycles_to_usec((track_now_timestamp - track_last_timestamp)));
                track_last_timestamp = track_now_timestamp;
                track_added_to_batch = 0; // it is no longer associated with any batch!
            }
        }

        // the batch range is [seq_num - batch_size + 1, seq_num]
        for (i = 0; i < payload->batch_size; i++) {
            seq_num = payload->seq_num - payload->batch_size + 1 + i;
            client_desc = &client_data[batch_desc->client_ids[i]];

            client_desc->payload.seq_num = seq_num;

            client_desc->req_status = CLIREQ_PERSISTING;

            bitmap_insert_seq_num (seq_num);

            // put the client request in the priority queue because
            // it is ready to be sent once it is persisted
            client_pq_elems[batch_desc->client_ids[i]].seq_num = seq_num;
            PQueue_insert(minPQ, &client_pq_elems[batch_desc->client_ids[i]]);
        }
        
        free_batch (batch_desc);

        process_ack (seq_desc_payload, &sequencer_state);

        // Record the sequence number & pkt_id and dump to file
#if(TEST_SEQUENCER == 1)
        int ret = record_seqnum (&sequencer_state.request_ring[idx].payload,
                seq_test_data);
        if (ret < 0) { 
            printf ("Failed writing test data!\n");
            force_quit = 1;
        } else if (sequencer_state.request_ring[idx].payload.pkt_id > req_limit) {
            if (force_quit != 1) {
                printf ("Received sufficient requests to complete test!\n");
                force_quit = 1;
            }
        }
#endif   

    } else {
        // Something went wrong here; possibly an old retransmit?
        if ((payload->seq_num % 1) == 0) {
            printf ("ReqIDs didn't match or descriptor is in RCVD state (2): %"
                    PKT_ID_FMT" state %d vs received %"PKT_ID_FMT" state %d\n",
                    sequencer_state.request_ring[idx].payload.pkt_id,
                    sequencer_state.request_ring[idx].payload.rpc_status,
                    payload->pkt_id, payload->rpc_status);
        }
    }

    return 0;
}

// Find the next unfulfilled sequencerequest (status NEW)
inline batch_desc_t *
get_next_unfulfilled_seq_request (void)
{
    uint8_t idx = sequencer_state.next_batch_ring_idx;
    batch_desc_t *desc;

    desc = &batch_ring[idx];

    while ((desc->batch_status != BATCH_SEQNUM_NEEDED) && (!force_quit)) {
        idx = (idx + 1) % BATCH_RING_SIZE;

        if (idx == sequencer_state.next_batch_ring_idx) {
            return NULL;
        }

        desc = &batch_ring[idx];
    }

    // Sanity check---this should never happen during normal operation
    if ((desc->batch_status != BATCH_SEQNUM_NEEDED) && (!force_quit)) {
        printf ("Incorrect transition to status BATCH_SEQNUM_NEEDED: wrong status\n");
        force_quit = 1;
        return NULL;
    }

    if (TRACK_REQUESTS) {
        // todo is this unlikely???
        if (track_client_req_id && track_added_to_batch && track_batch_idx == desc->batch_ring_idx) {
            track_now_timestamp = rte_rdtsc();
            fprintf(track_file, "get_next_unfulfilled_seq_request: %"PRIu64" %Lf\n",
                    track_now_timestamp, cycles_to_usec((track_now_timestamp - track_last_timestamp)));
            track_last_timestamp = track_now_timestamp;
        }
    }

    // Need to change this so the descriptor doesn't get re-selected
    desc->batch_status = BATCH_SEQNUM_REQUESTED;

    sequencer_state.next_batch_ring_idx = idx;
    if (force_quit) {
        return NULL;
    } else {
        return desc;
    }
}


void
send_packets_to_sequencer (void)
{
    send_packets (&sequencer_state);
}
/*--- END From sequencer_comm.c ------------------------------------------------*/


void
Init (ProgramArgs *pargs, int *pargc, char ***pargv)
{
    // Global variable for TCATS since there's only one thread
    pargs->thread_data = p;
}


void
LaunchThreads (ProgramArgs *pargs)
{
    // Copy relevant data to thread data structures and launch
    // threads
    int ret;

    // only one thread... 
    p->machineid = pargs->machineid;
    p->threadid = i;
    snprintf (p->threadname, 128, "[%s.%d]", pargs->machineid, i);

    // All clients will connect to the same server port
    p->host = pargs->host;
    p->tr = pargs->tr;
    p->program_state = startup;

    // For servers, this is how many clients from which to expect connections.
    // For clients, this is how many total server threads there are. 
    p->ncli = pargs->ncli;
    p->no_record = pargs->no_record;
    p->counter = 0;

    if (pargs->no_record) {
        printf ("Not recording measurements to file.\n");
    } else {    
        setup_filenames (&targs[i]);
        if (pargs->tr) {
            p->lbuf = (char *) malloc (LBUFSIZE);
            if (p->lbuf == NULL) {
                printf ("Couldn't allocate space for latency buffer! Exiting...\n");
                exit (-14);
            }
            p->lbuf_offset = 0;
            memset (p->lbuf, 0, LBUFSIZE);
        }
    }

    fflush (stdout);
}


void
generate_timestamp_str (char *pbuf)
{
    struct timespec sendtime;
    memset (pbuf, 0, PSIZE);

    sendtime = PreciseWhen ();
    snprintf (pbuf, PSIZE, "%lld,%.9ld",
            (long long) sendtime.tv_sec, sendtime.tv_nsec);
}


void
handle_received_timestamp (ThreadArgs *p, FILE *out, char *pbuf)
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
TimestampTxRx (__attribute__((unused)) void *dummy)
{
    // Send and then receive an echoed timestamp.
    // Return a negative value if error, else return 0.
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
    generate_timestamp_str (pbuf);

    // Client will be immediately put into experiment mode after 
    // startup phase
    while (p->program_state == experiment) {

        uint64_t max_acked_seqnum; 
        uint64_t last_acked_seqnum = 0;
        
        // calculates cycles per burst
        uint64_t hz = rte_get_tsc_hz ();

        // For sequencer timing
        uint64_t seq_period_tsc = (uint64_t) round(usec_to_cycles (hz, EPOCH) * MAX_TX_BURST / sequencer_state.cwnd);
        timer_state_t seq_timer; timer_init(&seq_timer, seq_period_tsc);

        // for dep_tracker timing, timer timing is the same
        timer_state_t dep_tracker_0_timer; timer_init(&dep_tracker_0_timer, 0);
        timer_state_t dep_tracker_1_timer; timer_init(&dep_tracker_1_timer, 0);
        timer_state_t dep_tracker_2_timer; timer_init(&dep_tracker_2_timer, 0);

        // Garbage collection timer information
        timer_state_t gc_timer; timer_init(&gc_timer, usec_to_cycles(hz, GC_PERIOD_USEC));

        // Timer resolution (check timers every timer_res_tsc cycles)
        timer_state__t timers_timer; timer_init(&timers_timer, usec_to_cycles (hz,
                    timer_resolution_us));

        // todo
    //    timer_state_t batch_timer; timer_init(&batch_timer, batch_timeout_tsc);

        // Stats reporting timer information
        timer_state_t stats_timer; timer_init(&stats_timer, hz);

        // Give the main thread some time to finish coming up
        sleep(1);

        // reset the sequencer timer so that we don't declare it dead
        // when we were just taking time to set up
        timer_reset(&seq_alive_timer);

        // loop and send a burst of packets every period_tsc
        // Also call timer manager to see if any timers have expired
        // Timer manager can be commented out to turn timers off
        while (!force_quit) {
            // todo we check this very often, don't check everytime?
            if (timer_check(&stats_timer)) {
                print_stats_periodically ();
            }

            // update timers based on the sending rate determined by cc
            seq_period_tsc = (uint64_t) round(MAX_TX_BURST * (hz / (sequencer_state.cwnd * CWND_MULTIPLIER)));
            timer_change_period(&seq_timer, seq_period_tsc);

            // Receive new packets
            receive_packets ();

            // first request gets min from dep tracker
            // then send to sequencer
            // sending to dep_tracker waits for more requests from clients,
            // then replies to previous clients and submits to dep_tracker?
            // Send packets
            if (timer_check(&seq_timer)) {
                send_packets_to_sequencer ();
            }

            // Check the timers
            if (timer_check(&timers_timer)) {
                rte_timer_manage (); 
            }
        }

        printf("lcore %u force quitting...\n", rte_lcore_id());

        return 0;
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
proxy_setup_rte (void) {
    int ret;
    
    int i;
    for (i = 0; i < RING_SIZE; i++) {
        rte_timer_init(&sequencer_state.pkt_timers[i]);
    }

	// # cycles between printing
	timer_period *= rte_get_tsc_hz(); // was timer_hz

	// create RX mbuf pool
    printf ("Setting up RX/TX mbuf pools...\n");
	seqcli_rx_pktmbuf_pool = rte_pktmbuf_pool_create("rx_pool", NB_MBUF,
													 MEMPOOL_CACHE_SIZE, 0, 
                                                     RTE_MBUF_DEFAULT_BUF_SIZE, 
                                                     rte_socket_id());
	if (seqcli_rx_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Couldn't init rx mbuf pool\n");

// create TX mbuf pool
	seqcli_tx_pktmbuf_pool = rte_pktmbuf_pool_create("tx_pool", NB_MBUF,
													 MEMPOOL_CACHE_SIZE, 0, 
                                                     RTE_MBUF_DEFAULT_BUF_SIZE, 
                                                     rte_socket_id());
	if (seqcli_tx_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Couldn't init tx mbuf pool\n");

    printf ("Checking Ethernet ports and configuring device...\n");
	// rte_eth_dev_configure must be called before any other Ethernet functions
	ret = rte_eth_dev_configure(seqcli_ethport, 1, 1, &port_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				 ret, (unsigned) seqcli_ethport);

	uint8_t nb_ports = rte_eth_dev_count();
	if (nb_ports != 1)
		rte_exit(EXIT_FAILURE, "Number of Ethernet ports is not 1!!!\n");

	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(seqcli_ethport, &dev_info);

	rte_eth_macaddr_get(seqcli_ethport, &eth_addr);
	// rte_eth_promiscuous_enable(seqcli_ethport);

    // check_all_ports_link_status (nb_ports, seqcli_enabled_port_mask);

    printf ("Setting up RX/TX queues...\n");
	// Set up the RX queue for our port
	fflush(stdout);
	ret = rte_eth_rx_queue_setup(seqcli_ethport, 0, nb_rxd,
								 rte_eth_dev_socket_id(seqcli_ethport),
								 NULL,
								 seqcli_rx_pktmbuf_pool);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				 ret, (unsigned) seqcli_ethport);

	/* init one TX queue on each port */
	fflush(stdout);
	ret = rte_eth_tx_queue_setup(seqcli_ethport, 0, nb_txd,
								 rte_eth_dev_socket_id(seqcli_ethport),
								 NULL);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				 ret, (unsigned) seqcli_ethport);

    printf ("Initializing TX buffer...\n");
	// Initialize the TX buffer todo should it be aligned? wasn't in l2fwd
	tx_buffer = rte_zmalloc_socket("tx_buffer", RTE_ETH_TX_BUFFER_SIZE(MAX_TX_BURST), 
                                    0, rte_eth_dev_socket_id(seqcli_ethport));
	if (tx_buffer == NULL)
		rte_exit(EXIT_FAILURE, "Couldn't allocate the tx buffer\n");

	rte_eth_tx_buffer_init(tx_buffer, MAX_TX_BURST);

    printf ("Setting error callback function...\n");
	// todo set an error callback function for dropped packets when sending?
	ret = rte_eth_dev_start(seqcli_ethport);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				 ret, (unsigned) seqcli_ethport);

    sleep (2);

    printf ("Launching RX/TX threads...\n");
    
    // Even-numbered CPUs for this NUMA node
	ret = rte_eal_remote_launch(TimestampTxRx, NULL, TX_CORE);
	if (ret < 0) {
		rte_exit (EXIT_FAILURE, "rte_eal_remote_launch: err=%d\n",
					ret);
	}
}

void
init_cc_state(cc_state_t *state, machine_id_t machine_id, uint8_t rpc_status, 
        struct ether_addr *addr, uint8_t dt_idx) 
{
    uint16_t i;

    // Create the template packet
    populate_header (&state->packet_template, addr);

    // Initialization of dep_tracker state
    for (i = 0; i < RING_SIZE; i++) {
        desc_t *desc = &state->request_ring[i];

        desc->cc_state = state;

        desc->payload.proxy_id = proxy_id;
        desc->payload.ring_idx = i;
        desc->payload.rpc_status = rpc_status; // can be used immediately
    }

    // Init sequencer state
    state->machine_id = machine_id;
    state->next_pkt_id = 0;
    state->cwnd = 1;
    state->ssthresh = 1000000;
    state->dupack = 0;
    state->highest_sent_pkt_id = 0;
    state->connection_state = SLOW_START;
    state->receiver = addr;
    state->dt_idx = dt_idx;

    // todo is this right?
    state->next_batch_ring_idx = 0;
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
        rte_eth_macaddr_get(seqcli_ethport, &eth_addr);

        hz = rte_get_tsc_hz ();
        core_frequency = rte_get_tsc_hz();

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

        printf ("\nSetting up RTE...\n");
        setup_rte_proxy ();

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


