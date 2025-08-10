//
// VirtIO network driver for F7LY OS
// Supports basic packet transmission and reception
// Compatible with both RISC-V MMIO and LoongArch PCI implementations
//

#include "virtio_net.hh"
#include "platform.hh"
#include "mem/memlayout.hh"
#include "libs/string.hh"
#include "libs/klib.hh"
#include "libs/printer.hh"
#include "virtual_memory_manager.hh"
#include "proc_manager.hh"

#ifdef RISCV
#include "param.h"
#elif defined(LOONGARCH)
#include "trap/loongarch/pci.h"
#include "fs/drivers/loongarch/virtio_pci.hh"
#include "fs/drivers/loongarch/virtio_ring.hh"
#include "virtual_memory_manager.hh"
#endif
namespace net
{
    // Forward declarations
    static int virtio_net_alloc_rx_desc(void);

    // Global VirtIO network device instance
    static struct virtio_net_device virtio_net;

    // Initialize VirtIO network device
    void virtio_net_init(void)
    {
        memset(&virtio_net, 0, sizeof(virtio_net));
        virtio_net.net_lock.init("virtio_net");

#ifdef RISCV
        virtio_net_init_mmio();
#elif defined(LOONGARCH)
        // virtio_net_init_pci();
        printfRed("loongarch virtio net未实现");
#endif

        printf("VirtIO network device initialized\n");
    }

#ifdef RISCV
    // RISC-V MMIO-based initialization
    void virtio_net_init_mmio(void)
    {
        printf("[virtio_net_init_mmio] Initializing RISC-V MMIO VirtIO network device\n");
        uint32 status = 0;

        // Check magic value, version, device type, and vendor
        uint32 magic = *R_NET(VIRTIO_MMIO_MAGIC_VALUE);
        uint32 version = *R_NET(VIRTIO_MMIO_VERSION);
        uint32 device_id = *R_NET(VIRTIO_MMIO_DEVICE_ID);
        uint32 vendor_id = *R_NET(VIRTIO_MMIO_VENDOR_ID);
        
        printf("[virtio_net_init_mmio] Device info: magic=0x%x, version=%d, device_id=%d, vendor_id=0x%x\n",
               magic, version, device_id, vendor_id);
        
        if (magic != 0x74726976 || version != 1 || device_id != 1 || vendor_id != 0x554d4551)
        {
            printf("[virtio_net_init_mmio] ERROR: VirtIO network device not found or invalid\n");
            printf("[virtio_net_init_mmio] Expected: magic=0x74726976, version=1, device_id=1, vendor_id=0x554d4551\n");
            return;
        }

        printf("[virtio_net_init_mmio] ✓ VirtIO network device detected\n");

        // Reset device
        *R_NET(VIRTIO_MMIO_STATUS) = 0;

        // Acknowledge device
        status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
        *R_NET(VIRTIO_MMIO_STATUS) = status;

        // Set driver status
        status |= VIRTIO_CONFIG_S_DRIVER;
        *R_NET(VIRTIO_MMIO_STATUS) = status;

        // Feature negotiation - enable basic features
        uint64 features = *R_NET(VIRTIO_MMIO_DEVICE_FEATURES);

        // Enable basic features for simple operation
        features &= (1ULL << VIRTIO_NET_F_MAC); // MAC address feature
        features &= ~(1ULL << VIRTIO_NET_F_CSUM);
        features &= ~(1ULL << VIRTIO_NET_F_GUEST_CSUM);
        features &= ~(1ULL << VIRTIO_NET_F_MRG_RXBUF);
        features &= ~(1ULL << VIRTIO_NET_F_HOST_TSO4);
        features &= ~(1ULL << VIRTIO_NET_F_HOST_TSO6);
        features &= ~(1ULL << VIRTIO_NET_F_CTRL_VQ);
        features &= ~(1ULL << VIRTIO_F_ANY_LAYOUT);
        features &= ~(1ULL << VIRTIO_RING_F_EVENT_IDX);
        features &= ~(1ULL << VIRTIO_RING_F_INDIRECT_DESC);

        *R_NET(VIRTIO_MMIO_DRIVER_FEATURES) = features;

        // Feature negotiation complete
        status |= VIRTIO_CONFIG_S_FEATURES_OK;
        *R_NET(VIRTIO_MMIO_STATUS) = status;

        // Set page size
        *R_NET(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;

        // Initialize RX queue (queue 0)
        *R_NET(VIRTIO_MMIO_QUEUE_SEL) = VIRTIO_NET_RX_QUEUE_IDX;
        uint32 max_rx = *R_NET(VIRTIO_MMIO_QUEUE_NUM_MAX);
        if (max_rx == 0)
        {
            panic("VirtIO net has no RX queue");
        }
        if (max_rx < NUM_NET_DESC)
        {
            panic("VirtIO net RX queue too short");
        }
        *R_NET(VIRTIO_MMIO_QUEUE_NUM) = NUM_NET_DESC;
        *R_NET(VIRTIO_MMIO_QUEUE_ALIGN) = PGSIZE;

        // Setup RX queue memory layout
        virtio_net.rx_desc = (struct VRingDesc *)virtio_net.pages;
        virtio_net.rx_avail = (uint16 *)(virtio_net.pages + NUM_NET_DESC * sizeof(struct VRingDesc));
        virtio_net.rx_used = (struct VRingUsedArea *)(virtio_net.pages + PGSIZE);

        memset(virtio_net.pages, 0, PGSIZE);
        *R_NET(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)virtio_net.rx_desc) >> 12;

        // Initialize TX queue (queue 1)
        *R_NET(VIRTIO_MMIO_QUEUE_SEL) = VIRTIO_NET_TX_QUEUE_IDX;
        uint32 max_tx = *R_NET(VIRTIO_MMIO_QUEUE_NUM_MAX);
        if (max_tx == 0)
        {
            panic("VirtIO net has no TX queue");
        }
        if (max_tx < NUM_NET_DESC)
        {
            panic("VirtIO net TX queue too short");
        }
        *R_NET(VIRTIO_MMIO_QUEUE_NUM) = NUM_NET_DESC;
        *R_NET(VIRTIO_MMIO_QUEUE_ALIGN) = PGSIZE;

        // Setup TX queue memory layout
        virtio_net.tx_desc = (struct VRingDesc *)(virtio_net.pages + 2 * PGSIZE);
        virtio_net.tx_avail = (uint16 *)(virtio_net.pages + 2 * PGSIZE + NUM_NET_DESC * sizeof(struct VRingDesc));
        virtio_net.tx_used = (struct VRingUsedArea *)(virtio_net.pages + 3 * PGSIZE);

        memset(virtio_net.pages + 2 * PGSIZE, 0, PGSIZE);
        *R_NET(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)virtio_net.tx_desc) >> 12;

        // Mark all descriptors as free initially
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            virtio_net.rx_free[i] = 1;
            virtio_net.tx_free[i] = 1;
            virtio_net.rx_buffers[i].in_use = false;
            virtio_net.tx_buffers[i].in_use = false;
        }

        // Pre-populate RX queue with buffers
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            if (virtio_net_alloc_rx_desc() < 0)
            {
                break;
            }
        }

        // Device ready
        status |= VIRTIO_CONFIG_S_DRIVER_OK;
        *R_NET(VIRTIO_MMIO_STATUS) = status;

        // Read MAC address from device config (if supported)
        if (features & (1ULL << VIRTIO_NET_F_MAC))
        {
            // MAC address is located at offset 0 in device config space
            // Note: This is simplified - real implementation might need config space access
            for (int i = 0; i < ETH_ALEN; i++)
            {
                virtio_net.mac_addr[i] = 0x52 + i; // Default test MAC: 52:53:54:55:56:57
            }
        }
    }
#endif

#ifdef LOONGARCH
    // LoongArch PCI-based initialization
    void virtio_net_init_pci(void)
    {
        printf("[virtio_net_init_pci] Initializing LoongArch PCI VirtIO network device\n");
        
        // Probe for VirtIO network device on PCI bus
        if (virtio_net_probe_pci() != 0)
        {
            printf("[virtio_net_init_pci] No VirtIO network device found on PCI bus\n");
            return;
        }

        uint8 status = 0;

        // 1. Reset device
        virtio_pci_set_status(&virtio_net.virtio_net_hw, 0);

        // 2. Set acknowledge status
        status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
        virtio_pci_set_status(&virtio_net.virtio_net_hw, status);

        // 3. Set driver status
        status |= VIRTIO_CONFIG_S_DRIVER;
        virtio_pci_set_status(&virtio_net.virtio_net_hw, status);

        // 4. Feature negotiation
        uint64 features = virtio_pci_get_device_features(&virtio_net.virtio_net_hw);
        printf("[virtio_net_init_pci] Device features: 0x%lx\n", features);

        // Enable only basic features for simple operation
        features &= (1ULL << VIRTIO_NET_F_MAC);
        features &= ~(1ULL << VIRTIO_NET_F_CSUM);
        features &= ~(1ULL << VIRTIO_NET_F_GUEST_CSUM);
        features &= ~(1ULL << VIRTIO_NET_F_MRG_RXBUF);
        features &= ~(1ULL << VIRTIO_NET_F_HOST_TSO4);
        features &= ~(1ULL << VIRTIO_NET_F_HOST_TSO6);
        features &= ~(1ULL << VIRTIO_NET_F_CTRL_VQ);
        features &= ~(1ULL << VIRTIO_F_ANY_LAYOUT);
        features &= ~(1ULL << VIRTIO_RING_F_EVENT_IDX);
        features &= ~(1ULL << VIRTIO_RING_F_INDIRECT_DESC);

        virtio_pci_set_driver_features(&virtio_net.virtio_net_hw, features);

        // 5. Feature negotiation complete
        status |= VIRTIO_CONFIG_S_FEATURES_OK;
        virtio_pci_set_status(&virtio_net.virtio_net_hw, status);

        // 6. Check if features were accepted
        status = virtio_pci_get_status(&virtio_net.virtio_net_hw);
        if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        {
            panic("VirtIO net device did not accept features");
        }

        // 7. Initialize RX queue (queue 0)
        uint32 qsize = virtio_pci_get_queue_size(&virtio_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX);
        printf("[virtio_net_init_pci] RX queue max size: %d\n", qsize);
        if (qsize == 0)
        {
            panic("VirtIO net has no RX queue");
        }
        if (qsize < NUM_NET_DESC)
        {
            panic("VirtIO net RX queue too short");
        }

        virtio_pci_set_queue_size(&virtio_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX, NUM_NET_DESC);

        // Setup RX queue memory layout
        memset(virtio_net.pages, 0, sizeof(virtio_net.pages));
        virtio_net.rx_desc = (struct VRingDesc *)virtio_net.pages;
        virtio_net.rx_avail = (uint16 *)(virtio_net.pages + NUM_NET_DESC * sizeof(struct VRingDesc));
        virtio_net.rx_used = (struct VRingUsedArea *)(virtio_net.pages + PGSIZE);

        virtio_pci_set_queue_addr2(&virtio_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX,
                                   virtio_net.rx_desc, virtio_net.rx_avail, virtio_net.rx_used);
        virtio_pci_set_queue_enable(&virtio_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX);

        // 8. Initialize TX queue (queue 1)
        qsize = virtio_pci_get_queue_size(&virtio_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX);
        printf("[virtio_net_init_pci] TX queue max size: %d\n", qsize);
        if (qsize == 0)
        {
            panic("VirtIO net has no TX queue");
        }
        if (qsize < NUM_NET_DESC)
        {
            panic("VirtIO net TX queue too short");
        }

        virtio_pci_set_queue_size(&virtio_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX, NUM_NET_DESC);

        // Setup TX queue memory layout
        virtio_net.tx_desc = (struct VRingDesc *)(virtio_net.pages + 2 * PGSIZE);
        virtio_net.tx_avail = (uint16 *)(virtio_net.pages + 2 * PGSIZE + NUM_NET_DESC * sizeof(struct VRingDesc));
        virtio_net.tx_used = (struct VRingUsedArea *)(virtio_net.pages + 3 * PGSIZE);

        virtio_pci_set_queue_addr2(&virtio_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX,
                                   virtio_net.tx_desc, virtio_net.tx_avail, virtio_net.tx_used);
        virtio_pci_set_queue_enable(&virtio_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX);

        // 9. Mark all descriptors as free initially
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            virtio_net.rx_free[i] = 1;
            virtio_net.tx_free[i] = 1;
            virtio_net.rx_buffers[i].in_use = false;
            virtio_net.tx_buffers[i].in_use = false;
        }

        // 10. Pre-populate RX queue with buffers
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            if (virtio_net_alloc_rx_desc() < 0)
            {
                break;
            }
        }

        // 11. Device ready
        status |= VIRTIO_CONFIG_S_DRIVER_OK;
        virtio_pci_set_status(&virtio_net.virtio_net_hw, status);

        // 12. Set default MAC address
        for (int i = 0; i < ETH_ALEN; i++)
        {
            virtio_net.mac_addr[i] = 0x52 + i; // Default test MAC: 52:53:54:55:56:57
        }

        printf("[virtio_net_init_pci] ✓ VirtIO network device initialized successfully\n");
    }

    // Probe for VirtIO network device on PCI bus
    int virtio_net_probe_pci(void)
    {
        printf("[virtio_net_probe_pci] Probing for VirtIO network device\n");
        
        uint64 pci_base = pci_device_probe(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID);
        if (pci_base == 0)
        {
            printf("[virtio_net_probe_pci] VirtIO network device not found\n");
            return -1;
        }

        virtio_net.pci_dev = pci_base;
        virtio_net.port_id = 0; // First network device

        printf("[virtio_net_probe_pci] Found VirtIO network device at PCI base: 0x%lx\n", pci_base);

        // Extract bus, device, function from pci_base offset
        uint8 bus = (pci_base >> 16) & 0xFF;
        uint8 device = (pci_base >> 11) & 0x1F;
        uint8 function = (pci_base >> 8) & 0x7;

        // Map PCI configuration space and MMIO space - this is crucial!
        mem::k_vmm.pci_map(bus, device, function, virtio_net.pages);
        printf("[virtio_net_probe_pci] PCI memory mapping completed for bus=%d, dev=%d, func=%d\n", bus, device, function);

        // Initialize PCI device - enable memory and bus master
        uint16 cmd = pci_config_read16(pci_base + PCI_STATUS_COMMAND);
        printf("[virtio_net_probe_pci] Original command register: 0x%x\n", cmd);
        cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_IO;
        pci_config_write16(pci_base + PCI_STATUS_COMMAND, cmd);
        uint16 new_cmd = pci_config_read16(pci_base + PCI_STATUS_COMMAND);
        printf("[virtio_net_probe_pci] New command register: 0x%x\n", new_cmd);

        // Read and setup PCI capabilities for VirtIO
        printf("[virtio_net_probe_pci] Reading PCI capabilities...\n");
        if (virtio_pci_read_caps(&virtio_net.virtio_net_hw, pci_base, 0) != 0)
        {
            printf("[virtio_net_probe_pci] Failed to read PCI capabilities\n");
            return -1;
        }

        printf("[virtio_net_probe_pci] ✓ PCI capabilities read successfully\n");
        return 0;
    }
#endif

    // Allocate and populate a receive descriptor
    static int virtio_net_alloc_rx_desc(void)
    {
        virtio_net.net_lock.acquire();

        // Find free descriptor
        int desc_idx = -1;
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            if (virtio_net.rx_free[i])
            {
                desc_idx = i;
                virtio_net.rx_free[i] = 0;
                break;
            }
        }

        if (desc_idx < 0)
        {
            virtio_net.net_lock.release();
            return -1; // No free descriptors
        }

        // Find free buffer
        int buf_idx = -1;
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            if (!virtio_net.rx_buffers[i].in_use)
            {
                buf_idx = i;
                virtio_net.rx_buffers[i].in_use = true;
                break;
            }
        }

        if (buf_idx < 0)
        {
            virtio_net.rx_free[desc_idx] = 1;
            virtio_net.net_lock.release();
            return -1; // No free buffers
        }

        // Setup descriptor for device to write incoming packet
        virtio_net.rx_desc[desc_idx].addr = (uint64) mem::k_pagetable.kwalk_addr((uint64)virtio_net.rx_buffers[buf_idx].data);
        virtio_net.rx_desc[desc_idx].len = sizeof(virtio_net.rx_buffers[buf_idx].data);
        virtio_net.rx_desc[desc_idx].flags = VRING_DESC_F_WRITE; // Device writes
        virtio_net.rx_desc[desc_idx].next = 0;

        // Add to available ring
        uint16 idx = virtio_net.rx_avail[1] % NUM_NET_DESC;
        virtio_net.rx_avail[2 + idx] = desc_idx;
        __sync_synchronize();
        virtio_net.rx_avail[1] = virtio_net.rx_avail[1] + 1;

        // Notify device
#ifdef RISCV
        *R_NET(VIRTIO_MMIO_QUEUE_SEL) = VIRTIO_NET_RX_QUEUE_IDX;
        *R_NET(VIRTIO_MMIO_QUEUE_NOTIFY) = VIRTIO_NET_RX_QUEUE_IDX;
#elif defined(LOONGARCH)
        virtio_pci_set_queue_notify(&virtio_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX);
#endif

        virtio_net.net_lock.release();
        return 0;
    }

    // Send a network packet
    int virtio_net_send(const void *data, uint32 len)
    {
        printf("[virtio_net_send] Attempting to send packet of length %d\n", len);
        
        if (len > ETH_FRAME_LEN)
        {
            printf("[virtio_net_send] ERROR: Packet too large (%d > %d)\n", len, ETH_FRAME_LEN);
            return -1; // Packet too large
        }

        virtio_net.net_lock.acquire();

        // Find free TX descriptor
        int desc_idx = -1;
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            if (virtio_net.tx_free[i])
            {
                desc_idx = i;
                virtio_net.tx_free[i] = 0;
                break;
            }
        }

        if (desc_idx < 0)
        {
            printf("[virtio_net_send] ERROR: No free TX descriptors\n");
            virtio_net.net_lock.release();
            return -1; // No free descriptors
        }

        // Find free buffer
        int buf_idx = -1;
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            if (!virtio_net.tx_buffers[i].in_use)
            {
                buf_idx = i;
                virtio_net.tx_buffers[i].in_use = true;
                break;
            }
        }

        if (buf_idx < 0)
        {
            printf("[virtio_net_send] ERROR: No free TX buffers\n");
            virtio_net.tx_free[desc_idx] = 1;
            virtio_net.net_lock.release();
            return -1; // No free buffers
        }

        printf("[virtio_net_send] Using TX descriptor %d, buffer %d\n", desc_idx, buf_idx);

        // Setup VirtIO net header (zeros for simple transmission)
        struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)virtio_net.tx_buffers[buf_idx].data;
        memset(hdr, 0, sizeof(*hdr));

        // Copy packet data after header
        memcpy(virtio_net.tx_buffers[buf_idx].data + sizeof(*hdr), data, len);
        virtio_net.tx_buffers[buf_idx].len = len + sizeof(*hdr);

        // Setup descriptor for transmission
        virtio_net.tx_desc[desc_idx].addr = (uint64) mem::k_pagetable.kwalk_addr((uint64)virtio_net.tx_buffers[buf_idx].data);
        virtio_net.tx_desc[desc_idx].len = virtio_net.tx_buffers[buf_idx].len;
        virtio_net.tx_desc[desc_idx].flags = 0; // Device reads
        virtio_net.tx_desc[desc_idx].next = 0;

        // Add to available ring
        uint16 idx = virtio_net.tx_avail[1] % NUM_NET_DESC;
        virtio_net.tx_avail[2 + idx] = desc_idx;
        __sync_synchronize();
        virtio_net.tx_avail[1] = virtio_net.tx_avail[1] + 1;

        // Notify device
#ifdef RISCV
        *R_NET(VIRTIO_MMIO_QUEUE_SEL) = VIRTIO_NET_TX_QUEUE_IDX;
        *R_NET(VIRTIO_MMIO_QUEUE_NOTIFY) = VIRTIO_NET_TX_QUEUE_IDX;
        printf("[virtio_net_send] Notified device via MMIO\n");
#elif defined(LOONGARCH)
        virtio_pci_set_queue_notify(&virtio_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX);
        printf("[virtio_net_send] Notified device via PCI\n");
#endif

        virtio_net.net_lock.release();
        printf("[virtio_net_send] Packet queued for transmission successfully\n");
        return 0;
    }

    // Receive a network packet (non-blocking)
    int virtio_net_recv(void *data, uint32 *len)
    {
        virtio_net.net_lock.acquire();

        // Check if any packets are available in used ring
        if (virtio_net.rx_used_idx == virtio_net.rx_used->idx)
        {
            virtio_net.net_lock.release();
            return -1; // No packets available
        }

        // Get completed descriptor
        struct VRingUsedElem *used_elem = &virtio_net.rx_used->ring[virtio_net.rx_used_idx % NUM_NET_DESC];
        int desc_idx = used_elem->id;
        uint32 packet_len = used_elem->len;

        if (packet_len <= sizeof(struct virtio_net_hdr))
        {
            // Packet too small or empty
            virtio_net.rx_free[desc_idx] = 1;
            virtio_net.rx_used_idx++;
            virtio_net.net_lock.release();
            return -1;
        }

        // Find corresponding buffer
        int buf_idx = -1;
        for (int i = 0; i < NUM_NET_DESC; i++)
        {
            if (virtio_net.rx_buffers[i].in_use &&
                (uint64) mem::k_pagetable.kwalk_addr((uint64)virtio_net.rx_buffers[i].data) == virtio_net.rx_desc[desc_idx].addr)
            {
                buf_idx = i;
                break;
            }
        }

        if (buf_idx < 0)
        {
            // Buffer not found - should not happen
            virtio_net.rx_free[desc_idx] = 1;
            virtio_net.rx_used_idx++;
            virtio_net.net_lock.release();
            return -1;
        }

        // Extract packet data (skip VirtIO header)
        uint32 data_len = packet_len - sizeof(struct virtio_net_hdr);
        if (data_len > *len)
        {
            data_len = *len; // Truncate if buffer too small
        }

        memcpy(data, virtio_net.rx_buffers[buf_idx].data + sizeof(struct virtio_net_hdr), data_len);
        *len = data_len;

        // Free descriptor and buffer
        virtio_net.rx_free[desc_idx] = 1;
        virtio_net.rx_buffers[buf_idx].in_use = false;
        virtio_net.rx_used_idx++;

        virtio_net.net_lock.release();

        // Allocate new RX descriptor to replace the one we just consumed
        virtio_net_alloc_rx_desc();

        return 0;
    }

    // Handle VirtIO network interrupts
    void virtio_net_intr(void)
    {
        printf("[virtio_net_intr] Network interrupt received\n");
        
#ifdef RISCV
        // Read and acknowledge interrupt status
        uint32 intr_status = *R_NET(VIRTIO_MMIO_INTERRUPT_STATUS);
        *R_NET(VIRTIO_MMIO_INTERRUPT_ACK) = intr_status;
        printf("[virtio_net_intr] MMIO interrupt status: 0x%x\n", intr_status);
#elif defined(LOONGARCH)
        // Clear PCI interrupt status
        uint32 isr_status = virtio_pci_clear_isr(&virtio_net.virtio_net_hw);
        printf("[virtio_net_intr] PCI ISR status: 0x%x\n", isr_status);
#endif

        virtio_net.net_lock.acquire();

        // Process completed TX descriptors
        int tx_completed = 0;
        while (virtio_net.tx_used_idx != virtio_net.tx_used->idx)
        {
            struct VRingUsedElem *used_elem = &virtio_net.tx_used->ring[virtio_net.tx_used_idx % NUM_NET_DESC];
            int desc_idx = used_elem->id;

            printf("[virtio_net_intr] TX descriptor %d completed\n", desc_idx);

            // Find and free corresponding buffer
            for (int i = 0; i < NUM_NET_DESC; i++)
            {
                if (virtio_net.tx_buffers[i].in_use &&
                    (uint64) mem::k_pagetable.kwalk_addr((uint64)virtio_net.tx_buffers[i].data) == virtio_net.tx_desc[desc_idx].addr)
                {
                    virtio_net.tx_buffers[i].in_use = false;
                    break;
                }
            }

            // Free descriptor
            virtio_net.tx_free[desc_idx] = 1;
            virtio_net.tx_used_idx++;
            tx_completed++;
        }

        if (tx_completed > 0) {
            printf("[virtio_net_intr] %d TX packets completed\n", tx_completed);
        }

        virtio_net.net_lock.release();

        // Wake up any processes waiting for TX completion
        proc::k_pm.wakeup(&virtio_net.tx_free[0]);
    }

    // Check if network link is up
    bool virtio_net_link_up(void)
    {
        // For simplicity, always return true
        // Real implementation would check device status register
        return true;
    }

    // Get device MAC address
    void virtio_net_get_mac(uint8 mac[ETH_ALEN])
    {
        for (int i = 0; i < ETH_ALEN; i++)
        {
            mac[i] = virtio_net.mac_addr[i];
        }
    }

    // Create a simple ARP packet for testing
    static void create_arp_packet(uint8 *packet, const uint8 src_mac[ETH_ALEN], const uint8 dst_mac[ETH_ALEN],
                                  uint32 src_ip, uint32 dst_ip, uint16 operation)
    {
        struct eth_hdr *eth = (struct eth_hdr *)packet;

        // Ethernet header
        memcpy(eth->dst_mac, dst_mac, ETH_ALEN);
        memcpy(eth->src_mac, src_mac, ETH_ALEN);
        eth->ethertype = __builtin_bswap16(0x0806); // ARP

        // ARP header
        uint8 *arp = packet + sizeof(struct eth_hdr);
        *((uint16 *)(arp + 0)) = __builtin_bswap16(1);         // Hardware type: Ethernet
        *((uint16 *)(arp + 2)) = __builtin_bswap16(0x0800);    // Protocol type: IPv4
        arp[4] = ETH_ALEN;                                     // Hardware address length
        arp[5] = 4;                                            // Protocol address length
        *((uint16 *)(arp + 6)) = __builtin_bswap16(operation); // Operation

        memcpy(arp + 8, src_mac, ETH_ALEN);  // Sender MAC
        *((uint32 *)(arp + 14)) = src_ip;    // Sender IP
        memcpy(arp + 18, dst_mac, ETH_ALEN); // Target MAC
        *((uint32 *)(arp + 24)) = dst_ip;    // Target IP
    }

    // Simple test function to send an ARP packet
    int virtio_net_test_send(void)
    {
        uint8 packet[64];
        uint8 broadcast_mac[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        uint8 my_mac[ETH_ALEN];

        virtio_net_get_mac(my_mac);

        // Create ARP request: Who has 192.168.1.1?
        create_arp_packet(packet, my_mac, broadcast_mac,
                          __builtin_bswap32(0xC0A80102), // 192.168.1.2 (our IP)
                          __builtin_bswap32(0xC0A80101), // 192.168.1.1 (gateway)
                          1);                            // ARP request

        return virtio_net_send(packet, sizeof(struct eth_hdr) + 28); // Eth header + ARP
    }

    // Simple test function to check for received packets
    int virtio_net_test_recv(void)
    {
        uint8 packet[ETH_FRAME_LEN];
        uint32 len = sizeof(packet);

        if (virtio_net_recv(packet, &len) == 0)
        {
            printf("Received packet of length %d\n", len);

            // Print first few bytes for debugging
            printf("Packet data: ");
            for (int i = 0; i < (int)MIN(len, 16); i++)
            {
                printf("%02x ", packet[i]);
            }
            printf("\n");

            return 0;
        }

        return -1;
    }

    // Debug function to check queue status
    void virtio_net_debug_status(void)
    {
        printf("[virtio_net_debug] === Network Device Status ===\n");
        
        // Print MAC address
        printf("[virtio_net_debug] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               virtio_net.mac_addr[0], virtio_net.mac_addr[1], virtio_net.mac_addr[2],
               virtio_net.mac_addr[3], virtio_net.mac_addr[4], virtio_net.mac_addr[5]);
        
        // Print TX queue status
        int tx_free_count = 0;
        int tx_used_count = 0;
        for (int i = 0; i < NUM_NET_DESC; i++) {
            if (virtio_net.tx_free[i]) tx_free_count++;
            if (virtio_net.tx_buffers[i].in_use) tx_used_count++;
        }
        printf("[virtio_net_debug] TX: %d free descriptors, %d buffers in use\n", tx_free_count, tx_used_count);
        printf("[virtio_net_debug] TX used_idx: %d, avail_idx: %d\n", 
               virtio_net.tx_used_idx, virtio_net.tx_avail[1]);
        
        // Print RX queue status  
        int rx_free_count = 0;
        int rx_used_count = 0;
        for (int i = 0; i < NUM_NET_DESC; i++) {
            if (virtio_net.rx_free[i]) rx_free_count++;
            if (virtio_net.rx_buffers[i].in_use) rx_used_count++;
        }
        printf("[virtio_net_debug] RX: %d free descriptors, %d buffers in use\n", rx_free_count, rx_used_count);
        printf("[virtio_net_debug] RX used_idx: %d, avail_idx: %d\n", 
               virtio_net.rx_used_idx, virtio_net.rx_avail[1]);
        
#ifdef RISCV
        // Read device status
        uint32 device_status = *R_NET(VIRTIO_MMIO_STATUS);
        printf("[virtio_net_debug] Device status: 0x%x\n", device_status);
        
        // Check if device is ready
        if (device_status & VIRTIO_CONFIG_S_DRIVER_OK) {
            printf("[virtio_net_debug] ✓ Device is ready\n");
        } else {
            printf("[virtio_net_debug] ✗ Device is not ready\n");
        }
#endif
        
        printf("[virtio_net_debug] ===============================\n");
    }
}