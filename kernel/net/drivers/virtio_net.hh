#pragma once

#include "types.hh"
#include "platform.hh"
#include "mem/memlayout.hh"
#include "spinlock.hh"

#ifdef RISCV
// VirtIO network device registers (same MMIO interface as disk)
#define VIRTIO_MMIO_MAGIC_VALUE 0x000 // 0x74726976
#define VIRTIO_MMIO_VERSION 0x004     // version; 1 is legacy
#define VIRTIO_MMIO_DEVICE_ID 0x008   // device type; 1 is net, 2 is disk
#define VIRTIO_MMIO_VENDOR_ID 0x00c   // 0x554d4551
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028  // page size for PFN, write-only
#define VIRTIO_MMIO_QUEUE_SEL 0x030        // select queue, write-only
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034    // max size of current queue, read-only
#define VIRTIO_MMIO_QUEUE_NUM 0x038        // size of current queue, write-only
#define VIRTIO_MMIO_QUEUE_ALIGN 0x03c      // used ring alignment, write-only
#define VIRTIO_MMIO_QUEUE_PFN 0x040        // physical page number for queue, read/write
#define VIRTIO_MMIO_QUEUE_READY 0x044      // ready bit
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050     // write-only
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060 // read-only
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064    // write-only
#define VIRTIO_MMIO_STATUS 0x070           // read/write

// Status register bits
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER 2
#define VIRTIO_CONFIG_S_DRIVER_OK 4
#define VIRTIO_CONFIG_S_FEATURES_OK 8

// VirtIO net feature bits
#define VIRTIO_NET_F_CSUM 0                /* Host handles pkts w/ partial csum */
#define VIRTIO_NET_F_GUEST_CSUM 1          /* Guest handles pkts w/ partial csum */
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS 2 /* Control channel offloads */
#define VIRTIO_NET_F_MTU 3                 /* Initial MTU advice */
#define VIRTIO_NET_F_MAC 5                 /* Host has given MAC address. */
#define VIRTIO_NET_F_GUEST_TSO4 7          /* Guest can handle TSOv4 in. */
#define VIRTIO_NET_F_GUEST_TSO6 8          /* Guest can handle TSOv6 in. */
#define VIRTIO_NET_F_GUEST_ECN 9           /* Guest can handle TSO[6] w/ ECN in. */
#define VIRTIO_NET_F_GUEST_UFO 10          /* Guest can handle UFO in. */
#define VIRTIO_NET_F_HOST_TSO4 11          /* Host can handle TSOv4 in. */
#define VIRTIO_NET_F_HOST_TSO6 12          /* Host can handle TSOv6 in. */
#define VIRTIO_NET_F_HOST_ECN 13           /* Host can handle TSO[6] w/ ECN in. */
#define VIRTIO_NET_F_HOST_UFO 14           /* Host can handle UFO in. */
#define VIRTIO_NET_F_MRG_RXBUF 15          /* Host can merge receive buffers. */
#define VIRTIO_NET_F_STATUS 16             /* virtio_net_config.status available */
#define VIRTIO_NET_F_CTRL_VQ 17            /* Control channel available */
#define VIRTIO_NET_F_CTRL_RX 18            /* Control channel RX mode support */
#define VIRTIO_NET_F_CTRL_VLAN 19          /* Control channel VLAN filtering */
#define VIRTIO_NET_F_CTRL_RX_EXTRA 20      /* Extra RX mode control support */
#define VIRTIO_NET_F_GUEST_ANNOUNCE 21     /* Guest can announce device on the network */
#define VIRTIO_NET_F_MQ 22                 /* Device supports Receive Flow Steering */

// Common VirtIO feature bits
#define VIRTIO_F_ANY_LAYOUT 27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX 29

// Queue constants
#define VIRTIO_NET_RX_QUEUE_IDX 0
#define VIRTIO_NET_TX_QUEUE_IDX 1

#elif defined(LOONGARCH)
// LoongArch specific includes for PCI-based VirtIO
#include "devs/loongarch/pci.hh"

// Status register bits (same as RISCV)
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER 2
#define VIRTIO_CONFIG_S_DRIVER_OK 4
#define VIRTIO_CONFIG_S_FEATURES_OK 8

// VirtIO net feature bits (same as RISCV)
#define VIRTIO_NET_F_CSUM 0
#define VIRTIO_NET_F_GUEST_CSUM 1
#define VIRTIO_NET_F_MAC 5
#define VIRTIO_NET_F_HOST_TSO4 11
#define VIRTIO_NET_F_HOST_TSO6 12
#define VIRTIO_NET_F_MRG_RXBUF 15
#define VIRTIO_NET_F_STATUS 16
#define VIRTIO_NET_F_CTRL_VQ 17
#define VIRTIO_NET_F_CTRL_RX 18
#define VIRTIO_NET_F_CTRL_VLAN 19
#define VIRTIO_NET_F_MQ 22

// Common VirtIO feature bits
#define VIRTIO_F_ANY_LAYOUT 27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX 29

// VirtIO Net device type for PCI
#define VIRTIO_NET_VENDOR_ID 0x1af4
#define VIRTIO_NET_DEVICE_ID 0x1000

// Queue constants
#define VIRTIO_NET_RX_QUEUE_IDX 0
#define VIRTIO_NET_TX_QUEUE_IDX 1

#endif

// Common definitions for both architectures
#define NUM_NET_DESC 8     // Number of descriptors per queue (must be power of 2)
#define ETH_ALEN 6         // Ethernet address length
#define ETH_FRAME_LEN 1514 // Maximum Ethernet frame size

namespace virtio_net
{
    // VirtIO Ring Descriptor
    struct VRingDesc
    {
        uint64 addr;
        uint32 len;
        uint16 flags;
        uint16 next;
    };

#define VRING_DESC_F_NEXT 1  // chained with another descriptor
#define VRING_DESC_F_WRITE 2 // device writes (vs read)

    // VirtIO Ring Used Element
    struct VRingUsedElem
    {
        uint32 id; // index of start of completed descriptor chain
        uint32 len;
    };

    // VirtIO Ring Used Area
    struct VRingUsedArea
    {
        uint16 flags;
        uint16 idx;
        struct VRingUsedElem ring[NUM_NET_DESC];
    };

    // VirtIO Net Header (prepended to each packet)
    struct virtio_net_hdr
    {
        uint8 flags;
        uint8 gso_type;
        uint16 hdr_len;     // Ethernet + IP + tcp/udp hdrs
        uint16 gso_size;    // Bytes to append to hdr_len per frame
        uint16 csum_start;  // Position to start checksumming from
        uint16 csum_offset; // Offset after that to place checksum
    };

    // Basic Ethernet header
    struct eth_hdr
    {
        uint8 dst_mac[ETH_ALEN];
        uint8 src_mac[ETH_ALEN];
        uint16 ethertype;
    } __attribute__((packed));

    // Network packet buffer
    struct net_buf
    {
        uint8 data[ETH_FRAME_LEN + sizeof(struct virtio_net_hdr)];
        uint32 len;
        bool in_use;
    };

    // VirtIO Network Device
    struct virtio_net_device
    {
        // Memory for virtio descriptors & rings for both RX and TX queues
        char pages[4 * PGSIZE] __attribute__((aligned(PGSIZE)));

        // RX queue (receiveq - queue 0)
        struct VRingDesc *rx_desc;
        uint16 *rx_avail;
        struct VRingUsedArea *rx_used;

        // TX queue (transmitq - queue 1)
        struct VRingDesc *tx_desc;
        uint16 *tx_avail;
        struct VRingUsedArea *tx_used;

        // Book-keeping
        char rx_free[NUM_NET_DESC]; // is a RX descriptor free?
        char tx_free[NUM_NET_DESC]; // is a TX descriptor free?
        uint16 rx_used_idx;         // we've looked this far in rx_used
        uint16 tx_used_idx;         // we've looked this far in tx_used

        // Network buffers for packet storage
        struct net_buf rx_buffers[NUM_NET_DESC];
        struct net_buf tx_buffers[NUM_NET_DESC];

        // Device configuration
        uint8 mac_addr[ETH_ALEN];   // Device MAC address
        uint16 status;              // Link status
        uint16 max_virtqueue_pairs; // Number of supported queue pairs

        // Synchronization
        SpinLock net_lock;

#ifdef LOONGARCH
        // PCI specific fields for LoongArch
        loongarch::qemu::virtio_pci_hw virtio_net_hw;
        uint64 pci_dev;
        int port_id;
#endif
    };

    // Function declarations
    void virtio_net_init(void);
    int virtio_net_send(const void *data, uint32 len);
    int virtio_net_recv(void *data, uint32 *len);
    void virtio_net_intr(void);
    bool virtio_net_link_up(void);
    void virtio_net_get_mac(uint8 mac[ETH_ALEN]);

    // Test functions
    int virtio_net_test_send(void);
    int virtio_net_test_recv(void);
    void virtio_net_debug_status(void);

// Architecture specific functions
#ifdef RISCV
#define VIRTIO_NET_MMIO_BASE 0x10008000
#define R_NET(r) ((volatile uint32 *)(VIRTIO_NET_MMIO_BASE + (r)))

    void virtio_net_init_mmio(void);
#elif defined(LOONGARCH)
    void virtio_net_init_pci(void);
    int virtio_net_probe_pci(void);
#endif
}