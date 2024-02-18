#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_pcapng.h>

#define MEMPOOL_CACHE_SIZE 256
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define BURST_SIZE 32

static int g_numa_cnt = -1;
static int g_intf_cnt = 0;
static uint64_t tsc_per_sec;

struct intfs_list_t {
    int cnt;
    int intfs[128];
};

struct intfs_list_t cores2intfs[] = {
    {     2,  {0,1},     },
    {     2,  {0,1},     },
    {     2,  {2,3},     },
    {     2,  {2,3},     },
    {     2,  {4,5},     },
    {     2,  {4,5},     },
    {     2,  {6,7},     },
    {     2,  {6,7},     },
};

static int cores_per_intf[128] = {0};

static struct rte_mempool *pktmbuf_pool_per_numa[16] = {NULL};
static struct rte_mempool *pktmbuf_pool_capture = NULL;

static int vlan_pair[2] = {1381, 1382};

static int stop = 0;

static rte_pcapng_t *pcapng_fd[128] = {NULL};

static int capture = 0;

static int port_init(uint16_t port)
{
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = cores_per_intf[port], tx_rings = cores_per_intf[port];
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;
    int numaid = rte_eth_dev_socket_id(port);

    printf("PORT INIT %d: NUMA is %d\n", port, numaid);

    memset(&port_conf, 0, sizeof(struct rte_eth_conf));

    retval = rte_eth_dev_info_get(port, &dev_info);
    if(retval != 0) {
        printf("Error during getting device (port %u) info: %s\n", port, strerror(-retval));
        return retval;
    }

    if(dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
        printf("PORT INIT %d: RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE\n", port);
    }

    printf("PORT INIT %d: tx rx Q cnt %d\n", port, rx_rings);
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if(retval != 0) {
        printf("rte_eth_dev_configure err %d\n", port);
        return retval;
    }

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if(retval != 0) {
        return retval;
    }

    for(q = 0; q < rx_rings; q++) {
        printf("PORT INIT %d: rx Q %d: nb_rxd %d\n", port, q, nb_rxd);
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd, numaid, NULL, pktmbuf_pool_per_numa[numaid]);
        if(retval < 0) {
            return retval;
        }
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;

    for(q = 0; q < tx_rings; q++) {
        printf("PORT INIT %d: tx Q %d: nb_txd %d\n", port, q, nb_txd);
        retval = rte_eth_tx_queue_setup(port, q, nb_txd, numaid, &txconf);
        if(retval < 0) {
            return retval;
        }
    }

    retval = rte_eth_dev_start(port);
    if(retval < 0) {
        return retval;

    }

    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if(retval != 0) {
        return retval;
    }

    printf("PORT INIT %d: MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n", port, RTE_ETHER_ADDR_BYTES(&addr));

    retval = rte_eth_promiscuous_enable(port);

    if(retval != 0) {
        return retval;
    }

    return 0;
}

static int is_intf_in_core(int port_id, int core_idx)
{
    struct intfs_list_t *intfinfo = &cores2intfs[core_idx];
    int i;

    for(i=0;i<intfinfo->cnt;i++){
        if(intfinfo->intfs[i] == port_id){
            return 1;
        }
    }

    return 0;
}

static RTE_DEFINE_PER_LCORE(int, intf_to_rss[128]);

static int launch_one_lcore(__rte_unused void *dummy)
{
    int core_idx = rte_lcore_index(rte_lcore_id());
    int coreid = rte_lcore_id();
    int numaid = rte_socket_id();
    struct intfs_list_t *intfinfo;
    int i, j, k, busy;
    int curr_intf, rss;
    struct rte_mbuf *bufs[BURST_SIZE];
    struct rte_mbuf *mbuf_clones[BURST_SIZE];
    struct rte_mbuf *mc;
    uint16_t nb_rx;
    uint16_t nb_tx;
    uint64_t time0, time1, time2, timeend;
    uint64_t ms_last = 0;
    uint64_t ms_curr = 0;
    uint64_t time_all = 0, time_work = 0;
    uint64_t time_all_last = 0, time_work_last = 0;
    uint64_t cpu;
    char str[1024];
    struct rte_ether_hdr *eh;
    struct rte_vlan_hdr *vh;
    struct rte_eth_stats stats;
    int fd, ret;

    printf("CORE IDX %d: CoreID %d on socket %d\n", core_idx, coreid, numaid);

    intfinfo = &cores2intfs[core_idx];

    for(i=0;i<intfinfo->cnt;i++){
        curr_intf = intfinfo->intfs[i];

        rss = 0;
        for(j=0;j<core_idx;j++){
            if(is_intf_in_core(curr_intf, j)){
                rss++;
            }
        }

        RTE_PER_LCORE(intf_to_rss[curr_intf]) = rss;

        printf("CORE IDX %d: process interface %d on RSS %d\n", core_idx, curr_intf, RTE_PER_LCORE(intf_to_rss[curr_intf]));
    }

    sprintf(str, "capture-%d.pcapng", core_idx);
    fd = open(str, O_RDWR | O_CREAT, 0666);
    if(fd < 0){
        printf("CORE IDX %d: open capture err\n", core_idx);
        return 0;
    }

    pcapng_fd[core_idx] = rte_pcapng_fdopen(fd, NULL, NULL, NULL, NULL);
    if (!pcapng_fd[core_idx]) {
        printf("CORE IDX %d: rte_pcapng_fdopen err\n", core_idx);
        return 0;
    }

    ret = rte_pcapng_add_interface(pcapng_fd[core_idx], 0, NULL, NULL, NULL);
    if (ret < 0) {
        printf("CORE IDX %d: rte_pcapng_add_interface err\n", core_idx);
        return 0;
    }

    while(1){

        time0 = rte_rdtsc();

        ms_curr = time0 * 1000ULL / tsc_per_sec;

        for(i=0;i<intfinfo->cnt;i++){
            curr_intf = intfinfo->intfs[i];
            rss = RTE_PER_LCORE(intf_to_rss[curr_intf]);
            busy = 0;

            time1 = rte_rdtsc();
            nb_rx = rte_eth_rx_burst(curr_intf, rss, bufs, BURST_SIZE);
            if(likely(nb_rx)){
                busy = 1;

                // printf("CORE IDX %d: interface %d RSS %d rcv pkts %d\n", core_idx, curr_intf, rss, nb_rx);

                for(k=0;k<nb_rx;k++){

                    if(unlikely(capture)){
                        mc = rte_pcapng_copy(0, 0, bufs[k], pktmbuf_pool_capture, bufs[k]->pkt_len, 0, NULL);
                        if (mc){
                            mbuf_clones[0] = mc;
                            ret = rte_pcapng_write_packets(pcapng_fd[core_idx], mbuf_clones, 1);
                            rte_pktmbuf_free_bulk(mbuf_clones, 1);
                        }
                    }
/*
                    rte_get_ptype_name(bufs[k]->packet_type, str, sizeof(str));
                    printf("%s\n", str);
                    rte_pktmbuf_dump(stdout, bufs[k], bufs[k]->pkt_len);
*/
                    eh = rte_pktmbuf_mtod(bufs[k], struct rte_ether_hdr *);
                    if (eh->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN)){
                        vh = (struct rte_vlan_hdr *)(eh + 1);
                        // printf("----- VLAN %d\n", rte_cpu_to_be_16(vh->vlan_tci));
                        if(rte_cpu_to_be_16(vh->vlan_tci) == vlan_pair[0]){
                            vh->vlan_tci = rte_cpu_to_be_16(vlan_pair[1]);
                        }else if(rte_cpu_to_be_16(vh->vlan_tci) == vlan_pair[1]){
                            vh->vlan_tci = rte_cpu_to_be_16(vlan_pair[0]);
                        }
                    }else{
                    }
                }

                nb_tx = rte_eth_tx_burst(curr_intf, rss, bufs, nb_rx);

                if(unlikely(capture)){
                    for(k=0;k<nb_tx;k++){
                        mc = rte_pcapng_copy(0, 0, bufs[k], pktmbuf_pool_capture, bufs[k]->pkt_len, 0, NULL);
                        if (mc){
                            mbuf_clones[0] = mc;
                            ret = rte_pcapng_write_packets(pcapng_fd[core_idx], mbuf_clones, 1);
                            rte_pktmbuf_free_bulk(mbuf_clones, 1);
                        }
                    }
                }

                if (unlikely(nb_tx < nb_rx)) {
                    for (k = nb_tx; k < nb_rx; k++){
                        rte_pktmbuf_free(bufs[k]);
                    }
                }
            }
            if(busy){
                time_work += rte_rdtsc() - time1;
            }
        }

        time_all += rte_rdtsc() - time0;

        if(ms_curr - ms_last > 5000){
            cpu = (time_work - time_work_last) * 100ULL / (time_all - time_all_last);
            printf("CORE IDX %d: s %llu cpu %d\n", core_idx, ms_curr / 1000, cpu);
            ms_last = ms_curr;
            time_work_last = time_work;
            time_all_last = time_all;

            if(core_idx == 0){
                for(i=0;i<g_intf_cnt;i++){
                    memset(&stats, 0, sizeof(stats));
                    rte_eth_stats_get(i, &stats);
                    printf("INTF %d: %llu/%llu %llu %llu %llu %llu\n", i, stats.ipackets, stats.opackets, stats.imissed, stats.rx_nombuf, stats.ierrors, stats.oerrors);
                }
            }

            if(stop){
                break;
            }
        }
    }

    return 0;
}

static void sigint_handler(__rte_unused int signum)
{
    stop = 1;
    printf("stopping ...\n");
}

int main(int argc, char *argv[])
{
    struct rte_mempool * mbuf_pool;
    uint16_t portid;
    unsigned lcore_id;
    int i, j;
    struct intfs_list_t *intfs;
    char buff[32];
    int ret = rte_eal_init(argc, argv);

    if(ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }

    argc -= ret;
    argv += ret;

    tsc_per_sec = rte_get_tsc_hz();
    printf("TSC per sec %llu\n", tsc_per_sec);

    printf("Cores %d\n", rte_lcore_count());

    g_numa_cnt = rte_socket_count();
    printf("Sockets %d\n", g_numa_cnt);

    g_intf_cnt = rte_eth_dev_count_avail();
    printf("Intfs %d\n", g_intf_cnt);

    for(i=0;i<sizeof(cores2intfs)/sizeof(cores2intfs[0]);i++){
        intfs = &cores2intfs[i];
        for(j=0;j<intfs->cnt;j++){
            cores_per_intf[intfs->intfs[j]]++;
        }
    }

    for(i=0;i<g_numa_cnt;i++){
        sprintf(buff, "pktmbuf_per_numa_%d", i);
        pktmbuf_pool_per_numa[i] = rte_pktmbuf_pool_create(buff, 65535, MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, i);
        if(pktmbuf_pool_per_numa[i]){
            printf("rte_pktmbuf_pool_create for NUMA %d OK\n", i);
        }else{
            rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create %d\n", i);
        }
    }

    pktmbuf_pool_capture = rte_pktmbuf_pool_create("capture", 65535, MEMPOOL_CACHE_SIZE, 0, rte_pcapng_mbuf_size(RTE_MBUF_DEFAULT_BUF_SIZE), SOCKET_ID_ANY);
    if(!pktmbuf_pool_capture){
        rte_exit(EXIT_FAILURE, "pktmbuf_pool_capture fail\n");
    }

    RTE_ETH_FOREACH_DEV(portid){
        printf("Interface %d on socket %d has cores %d\n", portid, rte_eth_dev_socket_id(portid), cores_per_intf[portid]);
    }

    RTE_ETH_FOREACH_DEV(portid){
        if(port_init(portid)){
            rte_exit(EXIT_FAILURE, "port_init error %d\n", portid);
        }
    }

    signal(SIGINT, sigint_handler);

    rte_eal_mp_remote_launch(launch_one_lcore, NULL, CALL_MAIN);
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0) {
            break;
        }
    }

    for(i=0;i<128;i++){
        if(pcapng_fd[i]){
            rte_pcapng_close(pcapng_fd[i]);
        }
    }

    printf("--- exit ---\n");

    rte_eal_cleanup();

    return 0;
}

