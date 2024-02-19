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


static struct rte_mempool *pktmbuf_pool = NULL;
static struct rte_mempool *pktmbuf_pool_capture = NULL;

static int vlan_pair[2] = {1381, 1382};

static int stop = 0;

static rte_pcapng_t *pcapng_fd[128] = {NULL};

static int capture = 0;

static int port_init(uint16_t port)
{
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;
    int numaid = SOCKET_ID_ANY;

    printf("PORT INIT %d:\n", port);

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
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd, numaid, NULL, pktmbuf_pool);
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

    rte_eth_promiscuous_enable(port);

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

    while(1){

        time0 = rte_rdtsc();

        ms_curr = time0 * 1000ULL / tsc_per_sec;

        for(i=0;i<g_intf_cnt;i++){
            nb_rx = rte_eth_rx_burst(i, 0, bufs, BURST_SIZE);
            if(nb_rx){
                nb_tx = rte_eth_tx_burst((i == 1) ? 0 : 1, 0, bufs, nb_rx);
                if (unlikely(nb_tx < nb_rx)) {
                    for (k = nb_tx; k < nb_rx; k++){
                        rte_pktmbuf_free(bufs[k]);
                    }
                }
            }
        }

        if(ms_curr - ms_last > 5000){

            ms_last = ms_curr;

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

    pktmbuf_pool = rte_pktmbuf_pool_create(buff, 65535, MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
    if(pktmbuf_pool){
        printf("rte_pktmbuf_pool_create OK\n");
    }else{
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create\n");
    }

    pktmbuf_pool_capture = rte_pktmbuf_pool_create("capture", 65535, MEMPOOL_CACHE_SIZE, 0, rte_pcapng_mbuf_size(RTE_MBUF_DEFAULT_BUF_SIZE), SOCKET_ID_ANY);
    if(!pktmbuf_pool_capture){
        rte_exit(EXIT_FAILURE, "pktmbuf_pool_capture fail\n");
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

