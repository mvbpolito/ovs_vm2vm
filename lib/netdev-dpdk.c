/*
 * Copyright (c) 2014, 2015, 2016 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <config.h>
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "dirs.h"
#include "dp-packet.h"
#include "dpif-netdev.h"
#include "fatal-signal.h"
#include "netdev-dpdk.h"
#include "netdev-vport.h"
#include "odp-util.h"
#include "openvswitch/list.h"
#include "openvswitch/ofp-print.h"
#include "openvswitch/vlog.h"
#include "ovs-numa.h"
#include "ovs-thread.h"
#include "ovs-rcu.h"
#include "packets.h"
#include "shash.h"
#include "smap.h"
#include "sset.h"
#include "unaligned.h"
#include "timeval.h"
#include "unixctl.h"

#include "rte_config.h"
#include "rte_mbuf.h"
#include "rte_meter.h"
#include "rte_virtio_net.h"
#include "rte_pci.h"

VLOG_DEFINE_THIS_MODULE(dpdk);
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

#define DPDK_PORT_WATCHDOG_INTERVAL 5

#define OVS_CACHE_LINE_SIZE CACHE_LINE_SIZE
#define OVS_VPORT_DPDK "ovs_dpdk"

/*
 * need to reserve tons of extra space in the mbufs so we can align the
 * DMA addresses to 4KB.
 * The minimum mbuf size is limited to avoid scatter behaviour and drop in
 * performance for standard Ethernet MTU.
 */
#define ETHER_HDR_MAX_LEN           (ETHER_HDR_LEN + ETHER_CRC_LEN + (2 * VLAN_HEADER_LEN))
#define MTU_TO_FRAME_LEN(mtu)       ((mtu) + ETHER_HDR_LEN + ETHER_CRC_LEN)
#define MTU_TO_MAX_FRAME_LEN(mtu)   ((mtu) + ETHER_HDR_MAX_LEN)
#define FRAME_LEN_TO_MTU(frame_len) ((frame_len)- ETHER_HDR_LEN - ETHER_CRC_LEN)
#define MBUF_SIZE(mtu)              ( MTU_TO_MAX_FRAME_LEN(mtu)   \
                                    + sizeof(struct dp_packet)    \
                                    + RTE_PKTMBUF_HEADROOM)
#define NETDEV_DPDK_MBUF_ALIGN      1024

/* Max and min number of packets in the mempool.  OVS tries to allocate a
 * mempool with MAX_NB_MBUF: if this fails (because the system doesn't have
 * enough hugepages) we keep halving the number until the allocation succeeds
 * or we reach MIN_NB_MBUF */

#define MAX_NB_MBUF          (4096 * 64)
#define MIN_NB_MBUF          (4096 * 4)
#define MP_CACHE_SZ          RTE_MEMPOOL_CACHE_MAX_SIZE

/* MAX_NB_MBUF can be divided by 2 many times, until MIN_NB_MBUF */
BUILD_ASSERT_DECL(MAX_NB_MBUF % ROUND_DOWN_POW2(MAX_NB_MBUF/MIN_NB_MBUF) == 0);

/* The smallest possible NB_MBUF that we're going to try should be a multiple
 * of MP_CACHE_SZ. This is advised by DPDK documentation. */
BUILD_ASSERT_DECL((MAX_NB_MBUF / ROUND_DOWN_POW2(MAX_NB_MBUF/MIN_NB_MBUF))
                  % MP_CACHE_SZ == 0);

#define SOCKET0              0

#define NIC_PORT_RX_Q_SIZE 2048  /* Size of Physical NIC RX Queue, Max (n+32<=4096)*/
#define NIC_PORT_TX_Q_SIZE 2048  /* Size of Physical NIC TX Queue, Max (n+32<=4096)*/

#define DIRECT_LINK_NAME_FORMAT "ring_%d_%d"
#define DIRECT_PORT_NAME_FORMAT "port_%d_%d"

#define UNIVERSAL_NODE_ADDRESS "127.0.0.1"
#define UNIVERSAL_NODE_PORT     8080
#define UNIVERSAL_NODE_URL_ATTACH "/attach/"
#define UNIVERSAL_NODE_URL_DETACH "/detach/"
#define UNIVERSAL_NODE_URL_SEND_DPDK "/send_dpdk/"

#define PLUG_PORT_JSON_FORMAT  "{ \n"                       \
                               "\"port\":\"%s\",\n"         \
                               "\"id\":\"%s\",\n"           \
                               "\"type\":\"%s\",\n"         \
                               "\"device\":\"%s\"\n"        \
                               "}"

#define UNPLUG_PORT_JSON_FORMAT     "{ \n"                      \
                                    "\"port\":\"%s\",\n"        \
                                    "\"id\":\"%s\",\n"          \
                                    "}"

#define DPDK_SEND_JSON_FORMAT  "{ \n"                   \
                               "\"port\":\"%s\",\n"     \
                               "\"command\":\"%s\"\n"   \
                               "}"

#define CHANGE_PORTS_JSON_FORMAT "old=%s,new=%s"

#define ADD_SLAVE_FORMAT "action=add,old=%s,new=%s"
#define DEL_SLAVE_FORMAT "action=del,old=%s"

#define OVS_VHOST_MAX_QUEUE_NUM 1024  /* Maximum number of vHost TX queues. */
#define OVS_VHOST_QUEUE_MAP_UNKNOWN (-1) /* Mapping not initialized. */
#define OVS_VHOST_QUEUE_DISABLED    (-2) /* Queue was disabled by guest and not
                                          * yet mapped to another queue. */

static char *cuse_dev_name = NULL;    /* Character device cuse_dev_name. */
static char *vhost_sock_dir = NULL;   /* Location of vhost-user sockets */

/*
 * Maximum amount of time in micro seconds to try and enqueue to vhost.
 */
#define VHOST_ENQ_RETRY_USECS 100

static const struct rte_eth_conf port_conf = {
    .rxmode = {
        //.mq_mode = ETH_MQ_RX_RSS,
        .split_hdr_size = 0,
        .header_split   = 0, /* Header Split disabled */
        .hw_ip_checksum = 0, /* IP checksum offload disabled */
        .hw_vlan_filter = 0, /* VLAN filtering disabled */
        .jumbo_frame    = 0, /* Jumbo Frame Support disabled */
        .hw_strip_crc   = 0,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = ETH_RSS_IP | ETH_RSS_UDP | ETH_RSS_TCP,
        },
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

enum { MAX_TX_QUEUE_LEN = 384 };
enum { DPDK_RING_SIZE = 256 };
BUILD_ASSERT_DECL(IS_POW2(DPDK_RING_SIZE));
enum { DRAIN_TSC = 200000ULL };

enum dpdk_dev_type {
    DPDK_DEV_ETH = 0,
    DPDK_DEV_VHOST = 1,
};

static int rte_eal_init_ret = ENODEV;

static struct ovs_mutex dpdk_mutex = OVS_MUTEX_INITIALIZER;

/* Quality of Service */

/* An instance of a QoS configuration.  Always associated with a particular
 * network device.
 *
 * Each QoS implementation subclasses this with whatever additional data it
 * needs.
 */
struct qos_conf {
    const struct dpdk_qos_ops *ops;
};

/* A particular implementation of dpdk QoS operations.
 *
 * The functions below return 0 if successful or a positive errno value on
 * failure, except where otherwise noted. All of them must be provided, except
 * where otherwise noted.
 */
struct dpdk_qos_ops {

    /* Name of the QoS type */
    const char *qos_name;

    /* Called to construct the QoS implementation on 'netdev'. The
     * implementation should make the appropriate calls to configure QoS
     * according to 'details'. The implementation may assume that any current
     * QoS configuration already installed should be destroyed before
     * constructing the new configuration.
     *
     * The contents of 'details' should be documented as valid for 'ovs_name'
     * in the "other_config" column in the "QoS" table in vswitchd/vswitch.xml
     * (which is built as ovs-vswitchd.conf.db(8)).
     *
     * This function must return 0 if and only if it sets 'netdev->qos_conf'
     * to an initialized 'struct qos_conf'.
     *
     * For all QoS implementations it should always be non-null.
     */
    int (*qos_construct)(struct netdev *netdev, const struct smap *details);

    /* Destroys the data structures allocated by the implementation as part of
     * 'qos_conf.
     *
     * For all QoS implementations it should always be non-null.
     */
    void (*qos_destruct)(struct netdev *netdev, struct qos_conf *conf);

    /* Retrieves details of 'netdev->qos_conf' configuration into 'details'.
     *
     * The contents of 'details' should be documented as valid for 'ovs_name'
     * in the "other_config" column in the "QoS" table in vswitchd/vswitch.xml
     * (which is built as ovs-vswitchd.conf.db(8)).
     */
    int (*qos_get)(const struct netdev *netdev, struct smap *details);

    /* Reconfigures 'netdev->qos_conf' according to 'details', performing any
     * required calls to complete the reconfiguration.
     *
     * The contents of 'details' should be documented as valid for 'ovs_name'
     * in the "other_config" column in the "QoS" table in vswitchd/vswitch.xml
     * (which is built as ovs-vswitchd.conf.db(8)).
     *
     * This function may be null if 'qos_conf' is not configurable.
     */
    int (*qos_set)(struct netdev *netdev, const struct smap *details);

    /* Modify an array of rte_mbufs. The modification is specific to
     * each qos implementation.
     *
     * The function should take and array of mbufs and an int representing
     * the current number of mbufs present in the array.
     *
     * After the function has performed a qos modification to the array of
     * mbufs it returns an int representing the number of mbufs now present in
     * the array. This value is can then be passed to the port send function
     * along with the modified array for transmission.
     *
     * For all QoS implementations it should always be non-null.
     */
    int (*qos_run)(struct netdev *netdev, struct rte_mbuf **pkts,
                           int pkt_cnt);
};

/* dpdk_qos_ops for each type of user space QoS implementation */
static const struct dpdk_qos_ops egress_policer_ops;

/*
 * Array of dpdk_qos_ops, contains pointer to all supported QoS
 * operations.
 */
static const struct dpdk_qos_ops *const qos_confs[] = {
    &egress_policer_ops,
    NULL
};

/* Contains all 'struct dpdk_dev's. */
static struct ovs_list dpdk_list OVS_GUARDED_BY(dpdk_mutex)
    = OVS_LIST_INITIALIZER(&dpdk_list);

static struct ovs_list dpdk_mp_list OVS_GUARDED_BY(dpdk_mutex)
    = OVS_LIST_INITIALIZER(&dpdk_mp_list);

struct vf_info {
    uint8_t vf_id;              /* virtual function id */
    struct rte_pci_addr addr;   /* pci address of the virtual function */
    bool available;
};

/* This mutex must be used by non pmd threads when allocating or freeing
 * mbufs through mempools. Since dpdk_queue_pkts() and dpdk_queue_flush() may
 * use mempools, a non pmd thread should hold this mutex while calling them */
static struct ovs_mutex nonpmd_mempool_mutex = OVS_MUTEX_INITIALIZER;

struct dpdk_mp {
    struct rte_mempool *mp;
    int mtu;
    int socket_id;
    int refcount;
    struct ovs_list list_node OVS_GUARDED_BY(dpdk_mutex);
};

/* There should be one 'struct dpdk_tx_queue' created for
 * each cpu core. */
struct dpdk_tx_queue {
    bool flush_tx;                 /* Set to true to flush queue everytime */
                                   /* pkts are queued. */
    int count;
    rte_spinlock_t tx_lock;        /* Protects the members and the NIC queue
                                    * from concurrent access.  It is used only
                                    * if the queue is shared among different
                                    * pmd threads (see 'txq_needs_locking'). */
    int map;                       /* Mapping of configured vhost-user queues
                                    * to enabled by guest. */
    uint64_t tsc;
    struct rte_mbuf *burst_pkts[MAX_TX_QUEUE_LEN];
};

/* dpdk has no way to remove dpdk ring ethernet devices
   so we have to keep them around once they've been created
*/

static struct ovs_list dpdk_ring_list OVS_GUARDED_BY(dpdk_mutex)
    = OVS_LIST_INITIALIZER(&dpdk_ring_list);

/* it should be a union */
struct dpdkr_direct_link {
    struct rte_ring *rings[2];  /* rte_rings that communicates both VMs */
    struct vf_info *vf_info;    /* vf_info that is being used */
};

struct dpdk_direct_link {
    struct dpdk_ring *ring;
};

struct dpdk_ring {
    /* For the client rings */
    struct rte_ring *cring_tx;
    struct rte_ring *cring_rx;
    unsigned int user_port_id; /* User given port no, parsed from port name */
    int eth_port_id; /* ethernet device port id */
    struct pmd_internals *internals; /* pmd_internals structure on the guest */
    struct dpdkr_direct_link *direct;  /* if set, the port is direct */
    struct ovs_list list_node OVS_GUARDED_BY(dpdk_mutex);
};

struct netdev_dpdk {
    struct netdev up;
    int port_id;
    int max_packet_len;
    enum dpdk_dev_type type;

    struct dpdk_tx_queue *tx_q;

    struct ovs_mutex mutex OVS_ACQ_AFTER(dpdk_mutex);

    struct dpdk_mp *dpdk_mp;
    int mtu;
    int socket_id;
    int buf_size;
    struct netdev_stats stats;
    /* Protects stats */
    rte_spinlock_t stats_lock;

    struct eth_addr hwaddr;
    enum netdev_flags flags;

    struct rte_eth_link link;
    int link_reset_cnt;

    /* The user might request more txqs than the NIC has.  We remap those
     * ('up.n_txq') on these ('real_n_txq').
     * If the numbers match, 'txq_needs_locking' is false, otherwise it is
     * true and we will take a spinlock on transmission */
    int real_n_txq;
    int real_n_rxq;
    bool txq_needs_locking;

    /* virtio-net structure for vhost device */
    OVSRCU_TYPE(struct virtio_net *) virtio_dev;

    /* Identifier used to distinguish vhost devices from each other */
    char vhost_id[PATH_MAX];

    /* In dpdk_list. */
    struct ovs_list list_node OVS_GUARDED_BY(dpdk_mutex);

    /* QoS configuration and lock for the device */
    struct qos_conf *qos_conf;
    rte_spinlock_t qos_lock;

    /* The following properties cannot be changed when a device is running,
     * so we remember the request and update them next time
     * netdev_dpdk*_reconfigure() is called */
    int requested_n_txq;
    int requested_n_rxq;

    struct dpdk_direct_link *direct; /* if set the port is direct */

    /* vector containing all virtual functions info */
    struct vf_info *vf_info;
    uint16_t n_vfs;
    uint16_t pf_pool;   /* physical function pool XXX: should be n_vfs*/
};

struct netdev_rxq_dpdk {
    struct netdev_rxq up;
    int port_id;
};

static bool dpdk_thread_is_pmd(void);

static int netdev_dpdk_construct(struct netdev *);

struct virtio_net * netdev_dpdk_get_virtio(const struct netdev_dpdk *dev);

static bool
is_dpdk_class(const struct netdev_class *class)
{
    return class->construct == netdev_dpdk_construct;
}

/* DPDK NIC drivers allocate RX buffers at a particular granularity, typically
 * aligned at 1k or less. If a declared mbuf size is not a multiple of this
 * value, insufficient buffers are allocated to accomodate the packet in its
 * entirety. Furthermore, certain drivers need to ensure that there is also
 * sufficient space in the Rx buffer to accommodate two VLAN tags (for QinQ
 * frames). If the RX buffer is too small, then the driver enables scatter RX
 * behaviour, which reduces performance. To prevent this, use a buffer size that
 * is closest to 'mtu', but which satisfies the aforementioned criteria.
 */
static uint32_t
dpdk_buf_size(int mtu)
{
    return ROUND_UP((MTU_TO_MAX_FRAME_LEN(mtu) + RTE_PKTMBUF_HEADROOM),
                     NETDEV_DPDK_MBUF_ALIGN);
}

/* XXX: use dpdk malloc for entire OVS. in fact huge page should be used
 * for all other segments data, bss and text. */

static void *
dpdk_rte_mzalloc(size_t sz)
{
    void *ptr;

    ptr = rte_zmalloc(OVS_VPORT_DPDK, sz, OVS_CACHE_LINE_SIZE);
    if (ptr == NULL) {
        out_of_memory();
    }
    return ptr;
}

/* XXX this function should be called only by pmd threads (or by non pmd
 * threads holding the nonpmd_mempool_mutex) */
void
free_dpdk_buf(struct dp_packet *p)
{
    struct rte_mbuf *pkt = (struct rte_mbuf *) p;

    rte_pktmbuf_free(pkt);
}

static void
ovs_rte_pktmbuf_init(struct rte_mempool *mp,
                     void *opaque_arg OVS_UNUSED,
                     void *_m,
                     unsigned i OVS_UNUSED)
{
    struct rte_mbuf *m = _m;

    rte_pktmbuf_init(mp, opaque_arg, _m, i);

    dp_packet_init_dpdk((struct dp_packet *) m, m->buf_len);
}

static struct dpdk_mp *
dpdk_mp_get(int socket_id, int mtu) OVS_REQUIRES(dpdk_mutex)
{
    struct dpdk_mp *dmp = NULL;
    char mp_name[RTE_MEMPOOL_NAMESIZE];
    unsigned mp_size;
    struct rte_pktmbuf_pool_private mbp_priv;

    LIST_FOR_EACH (dmp, list_node, &dpdk_mp_list) {
        if (dmp->socket_id == socket_id && dmp->mtu == mtu) {
            dmp->refcount++;
            return dmp;
        }
    }

    dmp = dpdk_rte_mzalloc(sizeof *dmp);
    dmp->socket_id = socket_id;
    dmp->mtu = mtu;
    dmp->refcount = 1;
    mbp_priv.mbuf_data_room_size = MBUF_SIZE(mtu) - sizeof(struct dp_packet);
    mbp_priv.mbuf_priv_size = sizeof (struct dp_packet) - sizeof (struct rte_mbuf);

    mp_size = MAX_NB_MBUF;
    do {
        if (snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "ovs_mp_%d_%d_%u",
                     dmp->mtu, dmp->socket_id, mp_size) < 0) {
            return NULL;
        }

        dmp->mp = rte_mempool_create(mp_name, mp_size, MBUF_SIZE(mtu),
                                     MP_CACHE_SZ,
                                     sizeof(struct rte_pktmbuf_pool_private),
                                     rte_pktmbuf_pool_init, &mbp_priv,
                                     ovs_rte_pktmbuf_init, NULL,
                                     socket_id, 0);
    } while (!dmp->mp && rte_errno == ENOMEM && (mp_size /= 2) >= MIN_NB_MBUF);

    if (dmp->mp == NULL) {
        return NULL;
    } else {
        VLOG_DBG("Allocated \"%s\" mempool with %u mbufs", mp_name, mp_size );
    }

    ovs_list_push_back(&dpdk_mp_list, &dmp->list_node);
    return dmp;
}

static void
dpdk_mp_put(struct dpdk_mp *dmp)
{

    if (!dmp) {
        return;
    }

    dmp->refcount--;
    ovs_assert(dmp->refcount >= 0);

#if 0
    /* I could not find any API to destroy mp. */
    if (dmp->refcount == 0) {
        list_delete(dmp->list_node);
        /* destroy mp-pool. */
    }
#endif
}

static void
check_link_status(struct netdev_dpdk *dev)
{
    struct rte_eth_link link;

    rte_eth_link_get_nowait(dev->port_id, &link);

    if (dev->link.link_status != link.link_status) {
        netdev_change_seq_changed(&dev->up);

        dev->link_reset_cnt++;
        dev->link = link;
        if (dev->link.link_status) {
            VLOG_DBG_RL(&rl, "Port %d Link Up - speed %u Mbps - %s",
                        dev->port_id, (unsigned)dev->link.link_speed,
                        (dev->link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
                         ("full-duplex") : ("half-duplex"));
        } else {
            VLOG_DBG_RL(&rl, "Port %d Link Down", dev->port_id);
        }
    }
}

static void *
dpdk_watchdog(void *dummy OVS_UNUSED)
{
    struct netdev_dpdk *dev;

    pthread_detach(pthread_self());

    for (;;) {
        ovs_mutex_lock(&dpdk_mutex);
        LIST_FOR_EACH (dev, list_node, &dpdk_list) {
            ovs_mutex_lock(&dev->mutex);
            check_link_status(dev);
            ovs_mutex_unlock(&dev->mutex);
        }
        ovs_mutex_unlock(&dpdk_mutex);
        xsleep(DPDK_PORT_WATCHDOG_INTERVAL);
    }

    return NULL;
}

static struct dpdk_ring *
look_dpdkr_for_port_no(unsigned int port_no)
{
    struct dpdk_ring *ring;
    /* look through our list to find the device */
    LIST_FOR_EACH (ring, list_node, &dpdk_ring_list) {
         if (ring->user_port_id == port_no) {
            return ring;
         }
    }

    return NULL;
}

static struct dpdk_ring *
look_dpdkr_for_port_id(int port_id)
{
    struct dpdk_ring *ring;
    /* look through our list to find the device */
    LIST_FOR_EACH (ring, list_node, &dpdk_ring_list) {
         if (ring->eth_port_id == port_id) {
            return ring;
         }
    }

    return NULL;
}

static int
dpdk_eth_dev_queue_setup(struct netdev_dpdk *dev, int n_rxq, int n_txq)
{
    int diag = 0;
    int i;

    /* A device may report more queues than it makes available (this has
     * been observed for Intel xl710, which reserves some of them for
     * SRIOV):  rte_eth_*_queue_setup will fail if a queue is not
     * available.  When this happens we can retry the configuration
     * and request less queues */
    while (n_rxq && n_txq) {
        if (diag) {
            VLOG_INFO("Retrying setup with (rxq:%d txq:%d)", n_rxq, n_txq);
        }

        diag = rte_eth_dev_configure(dev->port_id, n_rxq, n_txq, &port_conf);
        if (diag) {
            break;
        }

        for (i = 0; i < n_txq; i++) {
            diag = rte_eth_tx_queue_setup(dev->port_id, i, NIC_PORT_TX_Q_SIZE,
                                          dev->socket_id, NULL);
            if (diag) {
                VLOG_INFO("Interface %s txq(%d) setup error: %s",
                          dev->up.name, i, rte_strerror(-diag));
                break;
            }
        }

        if (i != n_txq) {
            /* Retry with less tx queues */
            n_txq = i;
            continue;
        }

        for (i = 0; i < n_rxq; i++) {
            diag = rte_eth_rx_queue_setup(dev->port_id, i, NIC_PORT_RX_Q_SIZE,
                                          dev->socket_id, NULL,
                                          dev->dpdk_mp->mp);
            if (diag) {
                VLOG_INFO("Interface %s rxq(%d) setup error: %s",
                          dev->up.name, i, rte_strerror(-diag));
                break;
            }
        }

        if (i != n_rxq) {
            /* Retry with less rx queues */
            n_rxq = i;
            continue;
        }

        dev->up.n_rxq = n_rxq;
        dev->real_n_txq = n_txq;

        return 0;
    }

    return diag;
}


static int
dpdk_eth_dev_init(struct netdev_dpdk *dev) OVS_REQUIRES(dpdk_mutex)
{
    struct rte_pktmbuf_pool_private *mbp_priv;
    struct rte_eth_dev_info info;
    struct ether_addr eth_addr;
    int diag;
    int n_rxq, n_txq;

    if (dev->port_id < 0 || dev->port_id >= rte_eth_dev_count()) {
        return ENODEV;
    }

    rte_eth_dev_info_get(dev->port_id, &info);

    n_rxq = MIN(info.max_rx_queues, dev->up.n_rxq);
    n_txq = MIN(info.max_tx_queues, dev->up.n_txq);

    diag = dpdk_eth_dev_queue_setup(dev, n_rxq, n_txq);
    if (diag) {
        VLOG_ERR("Interface %s(rxq:%d txq:%d) configure error: %s",
                 dev->up.name, n_rxq, n_txq, rte_strerror(-diag));
        return -diag;
    }

    if (info.max_vfs) {
        dev->vf_info = malloc(info.max_vfs * sizeof(*dev->vf_info));
        dev->n_vfs = info.max_vfs;
        dev->pf_pool = info.max_vfs;    /* XXX: only valid when 1 queue per vf */
        struct rte_pci_addr base_addr = info.pci_dev->addr;
        int i;
        for (i = 0; i < info.max_vfs; i++) {

            struct rte_pci_addr addr;
            addr.domain = base_addr.domain;
            addr.bus = base_addr.bus;
            addr.devid = 0x10;  /* is this value always correct? */
            addr.function = base_addr.function + 2*i;   /* XXX: it is not valid for al the cases */

            dev->vf_info[i].vf_id = i;
            dev->vf_info[i].addr = addr;
            dev->vf_info[i].available = true;
        }

    } else {
        dev->vf_info = NULL;
    }

    diag = rte_eth_dev_start(dev->port_id);
    if (diag) {
        VLOG_ERR("Interface %s start error: %s", dev->up.name,
                 rte_strerror(-diag));
        return -diag;
    }

    rte_eth_promiscuous_enable(dev->port_id);
    rte_eth_allmulticast_enable(dev->port_id);

    memset(&eth_addr, 0x0, sizeof(eth_addr));
    rte_eth_macaddr_get(dev->port_id, &eth_addr);
    VLOG_INFO_RL(&rl, "Port %d: "ETH_ADDR_FMT"",
                    dev->port_id, ETH_ADDR_BYTES_ARGS(eth_addr.addr_bytes));

    memcpy(dev->hwaddr.ea, eth_addr.addr_bytes, ETH_ADDR_LEN);
    rte_eth_link_get_nowait(dev->port_id, &dev->link);

    mbp_priv = rte_mempool_get_priv(dev->dpdk_mp->mp);
    dev->buf_size = mbp_priv->mbuf_data_room_size - RTE_PKTMBUF_HEADROOM;

    dev->flags = NETDEV_UP | NETDEV_PROMISC;
    return 0;
}

static struct netdev_dpdk *
netdev_dpdk_cast(const struct netdev *netdev)
{
    return CONTAINER_OF(netdev, struct netdev_dpdk, up);
}

static struct netdev *
netdev_dpdk_alloc(void)
{
    struct netdev_dpdk *dev = dpdk_rte_mzalloc(sizeof *dev);
    return &dev->up;
}

static void
netdev_dpdk_alloc_txq(struct netdev_dpdk *dev, unsigned int n_txqs)
{
    unsigned i;

    dev->tx_q = dpdk_rte_mzalloc(n_txqs * sizeof *dev->tx_q);
    for (i = 0; i < n_txqs; i++) {
        int numa_id = ovs_numa_get_numa_id(i);

        if (!dev->txq_needs_locking) {
            /* Each index is considered as a cpu core id, since there should
             * be one tx queue for each cpu core.  If the corresponding core
             * is not on the same numa node as 'dev', flags the
             * 'flush_tx'. */
            dev->tx_q[i].flush_tx = dev->socket_id == numa_id;
        } else {
            /* Queues are shared among CPUs. Always flush */
            dev->tx_q[i].flush_tx = true;
        }

        /* Initialize map for vhost devices. */
        dev->tx_q[i].map = OVS_VHOST_QUEUE_MAP_UNKNOWN;
        rte_spinlock_init(&dev->tx_q[i].tx_lock);
    }
}

static int
netdev_dpdk_init(struct netdev *netdev, unsigned int port_no,
                 enum dpdk_dev_type type)
    OVS_REQUIRES(dpdk_mutex)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int sid;
    int err = 0;
    uint32_t buf_size;

    ovs_mutex_init(&dev->mutex);
    ovs_mutex_lock(&dev->mutex);

    rte_spinlock_init(&dev->stats_lock);

    /* If the 'sid' is negative, it means that the kernel fails
     * to obtain the pci numa info.  In that situation, always
     * use 'SOCKET0'. */
    if (type == DPDK_DEV_ETH) {
        sid = rte_eth_dev_socket_id(port_no);
    } else {
        sid = rte_lcore_to_socket_id(rte_get_master_lcore());
    }

    dev->socket_id = sid < 0 ? SOCKET0 : sid;
    dev->port_id = port_no;
    dev->type = type;
    dev->flags = 0;
    dev->mtu = ETHER_MTU;
    dev->max_packet_len = MTU_TO_FRAME_LEN(dev->mtu);

    buf_size = dpdk_buf_size(dev->mtu);
    dev->dpdk_mp = dpdk_mp_get(dev->socket_id, FRAME_LEN_TO_MTU(buf_size));
    if (!dev->dpdk_mp) {
        err = ENOMEM;
        goto unlock;
    }

    /* Initialise QoS configuration to NULL and qos lock to unlocked */
    dev->qos_conf = NULL;
    rte_spinlock_init(&dev->qos_lock);

    netdev->n_txq = NR_QUEUE;
    netdev->n_rxq = NR_QUEUE;
    dev->requested_n_rxq = NR_QUEUE;
    dev->requested_n_txq = NR_QUEUE;
    dev->real_n_txq = NR_QUEUE;

    if (type == DPDK_DEV_ETH) {
        netdev_dpdk_alloc_txq(dev, NR_QUEUE);
        err = dpdk_eth_dev_init(dev);
        if (err) {
            goto unlock;
        }
    } else {
        netdev_dpdk_alloc_txq(dev, OVS_VHOST_MAX_QUEUE_NUM);
    }

    ovs_list_push_back(&dpdk_list, &dev->list_node);

unlock:
    if (err) {
        rte_free(dev->tx_q);
    }
    ovs_mutex_unlock(&dev->mutex);
    return err;
}

/* dev_name must be the prefix followed by a positive decimal number.
 * (no leading + or - signs are allowed) */
static int
dpdk_dev_parse_name(const char dev_name[], const char prefix[],
                    unsigned int *port_no)
{
    const char *cport;

    if (strncmp(dev_name, prefix, strlen(prefix))) {
        return ENODEV;
    }

    cport = dev_name + strlen(prefix);

    if (str_to_uint(cport, 10, port_no)) {
        return 0;
    } else {
        return ENODEV;
    }
}

static int
vhost_construct_helper(struct netdev *netdev) OVS_REQUIRES(dpdk_mutex)
{
    if (rte_eal_init_ret) {
        return rte_eal_init_ret;
    }

    return netdev_dpdk_init(netdev, -1, DPDK_DEV_VHOST);
}

static int
netdev_dpdk_vhost_cuse_construct(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int err;

    ovs_mutex_lock(&dpdk_mutex);
    strncpy(dev->vhost_id, netdev->name, sizeof(dev->vhost_id));
    err = vhost_construct_helper(netdev);
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static int
netdev_dpdk_vhost_user_construct(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    const char *name = netdev->name;
    int err;

    /* 'name' is appended to 'vhost_sock_dir' and used to create a socket in
     * the file system. '/' or '\' would traverse directories, so they're not
     * acceptable in 'name'. */
    if (strchr(name, '/') || strchr(name, '\\')) {
        VLOG_ERR("\"%s\" is not a valid name for a vhost-user port. "
                 "A valid name must not include '/' or '\\'",
                 name);
        return EINVAL;
    }

    ovs_mutex_lock(&dpdk_mutex);
    /* Take the name of the vhost-user port and append it to the location where
     * the socket is to be created, then register the socket.
     */
    snprintf(dev->vhost_id, sizeof(dev->vhost_id), "%s/%s",
             vhost_sock_dir, name);

    err = rte_vhost_driver_register(dev->vhost_id);
    if (err) {
        VLOG_ERR("vhost-user socket device setup failure for socket %s\n",
                 dev->vhost_id);
    } else {
        fatal_signal_add_file_to_unlink(dev->vhost_id);
        VLOG_INFO("Socket %s created for vhost-user port %s\n",
                  dev->vhost_id, name);
        err = vhost_construct_helper(netdev);
    }

    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static int
netdev_dpdk_construct(struct netdev *netdev)
{
    unsigned int port_no;
    int err;

    if (rte_eal_init_ret) {
        return rte_eal_init_ret;
    }

    /* Names always start with "dpdk" */
    err = dpdk_dev_parse_name(netdev->name, "dpdk", &port_no);
    if (err) {
        return err;
    }

    ovs_mutex_lock(&dpdk_mutex);
    err = netdev_dpdk_init(netdev, port_no, DPDK_DEV_ETH);
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static void
netdev_dpdk_destruct(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    rte_eth_dev_stop(dev->port_id);
    ovs_mutex_unlock(&dev->mutex);

    ovs_mutex_lock(&dpdk_mutex);
    rte_free(dev->tx_q);
    ovs_list_remove(&dev->list_node);
    dpdk_mp_put(dev->dpdk_mp);
    ovs_mutex_unlock(&dpdk_mutex);
}

static void
netdev_dpdk_vhost_destruct(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    /* Guest becomes an orphan if still attached. */
    if (netdev_dpdk_get_virtio(dev) != NULL) {
        VLOG_ERR("Removing port '%s' while vhost device still attached.",
                 netdev->name);
        VLOG_ERR("To restore connectivity after re-adding of port, VM on socket"
                 " '%s' must be restarted.",
                 dev->vhost_id);
    }

    if (rte_vhost_driver_unregister(dev->vhost_id)) {
        VLOG_ERR("Unable to remove vhost-user socket %s", dev->vhost_id);
    } else {
        fatal_signal_remove_file_to_unlink(dev->vhost_id);
    }

    ovs_mutex_lock(&dpdk_mutex);
    rte_free(dev->tx_q);
    ovs_list_remove(&dev->list_node);
    dpdk_mp_put(dev->dpdk_mp);
    ovs_mutex_unlock(&dpdk_mutex);
}

static void
netdev_dpdk_dealloc(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    rte_free(dev);
}

static int
netdev_dpdk_get_config(const struct netdev *netdev, struct smap *args)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);

    smap_add_format(args, "requested_rx_queues", "%d", dev->requested_n_rxq);
    smap_add_format(args, "configured_rx_queues", "%d", netdev->n_rxq);
    smap_add_format(args, "requested_tx_queues", "%d", netdev->n_txq);
    smap_add_format(args, "configured_tx_queues", "%d", dev->real_n_txq);
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_set_config(struct netdev *netdev, const struct smap *args)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int new_n_rxq;

    ovs_mutex_lock(&dev->mutex);
    new_n_rxq = MAX(smap_get_int(args, "n_rxq", dev->requested_n_rxq), 1);
    if (new_n_rxq != dev->requested_n_rxq) {
        dev->requested_n_rxq = new_n_rxq;
        netdev_request_reconfigure(netdev);
    }
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_numa_id(const struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    return dev->socket_id;
}

/* Sets the number of tx queues for the dpdk interface. */
static int
netdev_dpdk_set_tx_multiq(struct netdev *netdev, unsigned int n_txq)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);

    if (dev->requested_n_txq == n_txq) {
        goto out;
    }

    dev->requested_n_txq = n_txq;
    netdev_request_reconfigure(netdev);

out:
    ovs_mutex_unlock(&dev->mutex);
    return 0;
}

static struct netdev_rxq *
netdev_dpdk_rxq_alloc(void)
{
    struct netdev_rxq_dpdk *rx = dpdk_rte_mzalloc(sizeof *rx);

    return &rx->up;
}

static struct netdev_rxq_dpdk *
netdev_rxq_dpdk_cast(const struct netdev_rxq *rxq)
{
    return CONTAINER_OF(rxq, struct netdev_rxq_dpdk, up);
}

static int
netdev_dpdk_rxq_construct(struct netdev_rxq *rxq)
{
    struct netdev_rxq_dpdk *rx = netdev_rxq_dpdk_cast(rxq);
    struct netdev_dpdk *dev = netdev_dpdk_cast(rxq->netdev);

    ovs_mutex_lock(&dev->mutex);
    rx->port_id = dev->port_id;
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static void
netdev_dpdk_rxq_destruct(struct netdev_rxq *rxq OVS_UNUSED)
{
}

static void
netdev_dpdk_rxq_dealloc(struct netdev_rxq *rxq)
{
    struct netdev_rxq_dpdk *rx = netdev_rxq_dpdk_cast(rxq);

    rte_free(rx);
}

static inline void
dpdk_queue_flush__(struct netdev_dpdk *dev, int qid)
{
    struct dpdk_tx_queue *txq = &dev->tx_q[qid];
    uint32_t nb_tx = 0;

    while (nb_tx != txq->count) {
        uint32_t ret;

        ret = rte_eth_tx_burst(dev->port_id, qid, txq->burst_pkts + nb_tx,
                               txq->count - nb_tx);
        if (!ret) {
            break;
        }

        nb_tx += ret;
    }

    if (OVS_UNLIKELY(nb_tx != txq->count)) {
        /* free buffers, which we couldn't transmit, one at a time (each
         * packet could come from a different mempool) */
        int i;

        for (i = nb_tx; i < txq->count; i++) {
            rte_pktmbuf_free(txq->burst_pkts[i]);
        }
        rte_spinlock_lock(&dev->stats_lock);
        dev->stats.tx_dropped += txq->count-nb_tx;
        rte_spinlock_unlock(&dev->stats_lock);
    }

    txq->count = 0;
    txq->tsc = rte_get_timer_cycles();
}

static inline void
dpdk_queue_flush(struct netdev_dpdk *dev, int qid)
{
    struct dpdk_tx_queue *txq = &dev->tx_q[qid];

    if (txq->count == 0) {
        return;
    }
    dpdk_queue_flush__(dev, qid);
}

static bool
is_vhost_running(struct virtio_net *virtio_dev)
{
    return (virtio_dev != NULL && (virtio_dev->flags & VIRTIO_DEV_RUNNING));
}

static inline void
netdev_dpdk_vhost_update_rx_counters(struct netdev_stats *stats,
                                     struct dp_packet **packets, int count)
{
    int i;
    struct dp_packet *packet;

    stats->rx_packets += count;
    for (i = 0; i < count; i++) {
        packet = packets[i];

        if (OVS_UNLIKELY(dp_packet_size(packet) < ETH_HEADER_LEN)) {
            /* This only protects the following multicast counting from
             * too short packets, but it does not stop the packet from
             * further processing. */
            stats->rx_errors++;
            stats->rx_length_errors++;
            continue;
        }

        struct eth_header *eh = (struct eth_header *) dp_packet_data(packet);
        if (OVS_UNLIKELY(eth_addr_is_multicast(eh->eth_dst))) {
            stats->multicast++;
        }

        stats->rx_bytes += dp_packet_size(packet);
    }
}

/*
 * The receive path for the vhost port is the TX path out from guest.
 */
static int
netdev_dpdk_vhost_rxq_recv(struct netdev_rxq *rxq,
                           struct dp_packet **packets, int *c)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(rxq->netdev);
    struct virtio_net *virtio_dev = netdev_dpdk_get_virtio(dev);
    int qid = rxq->queue_id;
    uint16_t nb_rx = 0;

    if (OVS_UNLIKELY(!is_vhost_running(virtio_dev))) {
        return EAGAIN;
    }

    if (rxq->queue_id >= dev->real_n_rxq) {
        return EOPNOTSUPP;
    }

    nb_rx = rte_vhost_dequeue_burst(virtio_dev, qid * VIRTIO_QNUM + VIRTIO_TXQ,
                                    dev->dpdk_mp->mp,
                                    (struct rte_mbuf **)packets,
                                    NETDEV_MAX_BURST);
    if (!nb_rx) {
        return EAGAIN;
    }

    rte_spinlock_lock(&dev->stats_lock);
    netdev_dpdk_vhost_update_rx_counters(&dev->stats, packets, nb_rx);
    rte_spinlock_unlock(&dev->stats_lock);

    *c = (int) nb_rx;
    return 0;
}

static int
netdev_dpdk_rxq_recv(struct netdev_rxq *rxq, struct dp_packet **packets,
                     int *c)
{
    struct netdev_rxq_dpdk *rx = netdev_rxq_dpdk_cast(rxq);
    struct netdev_dpdk *dev = netdev_dpdk_cast(rxq->netdev);
    int nb_rx;

    /* There is only one tx queue for this core.  Do not flush other
     * queues.
     * Do not flush tx queue which is shared among CPUs
     * since it is always flushed */
    if (rxq->queue_id == rte_lcore_id() &&
        OVS_LIKELY(!dev->txq_needs_locking)) {
        dpdk_queue_flush(dev, rxq->queue_id);
    }

    nb_rx = rte_eth_rx_burst(rx->port_id, rxq->queue_id,
                             (struct rte_mbuf **) packets,
                             NETDEV_MAX_BURST);
    if (!nb_rx) {
        return EAGAIN;
    }

    *c = nb_rx;

    return 0;
}

static inline int
netdev_dpdk_qos_run__(struct netdev_dpdk *dev, struct rte_mbuf **pkts,
                        int cnt)
{
    struct netdev *netdev = &dev->up;

    if (dev->qos_conf != NULL) {
        rte_spinlock_lock(&dev->qos_lock);
        if (dev->qos_conf != NULL) {
            cnt = dev->qos_conf->ops->qos_run(netdev, pkts, cnt);
        }
        rte_spinlock_unlock(&dev->qos_lock);
    }

    return cnt;
}

static inline void
netdev_dpdk_vhost_update_tx_counters(struct netdev_stats *stats,
                                     struct dp_packet **packets,
                                     int attempted,
                                     int dropped)
{
    int i;
    int sent = attempted - dropped;

    stats->tx_packets += sent;
    stats->tx_dropped += dropped;

    for (i = 0; i < sent; i++) {
        stats->tx_bytes += dp_packet_size(packets[i]);
    }
}

static void
__netdev_dpdk_vhost_send(struct netdev *netdev, int qid,
                         struct dp_packet **pkts, int cnt,
                         bool may_steal)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct virtio_net *virtio_dev = netdev_dpdk_get_virtio(dev);
    struct rte_mbuf **cur_pkts = (struct rte_mbuf **) pkts;
    unsigned int total_pkts = cnt;
    unsigned int qos_pkts = cnt;
    uint64_t start = 0;

    qid = dev->tx_q[qid % dev->real_n_txq].map;

    if (OVS_UNLIKELY(!is_vhost_running(virtio_dev) || qid < 0)) {
        rte_spinlock_lock(&dev->stats_lock);
        dev->stats.tx_dropped+= cnt;
        rte_spinlock_unlock(&dev->stats_lock);
        goto out;
    }

    rte_spinlock_lock(&dev->tx_q[qid].tx_lock);

    /* Check has QoS has been configured for the netdev */
    cnt = netdev_dpdk_qos_run__(dev, cur_pkts, cnt);
    qos_pkts -= cnt;

    do {
        int vhost_qid = qid * VIRTIO_QNUM + VIRTIO_RXQ;
        unsigned int tx_pkts;

        tx_pkts = rte_vhost_enqueue_burst(virtio_dev, vhost_qid,
                                          cur_pkts, cnt);
        if (OVS_LIKELY(tx_pkts)) {
            /* Packets have been sent.*/
            cnt -= tx_pkts;
            /* Prepare for possible next iteration.*/
            cur_pkts = &cur_pkts[tx_pkts];
        } else {
            uint64_t timeout = VHOST_ENQ_RETRY_USECS * rte_get_timer_hz() / 1E6;
            unsigned int expired = 0;

            if (!start) {
                start = rte_get_timer_cycles();
            }

            /*
             * Unable to enqueue packets to vhost interface.
             * Check available entries before retrying.
             */
            while (!rte_vring_available_entries(virtio_dev, vhost_qid)) {
                if (OVS_UNLIKELY((rte_get_timer_cycles() - start) > timeout)) {
                    expired = 1;
                    break;
                }
            }
            if (expired) {
                /* break out of main loop. */
                break;
            }
        }
    } while (cnt);

    rte_spinlock_unlock(&dev->tx_q[qid].tx_lock);

    rte_spinlock_lock(&dev->stats_lock);
    cnt += qos_pkts;
    netdev_dpdk_vhost_update_tx_counters(&dev->stats, pkts, total_pkts, cnt);
    rte_spinlock_unlock(&dev->stats_lock);

out:
    if (may_steal) {
        int i;

        for (i = 0; i < total_pkts; i++) {
            dp_packet_delete(pkts[i]);
        }
    }
}

inline static void
dpdk_queue_pkts(struct netdev_dpdk *dev, int qid,
               struct rte_mbuf **pkts, int cnt)
{
    struct dpdk_tx_queue *txq = &dev->tx_q[qid];
    uint64_t diff_tsc;

    int i = 0;

    while (i < cnt) {
        int freeslots = MAX_TX_QUEUE_LEN - txq->count;
        int tocopy = MIN(freeslots, cnt-i);

        memcpy(&txq->burst_pkts[txq->count], &pkts[i],
               tocopy * sizeof (struct rte_mbuf *));

        txq->count += tocopy;
        i += tocopy;

        if (txq->count == MAX_TX_QUEUE_LEN || txq->flush_tx) {
            dpdk_queue_flush__(dev, qid);
        }
        diff_tsc = rte_get_timer_cycles() - txq->tsc;
        if (diff_tsc >= DRAIN_TSC) {
            dpdk_queue_flush__(dev, qid);
        }
    }
}

/* Tx function. Transmit packets indefinitely */
static void
dpdk_do_tx_copy(struct netdev *netdev, int qid, struct dp_packet **pkts,
                int cnt)
    OVS_NO_THREAD_SAFETY_ANALYSIS
{
#if !defined(__CHECKER__) && !defined(_WIN32)
    const size_t PKT_ARRAY_SIZE = cnt;
#else
    /* Sparse or MSVC doesn't like variable length array. */
    enum { PKT_ARRAY_SIZE = NETDEV_MAX_BURST };
#endif
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct rte_mbuf *mbufs[PKT_ARRAY_SIZE];
    int dropped = 0;
    int newcnt = 0;
    int i;

    /* If we are on a non pmd thread we have to use the mempool mutex, because
     * every non pmd thread shares the same mempool cache */

    if (!dpdk_thread_is_pmd()) {
        ovs_mutex_lock(&nonpmd_mempool_mutex);
    }

    for (i = 0; i < cnt; i++) {
        int size = dp_packet_size(pkts[i]);

        if (OVS_UNLIKELY(size > dev->max_packet_len)) {
            VLOG_WARN_RL(&rl, "Too big size %d max_packet_len %d",
                         (int)size , dev->max_packet_len);

            dropped++;
            continue;
        }

        mbufs[newcnt] = rte_pktmbuf_alloc(dev->dpdk_mp->mp);

        if (!mbufs[newcnt]) {
            dropped += cnt - i;
            break;
        }

        /* We have to do a copy for now */
        memcpy(rte_pktmbuf_mtod(mbufs[newcnt], void *), dp_packet_data(pkts[i]), size);

        rte_pktmbuf_data_len(mbufs[newcnt]) = size;
        rte_pktmbuf_pkt_len(mbufs[newcnt]) = size;

        newcnt++;
    }

    if (dev->type == DPDK_DEV_VHOST) {
        __netdev_dpdk_vhost_send(netdev, qid, (struct dp_packet **) mbufs, newcnt, true);
    } else {
        unsigned int qos_pkts = newcnt;

        /* Check if QoS has been configured for this netdev. */
        newcnt = netdev_dpdk_qos_run__(dev, mbufs, newcnt);

        dropped += qos_pkts - newcnt;
        dpdk_queue_pkts(dev, qid, mbufs, newcnt);
        dpdk_queue_flush(dev, qid);
    }

    if (OVS_UNLIKELY(dropped)) {
        rte_spinlock_lock(&dev->stats_lock);
        dev->stats.tx_dropped += dropped;
        rte_spinlock_unlock(&dev->stats_lock);
    }

    if (!dpdk_thread_is_pmd()) {
        ovs_mutex_unlock(&nonpmd_mempool_mutex);
    }
}

static int
netdev_dpdk_vhost_send(struct netdev *netdev, int qid, struct dp_packet **pkts,
                 int cnt, bool may_steal)
{
    if (OVS_UNLIKELY(pkts[0]->source != DPBUF_DPDK)) {
        int i;

        dpdk_do_tx_copy(netdev, qid, pkts, cnt);
        if (may_steal) {
            for (i = 0; i < cnt; i++) {
                dp_packet_delete(pkts[i]);
            }
        }
    } else {
        __netdev_dpdk_vhost_send(netdev, qid, pkts, cnt, may_steal);
    }
    return 0;
}

static inline void
netdev_dpdk_send__(struct netdev_dpdk *dev, int qid,
                   struct dp_packet **pkts, int cnt, bool may_steal)
{
    int i;

    if (OVS_UNLIKELY(dev->txq_needs_locking)) {
        qid = qid % dev->real_n_txq;
        rte_spinlock_lock(&dev->tx_q[qid].tx_lock);
    }

    if (OVS_UNLIKELY(!may_steal ||
                     pkts[0]->source != DPBUF_DPDK)) {
        struct netdev *netdev = &dev->up;

        dpdk_do_tx_copy(netdev, qid, pkts, cnt);

        if (may_steal) {
            for (i = 0; i < cnt; i++) {
                dp_packet_delete(pkts[i]);
            }
        }
    } else {
        int next_tx_idx = 0;
        int dropped = 0;
        unsigned int qos_pkts = 0;
        unsigned int temp_cnt = 0;

        for (i = 0; i < cnt; i++) {
            int size = dp_packet_size(pkts[i]);

            if (OVS_UNLIKELY(size > dev->max_packet_len)) {
                if (next_tx_idx != i) {
                    temp_cnt = i - next_tx_idx;
                    qos_pkts = temp_cnt;

                    temp_cnt = netdev_dpdk_qos_run__(dev, (struct rte_mbuf**)pkts,
                                                temp_cnt);
                    dropped += qos_pkts - temp_cnt;
                    dpdk_queue_pkts(dev, qid,
                                    (struct rte_mbuf **)&pkts[next_tx_idx],
                                    temp_cnt);

                }

                VLOG_WARN_RL(&rl, "Too big size %d max_packet_len %d",
                             (int)size , dev->max_packet_len);

                dp_packet_delete(pkts[i]);
                dropped++;
                next_tx_idx = i + 1;
            }
        }
        if (next_tx_idx != cnt) {
            cnt -= next_tx_idx;
            qos_pkts = cnt;

            cnt = netdev_dpdk_qos_run__(dev, (struct rte_mbuf**)pkts, cnt);
            dropped += qos_pkts - cnt;
            dpdk_queue_pkts(dev, qid, (struct rte_mbuf **)&pkts[next_tx_idx],
                            cnt);
        }

        if (OVS_UNLIKELY(dropped)) {
            rte_spinlock_lock(&dev->stats_lock);
            dev->stats.tx_dropped += dropped;
            rte_spinlock_unlock(&dev->stats_lock);
        }
    }

    if (OVS_UNLIKELY(dev->txq_needs_locking)) {
        rte_spinlock_unlock(&dev->tx_q[qid].tx_lock);
    }
}

static int
netdev_dpdk_eth_send(struct netdev *netdev, int qid,
                     struct dp_packet **pkts, int cnt, bool may_steal)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    netdev_dpdk_send__(dev, qid, pkts, cnt, may_steal);
    return 0;
}

static int
netdev_dpdk_set_etheraddr(struct netdev *netdev, const struct eth_addr mac)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    if (!eth_addr_equals(dev->hwaddr, mac)) {
        dev->hwaddr = mac;
        netdev_change_seq_changed(netdev);
    }
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_etheraddr(const struct netdev *netdev, struct eth_addr *mac)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    *mac = dev->hwaddr;
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_mtu(const struct netdev *netdev, int *mtup)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    *mtup = dev->mtu;
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_set_mtu(const struct netdev *netdev, int mtu)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int old_mtu, err, dpdk_mtu;
    struct dpdk_mp *old_mp;
    struct dpdk_mp *mp;
    uint32_t buf_size;

    ovs_mutex_lock(&dpdk_mutex);
    ovs_mutex_lock(&dev->mutex);
    if (dev->mtu == mtu) {
        err = 0;
        goto out;
    }

    buf_size = dpdk_buf_size(mtu);
    dpdk_mtu = FRAME_LEN_TO_MTU(buf_size);

    mp = dpdk_mp_get(dev->socket_id, dpdk_mtu);
    if (!mp) {
        err = ENOMEM;
        goto out;
    }

    rte_eth_dev_stop(dev->port_id);

    old_mtu = dev->mtu;
    old_mp = dev->dpdk_mp;
    dev->dpdk_mp = mp;
    dev->mtu = mtu;
    dev->max_packet_len = MTU_TO_FRAME_LEN(dev->mtu);

    err = dpdk_eth_dev_init(dev);
    if (err) {
        dpdk_mp_put(mp);
        dev->mtu = old_mtu;
        dev->dpdk_mp = old_mp;
        dev->max_packet_len = MTU_TO_FRAME_LEN(dev->mtu);
        dpdk_eth_dev_init(dev);
        goto out;
    }

    dpdk_mp_put(old_mp);
    netdev_change_seq_changed(netdev);
out:
    ovs_mutex_unlock(&dev->mutex);
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static int
netdev_dpdk_get_carrier(const struct netdev *netdev, bool *carrier);

static int
netdev_dpdk_vhost_get_stats(const struct netdev *netdev,
                            struct netdev_stats *stats)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    memset(stats, 0, sizeof(*stats));
    /* Unsupported Stats */
    stats->collisions = UINT64_MAX;
    stats->rx_crc_errors = UINT64_MAX;
    stats->rx_fifo_errors = UINT64_MAX;
    stats->rx_frame_errors = UINT64_MAX;
    stats->rx_missed_errors = UINT64_MAX;
    stats->rx_over_errors = UINT64_MAX;
    stats->tx_aborted_errors = UINT64_MAX;
    stats->tx_carrier_errors = UINT64_MAX;
    stats->tx_errors = UINT64_MAX;
    stats->tx_fifo_errors = UINT64_MAX;
    stats->tx_heartbeat_errors = UINT64_MAX;
    stats->tx_window_errors = UINT64_MAX;
    stats->rx_dropped += UINT64_MAX;

    rte_spinlock_lock(&dev->stats_lock);
    /* Supported Stats */
    stats->rx_packets += dev->stats.rx_packets;
    stats->tx_packets += dev->stats.tx_packets;
    stats->tx_dropped += dev->stats.tx_dropped;
    stats->multicast = dev->stats.multicast;
    stats->rx_bytes = dev->stats.rx_bytes;
    stats->tx_bytes = dev->stats.tx_bytes;
    stats->rx_errors = dev->stats.rx_errors;
    stats->rx_length_errors = dev->stats.rx_length_errors;
    rte_spinlock_unlock(&dev->stats_lock);

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_stats(const struct netdev *netdev, struct netdev_stats *stats)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct rte_eth_stats rte_stats;
    bool gg;

    netdev_dpdk_get_carrier(netdev, &gg);
    ovs_mutex_lock(&dev->mutex);
    rte_eth_stats_get(dev->port_id, &rte_stats);

    memset(stats, 0, sizeof(*stats));

    stats->rx_packets = rte_stats.ipackets;
    stats->tx_packets = rte_stats.opackets;
    stats->rx_bytes = rte_stats.ibytes;
    stats->tx_bytes = rte_stats.obytes;
    /* DPDK counts imissed as errors, but count them here as dropped instead */
    stats->rx_errors = rte_stats.ierrors - rte_stats.imissed;
    stats->tx_errors = rte_stats.oerrors;
    stats->multicast = rte_stats.imcasts;

    rte_spinlock_lock(&dev->stats_lock);
    stats->tx_dropped = dev->stats.tx_dropped;
    rte_spinlock_unlock(&dev->stats_lock);

    /* These are the available DPDK counters for packets not received due to
     * local resource constraints in DPDK and NIC respectively. */
    stats->rx_dropped = rte_stats.rx_nombuf + rte_stats.imissed;
    stats->collisions = UINT64_MAX;

    stats->rx_length_errors = UINT64_MAX;
    stats->rx_over_errors = UINT64_MAX;
    stats->rx_crc_errors = UINT64_MAX;
    stats->rx_frame_errors = UINT64_MAX;
    stats->rx_fifo_errors = UINT64_MAX;
    stats->rx_missed_errors = rte_stats.imissed;

    stats->tx_aborted_errors = UINT64_MAX;
    stats->tx_carrier_errors = UINT64_MAX;
    stats->tx_fifo_errors = UINT64_MAX;
    stats->tx_heartbeat_errors = UINT64_MAX;
    stats->tx_window_errors = UINT64_MAX;

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

int netdev_dpdk_get_bypass_stats(const struct netdev *netdev,
                                    struct netdev_stats *stats)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct dpdk_ring *ring;

    ring = look_dpdkr_for_port_id(dev->port_id);

    if (!ring)
        return -1;

    ovs_mutex_lock(&dev->mutex);

    struct pmd_internals *internal = ring->internals;

    memset(stats, 0, sizeof(*stats));

    unsigned i;
    uint64_t rx_total = 0, tx_total = 0, tx_err_total = 0;

    for (i = 0; i < internal->nb_rx_queues; i++) {
        rx_total += internal->rx_ring_queues[i].rx_pkts_bypass;
    }

    for (i = 0; i < internal->nb_tx_queues; i++) {
        tx_total += internal->tx_ring_queues[i].tx_pkts_bypass;
        tx_err_total += internal->tx_ring_queues[i].err_pkts_bypass;
    }

    /*
     * Currently statistics are taken from the same side of the port,
     * would it make sense to take them from the other side?
     */
    stats->tx_packets = rx_total;
    stats->rx_packets = tx_total;
    stats->rx_bytes = UINT64_MAX;   /* XXX: to be done */
    stats->tx_bytes = UINT64_MAX;   /* XXX: to be done */

    stats->tx_errors = UINT64_MAX;
    stats->rx_errors = tx_err_total;
    stats->multicast = UINT64_MAX;
    stats->tx_dropped = UINT64_MAX;

    stats->rx_dropped = UINT64_MAX;
    stats->collisions = UINT64_MAX;

    stats->rx_length_errors = UINT64_MAX;
    stats->rx_over_errors = UINT64_MAX;
    stats->rx_crc_errors = UINT64_MAX;
    stats->rx_frame_errors = UINT64_MAX;
    stats->rx_fifo_errors = UINT64_MAX;
    stats->rx_missed_errors = UINT64_MAX;

    stats->tx_aborted_errors = UINT64_MAX;
    stats->tx_carrier_errors = UINT64_MAX;
    stats->tx_fifo_errors = UINT64_MAX;
    stats->tx_heartbeat_errors = UINT64_MAX;
    stats->tx_window_errors = UINT64_MAX;

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}


static int
netdev_dpdk_ring_get_stats(const struct netdev *netdev, struct netdev_stats *stats)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct dpdk_ring *ring;
    int err;

    ring = look_dpdkr_for_port_id(dev->port_id);

    /*
     * if the ring is not direct then get the local stats
     */

    err = netdev_dpdk_get_stats(netdev, stats);
    if (err)
        return err;

    /* dev->stats represents the packets that were sent using the bypass device */
    stats->rx_packets += dev->stats.rx_packets;
    stats->tx_packets += dev->stats.tx_packets;
    stats->rx_bytes += dev->stats.rx_bytes;
    stats->tx_bytes += dev->stats.tx_bytes;
    stats->tx_errors += dev->stats.tx_errors;

    if (ring->direct) {
        struct netdev_stats bypass_stats;
        netdev_dpdk_get_bypass_stats(netdev, &bypass_stats);

        stats->rx_packets += bypass_stats.rx_packets;
        stats->tx_packets += bypass_stats.tx_packets;
        stats->rx_bytes += bypass_stats.rx_bytes;
        stats->tx_bytes += bypass_stats.tx_bytes;
        stats->tx_errors += bypass_stats.tx_errors;
    }
    return 0;
}


static int
netdev_dpdk_get_features(const struct netdev *netdev_,
                         enum netdev_features *current,
                         enum netdev_features *advertised OVS_UNUSED,
                         enum netdev_features *supported OVS_UNUSED,
                         enum netdev_features *peer OVS_UNUSED)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);
    struct rte_eth_link link;

    ovs_mutex_lock(&dev->mutex);
    link = dev->link;
    ovs_mutex_unlock(&dev->mutex);

    if (link.link_duplex == ETH_LINK_AUTONEG_DUPLEX) {
        if (link.link_speed == ETH_LINK_SPEED_AUTONEG) {
            *current = NETDEV_F_AUTONEG;
        }
    } else if (link.link_duplex == ETH_LINK_HALF_DUPLEX) {
        if (link.link_speed == ETH_LINK_SPEED_10) {
            *current = NETDEV_F_10MB_HD;
        }
        if (link.link_speed == ETH_LINK_SPEED_100) {
            *current = NETDEV_F_100MB_HD;
        }
        if (link.link_speed == ETH_LINK_SPEED_1000) {
            *current = NETDEV_F_1GB_HD;
        }
    } else if (link.link_duplex == ETH_LINK_FULL_DUPLEX) {
        if (link.link_speed == ETH_LINK_SPEED_10) {
            *current = NETDEV_F_10MB_FD;
        }
        if (link.link_speed == ETH_LINK_SPEED_100) {
            *current = NETDEV_F_100MB_FD;
        }
        if (link.link_speed == ETH_LINK_SPEED_1000) {
            *current = NETDEV_F_1GB_FD;
        }
        if (link.link_speed == ETH_LINK_SPEED_10000) {
            *current = NETDEV_F_10GB_FD;
        }
    }

    return 0;
}

static int
netdev_dpdk_get_ifindex(const struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int ifindex;

    ovs_mutex_lock(&dev->mutex);
    ifindex = dev->port_id;
    ovs_mutex_unlock(&dev->mutex);

    return ifindex;
}

static int
netdev_dpdk_get_carrier(const struct netdev *netdev, bool *carrier)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    check_link_status(dev);
    *carrier = dev->link.link_status;

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_vhost_get_carrier(const struct netdev *netdev, bool *carrier)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct virtio_net *virtio_dev = netdev_dpdk_get_virtio(dev);

    ovs_mutex_lock(&dev->mutex);

    if (is_vhost_running(virtio_dev)) {
        *carrier = 1;
    } else {
        *carrier = 0;
    }

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static long long int
netdev_dpdk_get_carrier_resets(const struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    long long int carrier_resets;

    ovs_mutex_lock(&dev->mutex);
    carrier_resets = dev->link_reset_cnt;
    ovs_mutex_unlock(&dev->mutex);

    return carrier_resets;
}

static int
netdev_dpdk_set_miimon(struct netdev *netdev OVS_UNUSED,
                       long long int interval OVS_UNUSED)
{
    return EOPNOTSUPP;
}

static int
netdev_dpdk_update_flags__(struct netdev_dpdk *dev,
                           enum netdev_flags off, enum netdev_flags on,
                           enum netdev_flags *old_flagsp) OVS_REQUIRES(dev->mutex)
{
    int err;

    if ((off | on) & ~(NETDEV_UP | NETDEV_PROMISC)) {
        return EINVAL;
    }

    *old_flagsp = dev->flags;
    dev->flags |= on;
    dev->flags &= ~off;

    if (dev->flags == *old_flagsp) {
        return 0;
    }

    if (dev->type == DPDK_DEV_ETH) {
        if (dev->flags & NETDEV_UP) {
            err = rte_eth_dev_start(dev->port_id);
            if (err)
                return -err;
        }

        if (dev->flags & NETDEV_PROMISC) {
            rte_eth_promiscuous_enable(dev->port_id);
        }

        if (!(dev->flags & NETDEV_UP)) {
            rte_eth_dev_stop(dev->port_id);
        }
    }

    return 0;
}

static int
netdev_dpdk_update_flags(struct netdev *netdev,
                         enum netdev_flags off, enum netdev_flags on,
                         enum netdev_flags *old_flagsp)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int error;

    ovs_mutex_lock(&dev->mutex);
    error = netdev_dpdk_update_flags__(dev, off, on, old_flagsp);
    ovs_mutex_unlock(&dev->mutex);

    return error;
}

static int
netdev_dpdk_get_status(const struct netdev *netdev, struct smap *args)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct rte_eth_dev_info dev_info;

    if (dev->port_id < 0)
        return ENODEV;

    ovs_mutex_lock(&dev->mutex);
    rte_eth_dev_info_get(dev->port_id, &dev_info);
    ovs_mutex_unlock(&dev->mutex);

    smap_add_format(args, "driver_name", "%s", dev_info.driver_name);

    smap_add_format(args, "port_no", "%d", dev->port_id);
    smap_add_format(args, "numa_id", "%d", rte_eth_dev_socket_id(dev->port_id));
    smap_add_format(args, "driver_name", "%s", dev_info.driver_name);
    smap_add_format(args, "min_rx_bufsize", "%u", dev_info.min_rx_bufsize);
    smap_add_format(args, "max_rx_pktlen", "%u", dev->max_packet_len);
    smap_add_format(args, "max_rx_queues", "%u", dev_info.max_rx_queues);
    smap_add_format(args, "max_tx_queues", "%u", dev_info.max_tx_queues);
    smap_add_format(args, "max_mac_addrs", "%u", dev_info.max_mac_addrs);
    smap_add_format(args, "max_hash_mac_addrs", "%u", dev_info.max_hash_mac_addrs);
    smap_add_format(args, "max_vfs", "%u", dev_info.max_vfs);
    smap_add_format(args, "max_vmdq_pools", "%u", dev_info.max_vmdq_pools);

    if (dev_info.pci_dev) {
        smap_add_format(args, "pci-vendor_id", "0x%u",
                        dev_info.pci_dev->id.vendor_id);
        smap_add_format(args, "pci-device_id", "0x%x",
                        dev_info.pci_dev->id.device_id);
    }

    return 0;
}

static int
write_to_orchestrator(const char * buf, char * answer)
{
    int sd;
    struct sockaddr_in serv_addr;
    int n;
    const char * buf_ptr;

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if(sd < 0)
    {
        VLOG_ERR("Error creating socket\n");
        return -1;
    }

    memset((void *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;

    if(inet_pton(AF_INET, UNIVERSAL_NODE_ADDRESS, &serv_addr.sin_addr) <= 0) {
        VLOG_ERR("Error converting UniversalNode Address\n");
        return -1;
    }

    serv_addr.sin_port = htons(UNIVERSAL_NODE_PORT);

    if(connect(sd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        VLOG_ERR("Error connecting to UniversalNode\n");
        return -1;
    }

    n = strlen(buf);
    buf_ptr = &buf[0];
    do {
        int ret = write(sd, buf_ptr, n);
        if(ret < 0) {
            VLOG_ERR("Error sending data over socket to UniversalNode\n");
            return -1;
        }

        n -= ret;
        buf_ptr += ret;
    } while(n > 0);

    char * ans_ptr = answer;
    char total = 0;
    do {
        int n = read(sd, ans_ptr, 1024 - total);
        if(n < 0)
            break;
        ans_ptr += n;
        total += n;
    } while(n != 0);

    close(sd);

    return 0;
}

/* XXX:
 * this function is totally unsafe, there is not any length check, then buffer
 * overflows are possible everywhere
 */
static int
send_command_to_vm(const char *url, const char *cmd, char *answer) {

    char buf[4096] = {0};   /* quite big due to the overhead of http and json */
    char tmp[1024] = {0};
    int err;
    int httpr;

    snprintf(tmp, 1024, "PUT %s HTTP/1.1\r\n", url);
    strncat(buf, tmp, 1024);

    snprintf(tmp, 1024, "Host: %s:%hu\r\n", UNIVERSAL_NODE_ADDRESS, UNIVERSAL_NODE_PORT);
    strncat(buf, tmp, 1024);

    strcat(buf, "Connection: close\r\n");
    strcat(buf, "Accept: */*\r\n");

    snprintf(tmp, 1024, "Content-Length: %"PRIuSIZE"\r\n", strlen(cmd));
    strncat(buf, tmp, 1024);

    strcat(buf, "Content-Type: application/json\r\n\r\n");

    strncat(buf, cmd, 1024);

    err = write_to_orchestrator(buf, tmp);
    if(err)
        return err;

    printf("server answered: %s\n", tmp);

    sscanf(tmp, "HTTP/1.1 %d", &httpr);
    if(httpr != 200)
        return -1;

    if(answer != NULL) {
        /* find the body */
        char * p = strstr(tmp, "\r\n\r\n") + 4;
        strcpy(answer, p);
    }
    return 0;
}

static int
plug_device(const char *port, const char *id,
                const char *device, char *pci_addr, int type)
{
    char json[1024] = {0};

    snprintf(json, 1024, PLUG_PORT_JSON_FORMAT, port, id,
        type == 0 ? "ivshmem" : "pci-assign", device);


    return send_command_to_vm(UNIVERSAL_NODE_URL_ATTACH, json, pci_addr);
}

static int
unplug_device(const char *port, const char *id)
{
    char json[1024] = {0};

    snprintf(json, 1024, UNPLUG_PORT_JSON_FORMAT, port, id);

    return send_command_to_vm(UNIVERSAL_NODE_URL_DETACH, json, NULL);
}

static int
plug_ivshmem_device(const char *port, const char *id,
    const char *cmdline_, char *pci_addr)
{
    /* remove first part of the command */
    const char * cmdline = strchr(cmdline_, ',') + 1;
    return plug_device(port, id, cmdline, pci_addr, 0);
}

static int
plug_physical_device(const char *port, const char *id,
    const char *cmdline_, char *pci_addr)
{
    char device[20];
    snprintf(device, sizeof(device), "host=%s", cmdline_);
    return plug_device(port, id, device, pci_addr, 1);
}

static int
send_dpdk_command(const char *port, const char *command)
{
    char json[1024] = {0};

    snprintf(json, 1024, DPDK_SEND_JSON_FORMAT, port, command);

    return send_command_to_vm(UNIVERSAL_NODE_URL_SEND_DPDK, json, NULL);
}

static int
request_add_slave(const char * port, const char *old, const char *new)
{
    char command[50];

    snprintf(command, sizeof(command), ADD_SLAVE_FORMAT, old, new);

    return send_dpdk_command(port, command);
}

static int
request_remove_slave(const char * port, const char *old)
{
    char command[50];

    snprintf(command, sizeof(command), DEL_SLAVE_FORMAT, old);

    return send_dpdk_command(port, command);
}

struct direct_args {
    struct netdev *dev1;
    struct netdev *dev2;
    void (*callback)(void *);
    void *args;
};

static void *
netdev_dpdk_delete_direct_dpdkr_link_thread(void *args_)
{
    struct direct_args args = *((struct direct_args *) args_);
    free(args_);

    struct netdev_dpdk *dev1 = netdev_dpdk_cast(args.dev1);
    struct netdev_dpdk *dev2 = netdev_dpdk_cast(args.dev2);
    struct dpdk_ring *dpdk_ring1, *dpdk_ring2;
    struct dpdkr_direct_link *direct_link;
    char port_name[RTE_ETH_NAME_MAX_LEN];
    int i, err;

    VLOG_INFO("Deleting direct dpdkr link %s <-> %s\n",
                dev1->up.name, dev2->up.name);

    ovs_mutex_lock(&dpdk_mutex);

    /* look for the ports */
    dpdk_ring1 = look_dpdkr_for_port_id(dev1->port_id);
    ovs_assert(dpdk_ring1);

    dpdk_ring2 = look_dpdkr_for_port_id(dev2->port_id);
    ovs_assert(dpdk_ring2);

    /* check that the ports are not already in direct mode */
    if (!dpdk_ring1->direct) {
        VLOG_ERR("Port '%s' is not direct\n", dev1->up.name);
        goto error_unlock;
    }

    if (!dpdk_ring2->direct) {
        VLOG_ERR("Port '%s' is not direct\n", dev2->up.name);
        goto error_unlock;
    }

    /* restarts PMD threads.
     * Important: current implementation of ovs does not stops the pmd threads even
     * if they have no queues to poll, then the PMDs are actually not stopped.
     */
    dev1->requested_n_rxq = 1;
    netdev_request_reconfigure(&dev1->up);

    dev2->requested_n_rxq = 1;
    netdev_request_reconfigure(&dev2->up);

    ovs_mutex_unlock(&dpdk_mutex);

    /* remove first slave device */
    err = request_remove_slave(dev1->up.name, dev1->up.name);
    if (err) {
        VLOG_ERR("Error removing device: '%s'", dev1->up.name);
        goto error_exit;
    }

    /* remove second slave device */
    err = request_remove_slave(dev2->up.name, dev2->up.name);
    if (err) {
        VLOG_ERR("Error removing device: '%s'", dev2->up.name);
        goto error_exit;
    }

    /* tell the guest to wait for the cap on the bypass channel */
    dpdk_ring1->internals->rx_ring_queues[0].state = DESTRUCTION_RX;
    dpdk_ring2->internals->rx_ring_queues[0].state = DESTRUCTION_RX;

    /* Once again:
     *  Memory barrier? Delay?
     */

    /* tell the guest to send the cap on the bypass channel */
    dpdk_ring1->internals->tx_ring_queues[0].state = DESTRUCTION_TX;
    dpdk_ring2->internals->tx_ring_queues[0].state = DESTRUCTION_TX;

    /* give some time for the apps to send the last packets over the bypass */
    xsleep(1);

    /* just in case the app have not set the state */
    dpdk_ring1->internals->tx_ring_queues[0].state = NORMAL_TX;
    dpdk_ring2->internals->tx_ring_queues[0].state = NORMAL_TX;
    dpdk_ring1->internals->rx_ring_queues[0].state = NORMAL_RX;
    dpdk_ring2->internals->rx_ring_queues[0].state = NORMAL_RX;

    /* wait until ports are in normal mode, then unplug slave devices */
    for (i = 0; i < 5; i++) {
        if (dpdk_ring1->internals->bypass_state == BYPASS_DETACHED &&
            dpdk_ring2->internals->bypass_state == BYPASS_DETACHED) {

            VLOG_INFO("Devices are in normal mode\n");

            snprintf(port_name, sizeof(port_name), DIRECT_PORT_NAME_FORMAT,
                dev1->port_id, dev2->port_id);
            err = unplug_device(dev1->up.name, port_name);
            if (err) {
                VLOG_ERR("Error unplugging device: '%s'", port_name);
                goto error_exit;
            }

            err = rte_ivshmem_metadata_remove(port_name);
            if (err) {
                VLOG_ERR("Error removing metadata: '%s'", port_name);
                goto error_exit;
            }

            snprintf(port_name, sizeof(port_name), DIRECT_PORT_NAME_FORMAT,
                dev2->port_id, dev1->port_id);
            err = unplug_device(dev2->up.name, port_name);
            if (err) {
                VLOG_ERR("Error unplugging device: '%s'", port_name);
                goto error_exit;
            }

            err = rte_ivshmem_metadata_remove(port_name);
            if (err) {
                VLOG_ERR("Error removing metadata: '%s'", port_name);
                goto error_exit;
            }

            direct_link = dpdk_ring1->direct;

            rte_ring_free(direct_link->rings[0]);
            rte_ring_free(direct_link->rings[1]);
            rte_free(direct_link);
            dpdk_ring1->direct = NULL;
            dpdk_ring2->direct = NULL;

            break;
        }

        xsleep(1);
    }

    if (args.callback) {
        args.callback(args.args);
    }

    struct netdev_stats stats;

    /* update the local counters with the packets sent using the direct link */
    netdev_dpdk_get_bypass_stats(&dev1->up, &stats);

    dev1->stats.rx_packets += stats.rx_packets;
    dev1->stats.tx_packets += stats.tx_packets;
    //dev1->stats.rx_bytes += stats.rx_bytes;
    //dev1->stats.tx_bytes += stats.tx_bytes;
    dev1->stats.tx_errors += stats.tx_errors;

    netdev_dpdk_get_bypass_stats(&dev2->up, &stats);

    dev2->stats.rx_packets += stats.rx_packets;
    dev2->stats.tx_packets += stats.tx_packets;
    //dev2->stats.rx_bytes += stats.rx_bytes;
    //dev2->stats.tx_bytes += stats.tx_bytes;
    dev2->stats.tx_errors += stats.tx_errors;

    return NULL;

error_unlock:
    ovs_mutex_unlock(&dpdk_mutex);
error_exit:
    return NULL;
}

static void *
netdev_dpdk_delete_direct_dpdk_link_thread(void *args_)
{
    struct direct_args args = *((struct direct_args *) args_);
    free(args_);

    struct netdev_dpdk *dpdk = netdev_dpdk_cast(args.dev1);
    struct netdev_dpdk *dpdkr_ = netdev_dpdk_cast(args.dev2);
    struct dpdk_ring *dpdkr;
    struct dpdk_direct_link *direct_link;
    struct dpdkr_direct_link *dpdkr_direct_link;
    struct vf_info *vf_info = NULL;
    char port_name[RTE_ETH_NAME_MAX_LEN];
    int i, err;

    VLOG_INFO("Deleting direct dpdk link %s <-> %s\n",
                dpdk->up.name, dpdkr_->up.name);

    ovs_mutex_lock(&dpdk_mutex);

    /* look for the ports */
    dpdkr = look_dpdkr_for_port_id(dpdkr_->port_id);
    ovs_assert(dpdkr);


    /* check that the ports are not already in direct mode */
    if (!dpdkr->direct) {
        VLOG_ERR("Port '%s' is not direct\n", dpdkr_->up.name);
        goto error_unlock;
    }

    if (!dpdk->direct) {
        VLOG_ERR("Port '%s' is not direct\n", dpdk->up.name);
        goto error_unlock;
    }

    /* restarts PMD threads.
     * Important: current implementation of ovs does not stops the pmd threads even
     * if they have no queues to poll, then the PMDs are actually not stopped.
     */
    dpdkr_->requested_n_rxq = 1;
    netdev_request_reconfigure(&dpdkr_->up);

    //dpdk->requested_n_rxq = 1;
    //netdev_request_reconfigure(&dpdk->up);

    ovs_mutex_unlock(&dpdk_mutex);

    /* remove first slave device */
    err = request_remove_slave(dpdkr_->up.name, dpdkr_->up.name);
    if (err) {
        VLOG_ERR("Error removing device: '%s'", dpdkr_->up.name);
        goto error_unlock;
    }

    err = rte_eth_set_default_pool(dpdk->port_id, dpdk->pf_pool);
    if (err) {
        VLOG_ERR("Error setting default pool for '%s': %s", dpdk->up.name, rte_strerror(err));
        goto error_unlock;
    }

    /* tell the guest to receive the cap on the normal channel */
    dpdkr->internals->tx_ring_queues[0].state = DESTRUCTION_RX;
    dpdkr->internals->tx_ring_queues[0].state = DESTRUCTION_TX;

    /* give some time for the apps to send the last packets over the bypass */
    xsleep(1);

    /* just in case the app have not set the state */
    dpdkr->internals->tx_ring_queues[0].state = NORMAL_TX;
    dpdkr->internals->rx_ring_queues[0].state = NORMAL_RX;

    /* wait until ports are in normal mode, then unplug slave devices */
    for (i = 0; i < 50; i++) {
        if (dpdkr->internals->bypass_state == BYPASS_DETACHED) {

            VLOG_INFO("Devices are in normal mode\n");

            snprintf(port_name, sizeof(port_name), DIRECT_PORT_NAME_FORMAT,
                dpdk->port_id, dpdkr_->port_id);
            err = unplug_device(dpdkr_->up.name, port_name);
            if (err) {
                VLOG_ERR("Error unplugging device: '%s'", port_name);
                goto error_unlock;
            }

            direct_link = dpdk->direct;

            rte_free(direct_link);
            dpdkr->direct = NULL;
            dpdkr_->direct = NULL;
            dpdk->direct = NULL;

            break;
        }

        xsleep(1);
    }

    //if (args.callback) {
    //    args.callback(args.args);
    //}
    //
    struct netdev_stats stats;

    /* update the local counters with the packets sent using the direct link */
    netdev_dpdk_get_bypass_stats(&dpdkr_->up, &stats);

    dpdkr_->stats.rx_packets += stats.rx_packets;
    dpdkr_->stats.tx_packets += stats.tx_packets;
    //dpdkr_->stats.rx_bytes += stats.rx_bytes;
    //dpdkr_->stats.tx_bytes += stats.tx_bytes;
    dpdkr_->stats.tx_errors += stats.tx_errors;

    dpdk->stats.rx_packets += stats.tx_packets;
    dpdk->stats.tx_packets += stats.rx_packets;
    //dev2->stats.rx_bytes += stats.tx_bytes;
    //dev2->stats.tx_bytes += stats.rx_bytes;
    //dpdk->stats.tx_errors += stats.tx_errors;

    return NULL;

error_unlock:
    ovs_mutex_unlock(&dpdk_mutex);
    //return err;
    return NULL;
}

int
netdev_dpdk_delete_direct_link(struct netdev *dev1_, struct netdev *dev2_,
                                    void (*callback)(void *), void * fargs)
{
    pthread_t tid;
    pthread_attr_t attr;

    struct direct_args *args = malloc(sizeof(*args));
    args->dev1 = dev1_;
    args->dev2 = dev2_;
    args->callback = callback;
    args->args = fargs;
    /*
     * before creating a thread to do all the work it is good to check that the
     * path can be optmized
     */

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    struct netdev_class *dpdkr_class = netdev_lookup_class("dpdkr")->class;
    struct netdev_class *dpdk_class = netdev_lookup_class("dpdk")->class;

    if (dev1_->netdev_class == dpdkr_class &&
        dev2_->netdev_class == dpdkr_class) {

        pthread_create(&tid, &attr, netdev_dpdk_delete_direct_dpdkr_link_thread,
                    (void *)args);
    }
    else if (dev1_->netdev_class == dpdkr_class &&
             dev2_->netdev_class == dpdk_class) {
        pthread_create(&tid, &attr, netdev_dpdk_delete_direct_dpdk_link_thread,
                    (void *)args);
    }
    else if (dev1_->netdev_class == dpdk_class &&
             dev2_->netdev_class == dpdkr_class) {
        pthread_create(&tid, &attr, netdev_dpdk_delete_direct_dpdk_link_thread,
                    (void *)args);
    }
    else {
        return -2;  /* not a valid direct path */
    }

    return 0;
}

static void *
netdev_dpdk_create_direct_dpdkr_link_thread(void *args_)
{
    struct direct_args args = *((struct direct_args *) args_);
    free(args_);

    struct netdev_dpdk *dev1 = netdev_dpdk_cast(args.dev1);
    struct netdev_dpdk *dev2 = netdev_dpdk_cast(args.dev2);

    int err;

    struct dpdkr_direct_link *direct_link;
    struct rte_ring *ring_1_2, *ring_2_1;
    struct dpdk_ring *dpdk_ring1, *dpdk_ring2;
    char cmdline[512];
    char pci_addr1[30];
    char pci_addr2[30];

    /* ports that are directly connected */

    char ring_name[RTE_RING_NAMESIZE];
    char port_name[RTE_ETH_NAME_MAX_LEN];

    VLOG_INFO("Creating direct dpdkr link %s <-> %s\n",
                dev1->up.name, dev2->up.name);

    ovs_assert(dev1 != dev2);

    ovs_mutex_lock(&dpdk_mutex);

    /* look for the ports */
    dpdk_ring1 = look_dpdkr_for_port_id(dev1->port_id);
    ovs_assert(dpdk_ring1);

    dpdk_ring2 = look_dpdkr_for_port_id(dev2->port_id);
    ovs_assert(dpdk_ring2);

    /* check that the ports are not already in direct mode */
    if (dpdk_ring1->direct) {
        VLOG_ERR("Port '%s' is already direct\n", dev1->up.name);
        goto error_unlock;
    }

    if (dpdk_ring2->direct) {
        VLOG_ERR("Port '%s' is already direct\n", dev2->up.name);
        goto error_unlock;
    }

    /* Ports exist and are not used within any other direct link */
    direct_link = dpdk_rte_mzalloc(sizeof(*direct_link));
    dpdk_ring1->direct = direct_link;
    dpdk_ring2->direct = direct_link;

    /* create direct links */
    snprintf(ring_name, sizeof(ring_name), DIRECT_LINK_NAME_FORMAT,
                dev1->port_id, dev2->port_id);
    ring_1_2 = rte_ring_create(ring_name, DPDK_RING_SIZE, SOCKET0,
                            RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ring_1_2 == NULL) {
        rte_free(direct_link);
        goto error_unlock;
    }

    snprintf(ring_name, sizeof(ring_name), DIRECT_LINK_NAME_FORMAT,
                dev2->port_id, dev1->port_id);
    ring_2_1 = rte_ring_create(ring_name, DPDK_RING_SIZE, SOCKET0,
                            RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ring_2_1 == NULL) {
        rte_free(direct_link);
        goto error_unlock;
    }

    direct_link->rings[0] = ring_1_2;
    direct_link->rings[1] = ring_2_1;
    direct_link->vf_info = NULL;

    /* create first ivshmem with a ring pmd inside */

    snprintf(port_name, sizeof(port_name), DIRECT_PORT_NAME_FORMAT,
                dev1->port_id, dev2->port_id);

    err = rte_ivshmem_metadata_create(port_name);
    if (err) {
        VLOG_ERR("Error creating metadata: '%s'", port_name);
        goto error_unlock;
    }

    err = rte_ivshmem_metadata_add_pmd_ring(port_name,
                                        &ring_2_1, 1, &ring_1_2, 1, port_name);
    if (err) {
        VLOG_ERR("Error adding pmd '%s'", port_name);
        goto error_unlock;
    }

    err = rte_ivshmem_metadata_cmdline_generate(cmdline, sizeof(cmdline), port_name);
    if (err) {
        VLOG_ERR("Error creating command line for '%s'", port_name);
        goto error_unlock;
    }

    err = plug_ivshmem_device(dev1->up.name, port_name, cmdline, pci_addr1);
    if (err) {
        VLOG_ERR("Error plugging port '%s'", port_name);
        goto error_unlock;
    }

    /* create second ivshmem with a ring pmd inside */

    snprintf(port_name, sizeof(port_name), DIRECT_PORT_NAME_FORMAT,
                dev2->port_id, dev1->port_id);

    err = rte_ivshmem_metadata_create(port_name);
    if (err) {
        VLOG_ERR("Error creating metadata: '%s'", port_name);
        goto error_unlock;
    }

    err = rte_ivshmem_metadata_add_pmd_ring(port_name,
                                        &ring_1_2, 1, &ring_2_1, 1, port_name);
    if (err) {
        VLOG_ERR("Error adding pmd '%s'", port_name);
        goto error_unlock;
    }

    err = rte_ivshmem_metadata_cmdline_generate(cmdline, sizeof(cmdline), port_name);
    if (err) {
        VLOG_ERR("Error creating command line for '%s'", port_name);
        goto error_unlock;
    }

    err = plug_ivshmem_device(dev2->up.name, port_name, cmdline, pci_addr2);
    if (err) {
        VLOG_ERR("Error plugging port '%s'", port_name);
        goto error_unlock;
    }

    usleep(10000);

    /* add slaves */
    strcpy(dpdk_ring1->internals->bypass_dev, pci_addr1);
    err = request_add_slave(dev1->up.name, dev1->up.name, pci_addr1);
    if (err) {
        VLOG_ERR("Error requesting changing ports");
        goto error_unlock;
    }

    strcpy(dpdk_ring2->internals->bypass_dev, pci_addr2);
    err = request_add_slave(dev2->up.name, dev2->up.name, pci_addr2);
    if (err) {
        VLOG_ERR("Error requesting changing ports");
        goto error_unlock;
    }

    usleep(200000);
    /* tell the guest to look for the cap on the normal channel */
    dpdk_ring1->internals->rx_ring_queues[0].state = CREATION_RX;
    dpdk_ring2->internals->rx_ring_queues[0].state = CREATION_RX;

    /* is a memory barrier necessary here?
     * If yes, is it enough? Is it also necessary a delay?
     * if the TX state is changed before the RX state the cap packet
     * could be lost. It will end up in the application.
     */

    /* tell the guest to send the cap on the normal channel */
    dpdk_ring1->internals->tx_ring_queues[0].state = CREATION_TX;
    dpdk_ring2->internals->tx_ring_queues[0].state = CREATION_TX;


    /* this delay is a bad idea, it only should be used in the case it was not
     * possible to add the bypass channel (i.e, the guest application is not
     * running).
     * XXX: it is also true that how the cap is send in the guest should be
     * analysed, just to avoid sending a cap when not body is waiting for it
     */

    /* give some time for the apps to send the last packets over the normal
     * channel */
    xsleep(1);

    /* just in case the app have not set the state */
    dpdk_ring1->internals->tx_ring_queues[0].state = BYPASS_TX;
    dpdk_ring2->internals->tx_ring_queues[0].state = BYPASS_TX;
    dpdk_ring1->internals->rx_ring_queues[0].state = BYPASS_RX;
    dpdk_ring2->internals->rx_ring_queues[0].state = BYPASS_RX;

    ovs_mutex_unlock(&dpdk_mutex);

    dev1->requested_n_rxq = 0;
    netdev_request_reconfigure(&dev1->up);

    dev2->requested_n_rxq = 0;
    netdev_request_reconfigure(&dev2->up);

    //return 0;
    return NULL;

error_unlock:
    ovs_mutex_unlock(&dpdk_mutex);
    //return -1;
    return NULL;
}

static void *
netdev_dpdk_create_direct_dpdk_link_thread(void *args_)
{
    struct direct_args args = *((struct direct_args *) args_);
    free(args_);

    struct netdev_dpdk *dpdk = netdev_dpdk_cast(args.dev1);
    struct netdev_dpdk *dpdkr_ = netdev_dpdk_cast(args.dev2);
    struct dpdk_ring *dpdkr;
    struct dpdk_direct_link *direct_link;
    struct dpdkr_direct_link *dpdkr_direct_link;
    struct vf_info *vf_info = NULL;
    int err;
    char pci_addr[30];
    char host_pci_addr[30];
    char port_name[RTE_ETH_NAME_MAX_LEN];

    bool bypass_ready = false;
    int i;

    ovs_mutex_lock(&dpdk_mutex);

    if (dpdk->direct) {
        VLOG_ERR("Port '%s' is already direct\n", dpdk->up.name);
        goto error_unlock;
    }

    dpdkr = look_dpdkr_for_port_id(dpdkr_->port_id);
    ovs_assert(dpdkr);

    if (dpdkr->direct) {
        VLOG_ERR("Port '%s' is already direct\n", dpdkr_->up.name);
        goto error_unlock;
    }

    VLOG_INFO("Creating direct dpdkr link %s <-> %s\n",
                dpdk->up.name, dpdkr_->up.name);

    snprintf(port_name, sizeof(port_name), DIRECT_PORT_NAME_FORMAT,
                dpdk->port_id, dpdkr_->port_id);


    for (i = 0; i < dpdk->n_vfs; i++) {
        if (dpdk->vf_info[i].available) {
            vf_info = &dpdk->vf_info[i];
            break;
        }
    }

    if (!vf_info) {
        VLOG_ERR("There are not available virtual function in port '%s'", port_name);
        goto error_unlock;
    }

    snprintf(host_pci_addr, sizeof(host_pci_addr), PCI_SHORT_PRI_FMT,
             vf_info->addr.bus, vf_info->addr.devid, vf_info->addr.function);

    err = plug_physical_device(dpdkr_->up.name, port_name, host_pci_addr, pci_addr);
    if (err) {
        VLOG_ERR("Error plugging port '%s'", port_name);
        goto error_unlock;
    }

    strcpy(dpdkr->internals->bypass_dev, pci_addr);
    err = request_add_slave(dpdkr_->up.name, dpdkr_->up.name, pci_addr);
    if (err) {
        VLOG_ERR("Error requesting changing ports");
        goto error_unlock;
    }

    for (i = 0; i < 500; i++) {

        if (dpdkr->internals->bypass_state == BYPASS_ATTACHED) {
            bypass_ready = true;
            VLOG_INFO("Bypass for port '%s' is attached. i = %d", port_name, i);
            break;
        }

        usleep(10*1000);    /* 10 ms */
    }

    if (!bypass_ready) {
        VLOG_ERR("Bypass device for '%s' is not ready", port_name);
        //goto error_unlock;
    }

    err = rte_eth_set_default_pool(dpdk->port_id, vf_info->vf_id);
    if (err) {
        VLOG_ERR("Error setting default pool for '%s': %s", port_name, rte_strerror(err));
        goto error_unlock;
    }

    /* tell to the guest to wait for a cap packet.
     * In this case there is not cap packet and the guest will switch to the
     * bypass state after the timeout
     */
    dpdkr->internals->rx_ring_queues[0].state = CREATION_RX;

    usleep(100*1000);    /* 100 ms */

    /* just in case the app have not set the state */
    dpdkr->internals->tx_ring_queues[0].state = BYPASS_TX;
    dpdkr->internals->rx_ring_queues[0].state = BYPASS_RX;

    vf_info->available = false;

    direct_link = dpdk_rte_mzalloc(sizeof(*direct_link));
    direct_link->ring = dpdkr;

    dpdk->direct = direct_link;

    dpdkr_direct_link = dpdk_rte_mzalloc(sizeof(*dpdkr_direct_link));
    dpdkr_direct_link->vf_info = vf_info;

    dpdkr->direct = dpdkr_direct_link;

    ovs_mutex_unlock(&dpdk_mutex);

    /*
     * It is not possible to reconfigure the port because the default pool is
     * cleared when that is done.
     * There is not a clear idea of how to implement it in a good way
     *  - extend dpdk?
     */

    //dpdk->requested_n_rxq = 0;
    //netdev_request_reconfigure(&dpdk->up);

    dpdkr_->requested_n_rxq = 0;
    netdev_request_reconfigure(&dpdkr_->up);

    return NULL;

error_unlock:
    ovs_mutex_unlock(&dpdk_mutex);
    return NULL;
}

int
netdev_dpdk_create_direct_link(struct netdev *dev1_, struct netdev *dev2_)
{
    pthread_t tid;
    pthread_attr_t attr;

    struct direct_args *args = malloc(sizeof(*args));

    /*
     * before creating a thread to do all the work it is good to check that the
     * path can be optmized
     */

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    struct netdev_class *dpdkr_class = netdev_lookup_class("dpdkr")->class;
    struct netdev_class *dpdk_class = netdev_lookup_class("dpdk")->class;

    if (dev1_->netdev_class == dpdkr_class &&
        dev2_->netdev_class == dpdkr_class) {
        args->dev1 = dev1_;
        args->dev2 = dev2_;
        pthread_create(&tid, &attr, netdev_dpdk_create_direct_dpdkr_link_thread,
                    (void *)args);
    }
    else if (dev1_->netdev_class == dpdkr_class &&
             dev2_->netdev_class == dpdk_class) {
        args->dev1 = dev2_;
        args->dev2 = dev1_;
        //netdev_dpdk_create_direct_dpdk_link_thread(args);
        pthread_create(&tid, &attr, netdev_dpdk_create_direct_dpdk_link_thread,
                    (void *)args);
    }
    else if (dev1_->netdev_class == dpdk_class &&
             dev2_->netdev_class == dpdkr_class) {
        args->dev1 = dev1_;
        args->dev2 = dev2_;
        //netdev_dpdk_create_direct_dpdk_link_thread(args);
        pthread_create(&tid, &attr, netdev_dpdk_create_direct_dpdk_link_thread,
                    (void *)args);
    }
    else {
        return -2;  /* direct path is not valid for optimization */
    }

    return 0;
}

static void
netdev_dpdk_set_admin_state__(struct netdev_dpdk *dev, bool admin_state)
    OVS_REQUIRES(dev->mutex)
{
    enum netdev_flags old_flags;

    if (admin_state) {
        netdev_dpdk_update_flags__(dev, 0, NETDEV_UP, &old_flags);
    } else {
        netdev_dpdk_update_flags__(dev, NETDEV_UP, 0, &old_flags);
    }
}

static void
netdev_dpdk_set_admin_state(struct unixctl_conn *conn, int argc,
                            const char *argv[], void *aux OVS_UNUSED)
{
    bool up;

    if (!strcasecmp(argv[argc - 1], "up")) {
        up = true;
    } else if ( !strcasecmp(argv[argc - 1], "down")) {
        up = false;
    } else {
        unixctl_command_reply_error(conn, "Invalid Admin State");
        return;
    }

    if (argc > 2) {
        struct netdev *netdev = netdev_from_name(argv[1]);
        if (netdev && is_dpdk_class(netdev->netdev_class)) {
            struct netdev_dpdk *dpdk_dev = netdev_dpdk_cast(netdev);

            ovs_mutex_lock(&dpdk_dev->mutex);
            netdev_dpdk_set_admin_state__(dpdk_dev, up);
            ovs_mutex_unlock(&dpdk_dev->mutex);

            netdev_close(netdev);
        } else {
            unixctl_command_reply_error(conn, "Not a DPDK Interface");
            netdev_close(netdev);
            return;
        }
    } else {
        struct netdev_dpdk *netdev;

        ovs_mutex_lock(&dpdk_mutex);
        LIST_FOR_EACH (netdev, list_node, &dpdk_list) {
            ovs_mutex_lock(&netdev->mutex);
            netdev_dpdk_set_admin_state__(netdev, up);
            ovs_mutex_unlock(&netdev->mutex);
        }
        ovs_mutex_unlock(&dpdk_mutex);
    }
    unixctl_command_reply(conn, "OK");
}

static void
netdev_dpdk_get_dpdkr_cmdline(struct unixctl_conn *conn, int argc OVS_UNUSED,
                            const char *argv[], void *aux OVS_UNUSED)
{
    int err;
    unsigned int port_id;
    struct dpdk_ring *ring;
    char cmdline[1024];

    err = dpdk_dev_parse_name(argv[1], "dpdkr", &port_id);
    if (err) {
        unixctl_command_reply_error(conn, "Invalid Port");
        return;
    }

    ovs_mutex_lock(&dpdk_mutex);

    /* look for the ports */
    ring = look_dpdkr_for_port_no(port_id);
    if (!ring) {
        ovs_mutex_unlock(&dpdk_mutex);
        unixctl_command_reply_error(conn, "Invalid Port");
        return;
    }

    err = rte_ivshmem_metadata_cmdline_generate(cmdline, sizeof(cmdline), argv[1]);
    if (err) {
        ovs_mutex_unlock(&dpdk_mutex);
        unixctl_command_reply_error(conn, "Error creating command line");
        return;
    }

    ovs_mutex_unlock(&dpdk_mutex);
    unixctl_command_reply(conn, cmdline);
}

/*
 * Set virtqueue flags so that we do not receive interrupts.
 */
static void
set_irq_status(struct virtio_net *virtio_dev)
{
    uint32_t i;
    uint64_t idx;

    for (i = 0; i < virtio_dev->virt_qp_nb; i++) {
        idx = i * VIRTIO_QNUM;
        rte_vhost_enable_guest_notification(virtio_dev, idx + VIRTIO_RXQ, 0);
        rte_vhost_enable_guest_notification(virtio_dev, idx + VIRTIO_TXQ, 0);
    }
}

/*
 * Fixes mapping for vhost-user tx queues. Must be called after each
 * enabling/disabling of queues and real_n_txq modifications.
 */
static void
netdev_dpdk_remap_txqs(struct netdev_dpdk *dev)
    OVS_REQUIRES(dev->mutex)
{
    int *enabled_queues, n_enabled = 0;
    int i, k, total_txqs = dev->real_n_txq;

    enabled_queues = dpdk_rte_mzalloc(total_txqs * sizeof *enabled_queues);

    for (i = 0; i < total_txqs; i++) {
        /* Enabled queues always mapped to themselves. */
        if (dev->tx_q[i].map == i) {
            enabled_queues[n_enabled++] = i;
        }
    }

    if (n_enabled == 0 && total_txqs != 0) {
        enabled_queues[0] = OVS_VHOST_QUEUE_DISABLED;
        n_enabled = 1;
    }

    k = 0;
    for (i = 0; i < total_txqs; i++) {
        if (dev->tx_q[i].map != i) {
            dev->tx_q[i].map = enabled_queues[k];
            k = (k + 1) % n_enabled;
        }
    }

    VLOG_DBG("TX queue mapping for %s\n", dev->vhost_id);
    for (i = 0; i < total_txqs; i++) {
        VLOG_DBG("%2d --> %2d", i, dev->tx_q[i].map);
    }

    rte_free(enabled_queues);
}

static int
netdev_dpdk_vhost_set_queues(struct netdev_dpdk *dev, struct virtio_net *virtio_dev)
    OVS_REQUIRES(dev->mutex)
{
    uint32_t qp_num;

    qp_num = virtio_dev->virt_qp_nb;
    if (qp_num > dev->up.n_rxq) {
        VLOG_ERR("vHost Device '%s' %"PRIu64" can't be added - "
                 "too many queues %d > %d", virtio_dev->ifname, virtio_dev->device_fh,
                 qp_num, dev->up.n_rxq);
        return -1;
    }

    dev->real_n_rxq = qp_num;
    dev->real_n_txq = qp_num;
    dev->txq_needs_locking = true;
    /* Enable TX queue 0 by default if it wasn't disabled. */
    if (dev->tx_q[0].map == OVS_VHOST_QUEUE_MAP_UNKNOWN) {
        dev->tx_q[0].map = 0;
    }

    netdev_dpdk_remap_txqs(dev);

    return 0;
}

/*
 * A new virtio-net device is added to a vhost port.
 */
static int
new_device(struct virtio_net *virtio_dev)
{
    struct netdev_dpdk *dev;
    bool exists = false;

    ovs_mutex_lock(&dpdk_mutex);
    /* Add device to the vhost port with the same name as that passed down. */
    LIST_FOR_EACH(dev, list_node, &dpdk_list) {
        if (strncmp(virtio_dev->ifname, dev->vhost_id, IF_NAME_SZ) == 0) {
            ovs_mutex_lock(&dev->mutex);
            if (netdev_dpdk_vhost_set_queues(dev, virtio_dev)) {
                ovs_mutex_unlock(&dev->mutex);
                ovs_mutex_unlock(&dpdk_mutex);
                return -1;
            }
            ovsrcu_set(&dev->virtio_dev, virtio_dev);
            exists = true;
            virtio_dev->flags |= VIRTIO_DEV_RUNNING;
            /* Disable notifications. */
            set_irq_status(virtio_dev);
            ovs_mutex_unlock(&dev->mutex);
            break;
        }
    }
    ovs_mutex_unlock(&dpdk_mutex);

    if (!exists) {
        VLOG_INFO("vHost Device '%s' %"PRIu64" can't be added - name not "
                  "found", virtio_dev->ifname, virtio_dev->device_fh);

        return -1;
    }

    VLOG_INFO("vHost Device '%s' %"PRIu64" has been added", virtio_dev->ifname,
              virtio_dev->device_fh);
    return 0;
}

/* Clears mapping for all available queues of vhost interface. */
static void
netdev_dpdk_txq_map_clear(struct netdev_dpdk *dev)
    OVS_REQUIRES(dev->mutex)
{
    int i;

    for (i = 0; i < dev->real_n_txq; i++) {
        dev->tx_q[i].map = OVS_VHOST_QUEUE_MAP_UNKNOWN;
    }
}

/*
 * Remove a virtio-net device from the specific vhost port.  Use dev->remove
 * flag to stop any more packets from being sent or received to/from a VM and
 * ensure all currently queued packets have been sent/received before removing
 *  the device.
 */
static void
destroy_device(volatile struct virtio_net *virtio_dev)
{
    struct netdev_dpdk *dev;
    bool exists = false;

    ovs_mutex_lock(&dpdk_mutex);
    LIST_FOR_EACH (dev, list_node, &dpdk_list) {
        if (netdev_dpdk_get_virtio(dev) == virtio_dev) {

            ovs_mutex_lock(&dev->mutex);
            virtio_dev->flags &= ~VIRTIO_DEV_RUNNING;
            ovsrcu_set(&dev->virtio_dev, NULL);
            netdev_dpdk_txq_map_clear(dev);
            exists = true;
            ovs_mutex_unlock(&dev->mutex);
            break;
        }
    }

    ovs_mutex_unlock(&dpdk_mutex);

    if (exists == true) {
        /*
         * Wait for other threads to quiesce after setting the 'virtio_dev'
         * to NULL, before returning.
         */
        ovsrcu_synchronize();
        /*
         * As call to ovsrcu_synchronize() will end the quiescent state,
         * put thread back into quiescent state before returning.
         */
        ovsrcu_quiesce_start();
        VLOG_INFO("vHost Device '%s' %"PRIu64" has been removed",
                  virtio_dev->ifname, virtio_dev->device_fh);
    } else {
        VLOG_INFO("vHost Device '%s' %"PRIu64" not found", virtio_dev->ifname,
                  virtio_dev->device_fh);
    }
}

static int
vring_state_changed(struct virtio_net *virtio_dev, uint16_t queue_id,
                    int enable)
{
    struct netdev_dpdk *dev;
    bool exists = false;
    int qid = queue_id / VIRTIO_QNUM;

    if (queue_id % VIRTIO_QNUM == VIRTIO_TXQ) {
        return 0;
    }

    ovs_mutex_lock(&dpdk_mutex);
    LIST_FOR_EACH (dev, list_node, &dpdk_list) {
        if (strncmp(virtio_dev->ifname, dev->vhost_id, IF_NAME_SZ) == 0) {
            ovs_mutex_lock(&dev->mutex);
            if (enable) {
                dev->tx_q[qid].map = qid;
            } else {
                dev->tx_q[qid].map = OVS_VHOST_QUEUE_DISABLED;
            }
            netdev_dpdk_remap_txqs(dev);
            exists = true;
            ovs_mutex_unlock(&dev->mutex);
            break;
        }
    }
    ovs_mutex_unlock(&dpdk_mutex);

    if (exists) {
        VLOG_INFO("State of queue %d ( tx_qid %d ) of vhost device '%s' %"
                  PRIu64" changed to \'%s\'", queue_id, qid,
                  virtio_dev->ifname, virtio_dev->device_fh,
                  (enable == 1) ? "enabled" : "disabled");
    } else {
        VLOG_INFO("vHost Device '%s' %"PRIu64" not found", virtio_dev->ifname,
                  virtio_dev->device_fh);
        return -1;
    }

    return 0;
}

struct virtio_net *
netdev_dpdk_get_virtio(const struct netdev_dpdk *dev)
{
    return ovsrcu_get(struct virtio_net *, &dev->virtio_dev);
}

/*
 * These callbacks allow virtio-net devices to be added to vhost ports when
 * configuration has been fully complete.
 */
static const struct virtio_net_device_ops virtio_net_device_ops =
{
    .new_device =  new_device,
    .destroy_device = destroy_device,
    .vring_state_changed = vring_state_changed
};

static void *
start_vhost_loop(void *dummy OVS_UNUSED)
{
     pthread_detach(pthread_self());
     /* Put the cuse thread into quiescent state. */
     ovsrcu_quiesce_start();
     rte_vhost_driver_session_start();
     return NULL;
}

static int
dpdk_vhost_class_init(void)
{
    rte_vhost_driver_callback_register(&virtio_net_device_ops);
    rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_HOST_TSO4
                            | 1ULL << VIRTIO_NET_F_HOST_TSO6
                            | 1ULL << VIRTIO_NET_F_CSUM);

    ovs_thread_create("vhost_thread", start_vhost_loop, NULL);
    return 0;
}

static int
dpdk_vhost_cuse_class_init(void)
{
    int err = -1;


    /* Register CUSE device to handle IOCTLs.
     * Unless otherwise specified on the vswitchd command line, cuse_dev_name
     * is set to vhost-net.
     */
    err = rte_vhost_driver_register(cuse_dev_name);

    if (err != 0) {
        VLOG_ERR("CUSE device setup failure.");
        return -1;
    }

    dpdk_vhost_class_init();
    return 0;
}

static int
dpdk_vhost_user_class_init(void)
{
    dpdk_vhost_class_init();
    return 0;
}

static void
dpdk_common_init(void)
{
    unixctl_command_register("netdev-dpdk/set-admin-state",
                             "[netdev] up|down", 1, 2,
                             netdev_dpdk_set_admin_state, NULL);

    unixctl_command_register("netdev-dpdk/get-cmdline",
                             "port", 1, 1,
                             netdev_dpdk_get_dpdkr_cmdline, NULL);

    ovs_thread_create("dpdk_watchdog", dpdk_watchdog, NULL);
}

/* Client Rings */

static int
dpdk_ring_create(const char dev_name[], unsigned int port_no,
                 unsigned int *eth_port_id)
{
    struct dpdk_ring *ivshmem;
    char ring_name[RTE_RING_NAMESIZE];
    int err;

    ivshmem = dpdk_rte_mzalloc(sizeof *ivshmem);
    if (ivshmem == NULL) {
        return ENOMEM;
    }

    /* XXX: Add support for multiquque ring. */
    err = snprintf(ring_name, sizeof(ring_name), "%s_tx", dev_name);
    if (err < 0) {
        return -err;
    }

    /* Create single producer tx ring, netdev does explicit locking. */
    ivshmem->cring_tx = rte_ring_create(ring_name, DPDK_RING_SIZE, SOCKET0,
                                        RING_F_SP_ENQ);
    if (ivshmem->cring_tx == NULL) {
        rte_free(ivshmem);
        return ENOMEM;
    }

    err = snprintf(ring_name, sizeof(ring_name), "%s_rx", dev_name);
    if (err < 0) {
        return -err;
    }

    /* Create single consumer rx ring, netdev does explicit locking. */
    ivshmem->cring_rx = rte_ring_create(ring_name, DPDK_RING_SIZE, SOCKET0,
                                        RING_F_SC_DEQ);
    if (ivshmem->cring_rx == NULL) {
        rte_free(ivshmem);
        return ENOMEM;
    }

    err = rte_eth_from_rings(dev_name, &ivshmem->cring_rx, 1,
                             &ivshmem->cring_tx, 1, SOCKET0);

    if (err < 0) {
        rte_free(ivshmem);
        return ENODEV;
    }

    /* create ivshmem metadata file for this port */
    err = rte_ivshmem_metadata_create(dev_name);
    if (err) {
        VLOG_ERR("Error creating metadata: '%s'", dev_name);
        return err;
    }

    err = rte_ivshmem_metadata_add_pmd_ring(dev_name, &ivshmem->cring_tx, 1,
                                    &ivshmem->cring_rx, 1, dev_name);
    if (err) {
        VLOG_ERR("Error adding pmd '%s'", dev_name);
        return err;
    }

    ivshmem->internals = rte_ivshmem_metadata_get_pmd_internals(dev_name, dev_name);

    ivshmem->direct = NULL; /* this port is not direct */
    ivshmem->user_port_id = port_no;
    ivshmem->eth_port_id = rte_eth_dev_count() - 1;
    ovs_list_push_back(&dpdk_ring_list, &ivshmem->list_node);

    *eth_port_id = ivshmem->eth_port_id;
    return 0;
}

static int
dpdk_ring_open(const char dev_name[], unsigned int *eth_port_id) OVS_REQUIRES(dpdk_mutex)
{
    struct dpdk_ring *ivshmem;
    unsigned int port_no;
    int err = 0;

    /* Names always start with "dpdkr" */
    err = dpdk_dev_parse_name(dev_name, "dpdkr", &port_no);
    if (err) {
        return err;
    }

    /* look through our list to find the device */
    LIST_FOR_EACH (ivshmem, list_node, &dpdk_ring_list) {
         if (ivshmem->user_port_id == port_no) {
            VLOG_INFO("Found dpdk ring device %s:", dev_name);
            *eth_port_id = ivshmem->eth_port_id; /* really all that is needed */
            return 0;
         }
    }
    /* Need to create the device rings */
    return dpdk_ring_create(dev_name, port_no, eth_port_id);
}

static int
netdev_dpdk_ring_send(struct netdev *netdev, int qid,
                      struct dp_packet **pkts, int cnt, bool may_steal)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    unsigned i;

    /* When using 'dpdkr' and sending to a DPDK ring, we want to ensure that the
     * rss hash field is clear. This is because the same mbuf may be modified by
     * the consumer of the ring and return into the datapath without recalculating
     * the RSS hash. */
    for (i = 0; i < cnt; i++) {
        dp_packet_rss_invalidate(pkts[i]);
    }

    netdev_dpdk_send__(dev, qid, pkts, cnt, may_steal);
    return 0;
}

static int
netdev_dpdk_ring_construct(struct netdev *netdev)
{
    unsigned int port_no = 0;
    int err = 0;

    if (rte_eal_init_ret) {
        return rte_eal_init_ret;
    }

    ovs_mutex_lock(&dpdk_mutex);

    err = dpdk_ring_open(netdev->name, &port_no);
    if (err) {
        goto unlock_dpdk;
    }

    err = netdev_dpdk_init(netdev, port_no, DPDK_DEV_ETH);

unlock_dpdk:
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

/* QoS Functions */

/*
 * Initialize QoS configuration operations.
 */
static void
qos_conf_init(struct qos_conf *conf, const struct dpdk_qos_ops *ops)
{
    conf->ops = ops;
}

/*
 * Search existing QoS operations in qos_ops and compare each set of
 * operations qos_name to name. Return a dpdk_qos_ops pointer to a match,
 * else return NULL
 */
static const struct dpdk_qos_ops *
qos_lookup_name(const char *name)
{
    const struct dpdk_qos_ops *const *opsp;

    for (opsp = qos_confs; *opsp != NULL; opsp++) {
        const struct dpdk_qos_ops *ops = *opsp;
        if (!strcmp(name, ops->qos_name)) {
            return ops;
        }
    }
    return NULL;
}

/*
 * Call qos_destruct to clean up items associated with the netdevs
 * qos_conf. Set netdevs qos_conf to NULL.
 */
static void
qos_delete_conf(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    rte_spinlock_lock(&dev->qos_lock);
    if (dev->qos_conf) {
        if (dev->qos_conf->ops->qos_destruct) {
            dev->qos_conf->ops->qos_destruct(netdev, dev->qos_conf);
        }
        dev->qos_conf = NULL;
    }
    rte_spinlock_unlock(&dev->qos_lock);
}

static int
netdev_dpdk_get_qos_types(const struct netdev *netdev OVS_UNUSED,
                           struct sset *types)
{
    const struct dpdk_qos_ops *const *opsp;

    for (opsp = qos_confs; *opsp != NULL; opsp++) {
        const struct dpdk_qos_ops *ops = *opsp;
        if (ops->qos_construct && ops->qos_name[0] != '\0') {
            sset_add(types, ops->qos_name);
        }
    }
    return 0;
}

static int
netdev_dpdk_get_qos(const struct netdev *netdev,
                    const char **typep, struct smap *details)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int error = 0;

    ovs_mutex_lock(&dev->mutex);
    if(dev->qos_conf) {
        *typep = dev->qos_conf->ops->qos_name;
        error = (dev->qos_conf->ops->qos_get
                 ? dev->qos_conf->ops->qos_get(netdev, details): 0);
    }
    ovs_mutex_unlock(&dev->mutex);

    return error;
}

static int
netdev_dpdk_set_qos(struct netdev *netdev,
                    const char *type, const struct smap *details)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    const struct dpdk_qos_ops *new_ops = NULL;
    int error = 0;

    /* If type is empty or unsupported then the current QoS configuration
     * for the dpdk-netdev can be destroyed */
    new_ops = qos_lookup_name(type);

    if (type[0] == '\0' || !new_ops || !new_ops->qos_construct) {
        qos_delete_conf(netdev);
        return EOPNOTSUPP;
    }

    ovs_mutex_lock(&dev->mutex);

    if (dev->qos_conf) {
        if (new_ops == dev->qos_conf->ops) {
            error = new_ops->qos_set ? new_ops->qos_set(netdev, details) : 0;
        } else {
            /* Delete existing QoS configuration. */
            qos_delete_conf(netdev);
            ovs_assert(dev->qos_conf == NULL);

            /* Install new QoS configuration. */
            error = new_ops->qos_construct(netdev, details);
            ovs_assert((error == 0) == (dev->qos_conf != NULL));
        }
    } else {
        error = new_ops->qos_construct(netdev, details);
        ovs_assert((error == 0) == (dev->qos_conf != NULL));
    }

    ovs_mutex_unlock(&dev->mutex);
    return error;
}

/* egress-policer details */

struct egress_policer {
    struct qos_conf qos_conf;
    struct rte_meter_srtcm_params app_srtcm_params;
    struct rte_meter_srtcm egress_meter;
};

static struct egress_policer *
egress_policer_get__(const struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    return CONTAINER_OF(dev->qos_conf, struct egress_policer, qos_conf);
}

static int
egress_policer_qos_construct(struct netdev *netdev,
                             const struct smap *details)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct egress_policer *policer;
    const char *cir_s;
    const char *cbs_s;
    int err = 0;

    rte_spinlock_lock(&dev->qos_lock);
    policer = xmalloc(sizeof *policer);
    qos_conf_init(&policer->qos_conf, &egress_policer_ops);
    dev->qos_conf = &policer->qos_conf;
    cir_s = smap_get(details, "cir");
    cbs_s = smap_get(details, "cbs");
    policer->app_srtcm_params.cir = cir_s ? strtoull(cir_s, NULL, 10) : 0;
    policer->app_srtcm_params.cbs = cbs_s ? strtoull(cbs_s, NULL, 10) : 0;
    policer->app_srtcm_params.ebs = 0;
    err = rte_meter_srtcm_config(&policer->egress_meter,
                                    &policer->app_srtcm_params);
    rte_spinlock_unlock(&dev->qos_lock);

    return err;
}

static void
egress_policer_qos_destruct(struct netdev *netdev OVS_UNUSED,
                        struct qos_conf *conf)
{
    struct egress_policer *policer = CONTAINER_OF(conf, struct egress_policer,
                                                qos_conf);
    free(policer);
}

static int
egress_policer_qos_get(const struct netdev *netdev, struct smap *details)
{
    struct egress_policer *policer = egress_policer_get__(netdev);
    smap_add_format(details, "cir", "%llu",
                    1ULL * policer->app_srtcm_params.cir);
    smap_add_format(details, "cbs", "%llu",
                    1ULL * policer->app_srtcm_params.cbs);

    return 0;
}

static int
egress_policer_qos_set(struct netdev *netdev, const struct smap *details)
{
    struct egress_policer *policer;
    const char *cir_s;
    const char *cbs_s;
    int err = 0;

    policer = egress_policer_get__(netdev);
    cir_s = smap_get(details, "cir");
    cbs_s = smap_get(details, "cbs");
    policer->app_srtcm_params.cir = cir_s ? strtoull(cir_s, NULL, 10) : 0;
    policer->app_srtcm_params.cbs = cbs_s ? strtoull(cbs_s, NULL, 10) : 0;
    policer->app_srtcm_params.ebs = 0;
    err = rte_meter_srtcm_config(&policer->egress_meter,
                                    &policer->app_srtcm_params);

    return err;
}

static inline bool
egress_policer_pkt_handle__(struct rte_meter_srtcm *meter,
                            struct rte_mbuf *pkt, uint64_t time)
{
    uint32_t pkt_len = rte_pktmbuf_pkt_len(pkt) - sizeof(struct ether_hdr);

    return rte_meter_srtcm_color_blind_check(meter, time, pkt_len) ==
                                                e_RTE_METER_GREEN;
}

static int
egress_policer_run(struct netdev *netdev, struct rte_mbuf **pkts,
                        int pkt_cnt)
{
    int i = 0;
    int cnt = 0;
    struct egress_policer *policer = egress_policer_get__(netdev);
    struct rte_mbuf *pkt = NULL;
    uint64_t current_time = rte_rdtsc();

    for(i = 0; i < pkt_cnt; i++) {
        pkt = pkts[i];
        /* Handle current packet */
        if (egress_policer_pkt_handle__(&policer->egress_meter, pkt,
                                        current_time)) {
            if (cnt != i) {
                pkts[cnt] = pkt;
            }
            cnt++;
        } else {
            rte_pktmbuf_free(pkt);
        }
    }

    return cnt;
}

static const struct dpdk_qos_ops egress_policer_ops = {
    "egress-policer",    /* qos_name */
    egress_policer_qos_construct,
    egress_policer_qos_destruct,
    egress_policer_qos_get,
    egress_policer_qos_set,
    egress_policer_run
};

static int
netdev_dpdk_reconfigure(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int err = 0;

    ovs_mutex_lock(&dpdk_mutex);
    ovs_mutex_lock(&dev->mutex);

    if (netdev->n_txq == dev->requested_n_txq
        && netdev->n_rxq == dev->requested_n_rxq) {
        /* Reconfiguration is unnecessary */

        goto out;
    }

    rte_eth_dev_stop(dev->port_id);

    netdev->n_txq = dev->requested_n_txq;
    netdev->n_rxq = dev->requested_n_rxq;

    rte_free(dev->tx_q);
    err = dpdk_eth_dev_init(dev);
    netdev_dpdk_alloc_txq(dev, dev->real_n_txq);

    dev->txq_needs_locking = dev->real_n_txq != netdev->n_txq;

out:

    ovs_mutex_unlock(&dev->mutex);
    ovs_mutex_unlock(&dpdk_mutex);

    return err;
}

static int
netdev_dpdk_vhost_user_reconfigure(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dpdk_mutex);
    ovs_mutex_lock(&dev->mutex);

    netdev->n_txq = dev->requested_n_txq;
    netdev->n_rxq = dev->requested_n_rxq;

    ovs_mutex_unlock(&dev->mutex);
    ovs_mutex_unlock(&dpdk_mutex);

    return 0;
}

static int
netdev_dpdk_vhost_cuse_reconfigure(struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dpdk_mutex);
    ovs_mutex_lock(&dev->mutex);

    netdev->n_txq = dev->requested_n_txq;
    dev->real_n_txq = 1;
    netdev->n_rxq = 1;
    dev->txq_needs_locking = dev->real_n_txq != netdev->n_txq;

    ovs_mutex_unlock(&dev->mutex);
    ovs_mutex_unlock(&dpdk_mutex);

    return 0;
}

#define NETDEV_DPDK_CLASS(NAME, INIT, CONSTRUCT, DESTRUCT, SEND, \
                          GET_CARRIER, GET_STATS, GET_FEATURES,  \
                          GET_STATUS, RECONFIGURE, RXQ_RECV)     \
{                                                             \
    NAME,                                                     \
    true,                       /* is_pmd */                  \
    INIT,                       /* init */                    \
    NULL,                       /* netdev_dpdk_run */         \
    NULL,                       /* netdev_dpdk_wait */        \
                                                              \
    netdev_dpdk_alloc,                                        \
    CONSTRUCT,                                                \
    DESTRUCT,                                                 \
    netdev_dpdk_dealloc,                                      \
    netdev_dpdk_get_config,                                   \
    netdev_dpdk_set_config,                                   \
    NULL,                       /* get_tunnel_config */       \
    NULL,                       /* build header */            \
    NULL,                       /* push header */             \
    NULL,                       /* pop header */              \
    netdev_dpdk_get_numa_id,    /* get_numa_id */             \
    netdev_dpdk_set_tx_multiq,                                \
                                                              \
    SEND,                       /* send */                    \
    NULL,                       /* send_wait */               \
                                                              \
    netdev_dpdk_set_etheraddr,                                \
    netdev_dpdk_get_etheraddr,                                \
    netdev_dpdk_get_mtu,                                      \
    netdev_dpdk_set_mtu,                                      \
    netdev_dpdk_get_ifindex,                                  \
    GET_CARRIER,                                              \
    netdev_dpdk_get_carrier_resets,                           \
    netdev_dpdk_set_miimon,                                   \
    GET_STATS,                                                \
    GET_FEATURES,                                             \
    NULL,                       /* set_advertisements */      \
                                                              \
    NULL,                       /* set_policing */            \
    netdev_dpdk_get_qos_types,                                \
    NULL,                       /* get_qos_capabilities */    \
    netdev_dpdk_get_qos,                                      \
    netdev_dpdk_set_qos,                                      \
    NULL,                       /* get_queue */               \
    NULL,                       /* set_queue */               \
    NULL,                       /* delete_queue */            \
    NULL,                       /* get_queue_stats */         \
    NULL,                       /* queue_dump_start */        \
    NULL,                       /* queue_dump_next */         \
    NULL,                       /* queue_dump_done */         \
    NULL,                       /* dump_queue_stats */        \
                                                              \
    NULL,                       /* set_in4 */                 \
    NULL,                       /* get_addr_list */           \
    NULL,                       /* add_router */              \
    NULL,                       /* get_next_hop */            \
    GET_STATUS,                                               \
    NULL,                       /* arp_lookup */              \
                                                              \
    netdev_dpdk_update_flags,                                 \
    RECONFIGURE,                                              \
                                                              \
    netdev_dpdk_rxq_alloc,                                    \
    netdev_dpdk_rxq_construct,                                \
    netdev_dpdk_rxq_destruct,                                 \
    netdev_dpdk_rxq_dealloc,                                  \
    RXQ_RECV,                                                 \
    NULL,                       /* rx_wait */                 \
    NULL,                       /* rxq_drain */               \
}

static int
process_vhost_flags(char *flag, char *default_val, int size,
                    char **argv, char **new_val)
{
    int changed = 0;

    /* Depending on which version of vhost is in use, process the vhost-specific
     * flag if it is provided on the vswitchd command line, otherwise resort to
     * a default value.
     *
     * For vhost-user: Process "-vhost_sock_dir" to set the custom location of
     * the vhost-user socket(s).
     * For vhost-cuse: Process "-cuse_dev_name" to set the custom name of the
     * vhost-cuse character device.
     */
    if (!strcmp(argv[1], flag) && (strlen(argv[2]) <= size)) {
        changed = 1;
        *new_val = xstrdup(argv[2]);
        VLOG_INFO("User-provided %s in use: %s", flag, *new_val);
    } else {
        VLOG_INFO("No %s provided - defaulting to %s", flag, default_val);
        *new_val = default_val;
    }

    return changed;
}

int
dpdk_init(int argc, char **argv)
{
    int result;
    int base = 0;
    char *pragram_name = argv[0];

    if (argc < 2 || strcmp(argv[1], "--dpdk"))
        return 0;

    /* Remove the --dpdk argument from arg list.*/
    argc--;
    argv++;

    /* Reject --user option */
    int i;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--user")) {
            VLOG_ERR("Can not mix --dpdk and --user options, aborting.");
        }
    }

#ifdef VHOST_CUSE
    if (process_vhost_flags("-cuse_dev_name", xstrdup("vhost-net"),
                            PATH_MAX, argv, &cuse_dev_name)) {
#else
    if (process_vhost_flags("-vhost_sock_dir", xstrdup(ovs_rundir()),
                            NAME_MAX, argv, &vhost_sock_dir)) {
        struct stat s;
        int err;

        err = stat(vhost_sock_dir, &s);
        if (err) {
            VLOG_ERR("vHostUser socket DIR '%s' does not exist.",
                     vhost_sock_dir);
            return err;
        }
#endif
        /* Remove the vhost flag configuration parameters from the argument
         * list, so that the correct elements are passed to the DPDK
         * initialization function
         */
        argc -= 2;
        argv += 2;    /* Increment by two to bypass the vhost flag arguments */
        base = 2;
    }

    /* Keep the program name argument as this is needed for call to
     * rte_eal_init()
     */
    argv[0] = pragram_name;

    /* Make sure things are initialized ... */
    result = rte_eal_init(argc, argv);
    if (result < 0) {
        ovs_abort(result, "Cannot init EAL");
    }

    rte_memzone_dump(stdout);
    rte_eal_init_ret = 0;

    if (argc > result) {
        argv[result] = argv[0];
    }

    /* We are called from the main thread here */
    RTE_PER_LCORE(_lcore_id) = NON_PMD_CORE_ID;

    return result + 1 + base;
}

static const struct netdev_class dpdk_class =
    NETDEV_DPDK_CLASS(
        "dpdk",
        NULL,
        netdev_dpdk_construct,
        netdev_dpdk_destruct,
        netdev_dpdk_eth_send,
        netdev_dpdk_get_carrier,
        netdev_dpdk_get_stats,
        netdev_dpdk_get_features,
        netdev_dpdk_get_status,
        netdev_dpdk_reconfigure,
        netdev_dpdk_rxq_recv);

static const struct netdev_class dpdk_ring_class =
    NETDEV_DPDK_CLASS(
        "dpdkr",
        NULL,
        netdev_dpdk_ring_construct,
        netdev_dpdk_destruct,
        netdev_dpdk_ring_send,
        netdev_dpdk_get_carrier,
        netdev_dpdk_ring_get_stats,
        netdev_dpdk_get_features,
        netdev_dpdk_get_status,
        netdev_dpdk_reconfigure,
        netdev_dpdk_rxq_recv);

static const struct netdev_class OVS_UNUSED dpdk_vhost_cuse_class =
    NETDEV_DPDK_CLASS(
        "dpdkvhostcuse",
        dpdk_vhost_cuse_class_init,
        netdev_dpdk_vhost_cuse_construct,
        netdev_dpdk_vhost_destruct,
        netdev_dpdk_vhost_send,
        netdev_dpdk_vhost_get_carrier,
        netdev_dpdk_vhost_get_stats,
        NULL,
        NULL,
        netdev_dpdk_vhost_cuse_reconfigure,
        netdev_dpdk_vhost_rxq_recv);

static const struct netdev_class OVS_UNUSED dpdk_vhost_user_class =
    NETDEV_DPDK_CLASS(
        "dpdkvhostuser",
        dpdk_vhost_user_class_init,
        netdev_dpdk_vhost_user_construct,
        netdev_dpdk_vhost_destruct,
        netdev_dpdk_vhost_send,
        netdev_dpdk_vhost_get_carrier,
        netdev_dpdk_vhost_get_stats,
        NULL,
        NULL,
        netdev_dpdk_vhost_user_reconfigure,
        netdev_dpdk_vhost_rxq_recv);

void
netdev_dpdk_register(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;

    if (rte_eal_init_ret) {
        return;
    }

    if (ovsthread_once_start(&once)) {
        dpdk_common_init();
        netdev_register_provider(&dpdk_class);
        netdev_register_provider(&dpdk_ring_class);
#ifdef VHOST_CUSE
        netdev_register_provider(&dpdk_vhost_cuse_class);
#else
        netdev_register_provider(&dpdk_vhost_user_class);
#endif
        ovsthread_once_done(&once);
    }
}

int
pmd_thread_setaffinity_cpu(unsigned cpu)
{
    cpu_set_t cpuset;
    int err;

    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    err = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (err) {
        VLOG_ERR("Thread affinity error %d",err);
        return err;
    }
    /* NON_PMD_CORE_ID is reserved for use by non pmd threads. */
    ovs_assert(cpu != NON_PMD_CORE_ID);
    RTE_PER_LCORE(_lcore_id) = cpu;

    return 0;
}

static bool
dpdk_thread_is_pmd(void)
{
    return rte_lcore_id() != NON_PMD_CORE_ID;
}
