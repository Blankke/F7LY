#pragma once

#include "hal/arch.hh"
#include "spinlock.hh"
#include "types.hh"

// 这些常量只描述 VirtIO net 公共队列布局，不描述 MMIO/PCI 或任何板级资源。
#define NUM_NET_DESC 32    // Number of descriptors per queue (must be power of 2)
#define ETH_ALEN 6         // Ethernet address length
#define ETH_FRAME_LEN 1514 // Maximum Ethernet frame size

namespace net
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
        uint8 rx_buf_index[NUM_NET_DESC];
        uint8 tx_buf_index[NUM_NET_DESC];
        uint16 rx_used_idx;         // we've looked this far in rx_used
        uint16 tx_used_idx;         // we've looked this far in tx_used

        // Network buffers for packet storage
        struct net_buf rx_buffers[NUM_NET_DESC];
        struct net_buf tx_buffers[NUM_NET_DESC];

        // Device configuration
        uint8 mac_addr[ETH_ALEN];   // Device MAC address
        uint16 status;              // Link status
        uint16 max_virtqueue_pairs; // Number of supported queue pairs
        uint64 features;            // negotiated feature set
        bool initialized;
        bool link_up;

        // Synchronization
        SpinLock net_lock;
    };

    // Function declarations
    bool virtio_net_init(void);
    bool virtio_net_is_initialized(void);
    void virtio_net_poll(void);
    int virtio_net_send(const void *data, uint32 len);
    int virtio_net_recv(void *data, uint32 *len);
    void virtio_net_intr(void);
    bool virtio_net_link_up(void);
    void virtio_net_get_mac(uint8 mac[ETH_ALEN]);

    // Test functions
    int virtio_net_test_send(void);
    int virtio_net_test_recv(void);
    void virtio_net_debug_status(void);
}
