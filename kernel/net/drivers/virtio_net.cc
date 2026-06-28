#include "virtio_net.hh"

#include "libs/klib.hh"
#include "libs/printer.hh"
#include "libs/string.hh"
#include "mem/memlayout.hh"
#include "param.h"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"

#ifdef LOONGARCH
#include "trap/loongarch/pci.h"
#endif

namespace net
{
    namespace
    {
        constexpr uint32 k_virtio_magic = 0x74726976;
        constexpr uint32 k_virtio_vendor_qemu = 0x554d4551;
        constexpr uint32 k_virtio_device_net = 1;
        constexpr uint32 k_net_header_len = sizeof(virtio_net_hdr);

        static virtio_net_device g_net;

#ifdef RISCV
        inline volatile uint32 *mmio_reg(uint64 base, uint32 offset)
        {
            return reinterpret_cast<volatile uint32 *>(base + offset);
        }

        uint32 mmio_read(uint32 offset)
        {
            return *mmio_reg(g_net.mmio_base, offset);
        }

        void mmio_write(uint32 offset, uint32 value)
        {
            *mmio_reg(g_net.mmio_base, offset) = value;
        }

        int mmio_irq_for_base(uint64 base)
        {
            return VIRTIO_MMIO_IRQ_FIRST +
                   static_cast<int>((base - VIRTIO_MMIO_FIRST) / VIRTIO_MMIO_STRIDE);
        }

        bool find_net_mmio_device(uint64 *base_out, int *irq_out)
        {
            for (uint64 base = VIRTIO_MMIO_FIRST; base <= VIRTIO_MMIO_LAST; base += VIRTIO_MMIO_STRIDE)
            {
                uint32 magic = *mmio_reg(base, VIRTIO_MMIO_MAGIC_VALUE);
                uint32 version = *mmio_reg(base, VIRTIO_MMIO_VERSION);
                uint32 device_id = *mmio_reg(base, VIRTIO_MMIO_DEVICE_ID);
                uint32 vendor_id = *mmio_reg(base, VIRTIO_MMIO_VENDOR_ID);

                if (magic == k_virtio_magic && version == 1 &&
                    device_id == k_virtio_device_net && vendor_id == k_virtio_vendor_qemu)
                {
                    *base_out = base;
                    *irq_out = mmio_irq_for_base(base);
                    return true;
                }
            }
            return false;
        }
#endif

        uint64 dma_addr(const void *ptr)
        {
            uint64 pa = mem::k_pagetable.kwalk_addr(reinterpret_cast<uint64>(ptr));
            if (pa == 0)
            {
                panic("virtio net: dma addr translate failed");
            }
            return pa;
        }

        uint16 avail_idx(uint16 *avail)
        {
            return avail[1];
        }

        void set_avail_idx(uint16 *avail, uint16 value)
        {
            avail[1] = value;
        }

        void reset_default_mac()
        {
            static const uint8 fallback_mac[ETH_ALEN] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
            memcpy(g_net.mac_addr, fallback_mac, sizeof(fallback_mac));
        }

        void init_queue_layout()
        {
            memset(g_net.pages, 0, sizeof(g_net.pages));

            g_net.rx_desc = reinterpret_cast<VRingDesc *>(g_net.pages);
            g_net.rx_avail = reinterpret_cast<uint16 *>(g_net.pages + NUM_NET_DESC * sizeof(VRingDesc));
            g_net.rx_used = reinterpret_cast<VRingUsedArea *>(g_net.pages + PGSIZE);

            g_net.tx_desc = reinterpret_cast<VRingDesc *>(g_net.pages + 2 * PGSIZE);
            g_net.tx_avail = reinterpret_cast<uint16 *>(g_net.pages + 2 * PGSIZE + NUM_NET_DESC * sizeof(VRingDesc));
            g_net.tx_used = reinterpret_cast<VRingUsedArea *>(g_net.pages + 3 * PGSIZE);

            for (int i = 0; i < NUM_NET_DESC; ++i)
            {
                g_net.rx_free[i] = 1;
                g_net.tx_free[i] = 1;
                g_net.rx_buf_index[i] = i;
                g_net.tx_buf_index[i] = i;
                g_net.rx_buffers[i].in_use = false;
                g_net.tx_buffers[i].in_use = false;
            }
            g_net.rx_used_idx = 0;
            g_net.tx_used_idx = 0;
        }

        uint64 negotiate_features(uint64 device_features)
        {
            // 当前驱动只收发完整以太网帧，不启用校验和、TSO、mergeable buffer 等复杂特性。
            uint64 features = device_features & (1ULL << VIRTIO_NET_F_MAC);
            return features;
        }

        void notify_queue(uint16 queue_index)
        {
#ifdef RISCV
            mmio_write(VIRTIO_MMIO_QUEUE_NOTIFY, queue_index);
#elif defined(LOONGARCH)
            virtio_pci_set_queue_notify(&g_net.virtio_net_hw, queue_index);
#endif
        }

        void ack_interrupt()
        {
#ifdef RISCV
            if (g_net.mmio_base == 0)
            {
                return;
            }
            uint32 intr_status = mmio_read(VIRTIO_MMIO_INTERRUPT_STATUS);
            if (intr_status != 0)
            {
                mmio_write(VIRTIO_MMIO_INTERRUPT_ACK, intr_status);
            }
#elif defined(LOONGARCH)
            virtio_pci_clear_isr(&g_net.virtio_net_hw);
#endif
        }

        int post_rx_desc_locked(int desc_idx)
        {
            if (desc_idx < 0 || desc_idx >= NUM_NET_DESC || g_net.rx_free[desc_idx] == 0)
            {
                return -1;
            }

            int buf_idx = desc_idx;
            g_net.rx_free[desc_idx] = 0;
            g_net.rx_buf_index[desc_idx] = static_cast<uint8>(buf_idx);
            g_net.rx_buffers[buf_idx].in_use = true;
            g_net.rx_buffers[buf_idx].len = 0;

            g_net.rx_desc[desc_idx].addr = dma_addr(g_net.rx_buffers[buf_idx].data);
            g_net.rx_desc[desc_idx].len = sizeof(g_net.rx_buffers[buf_idx].data);
            g_net.rx_desc[desc_idx].flags = VRING_DESC_F_WRITE;
            g_net.rx_desc[desc_idx].next = 0;

            uint16 idx = avail_idx(g_net.rx_avail);
            g_net.rx_avail[2 + (idx % NUM_NET_DESC)] = static_cast<uint16>(desc_idx);
            __sync_synchronize();
            set_avail_idx(g_net.rx_avail, idx + 1);
            return 0;
        }

        void post_all_rx_desc_locked()
        {
            for (int i = 0; i < NUM_NET_DESC; ++i)
            {
                if (post_rx_desc_locked(i) == 0)
                {
                    continue;
                }
                panic("virtio net: failed to post rx descriptor");
            }
        }

        void process_tx_used_locked()
        {
            __sync_synchronize();
            while (g_net.tx_used_idx != g_net.tx_used->idx)
            {
                VRingUsedElem *elem = &g_net.tx_used->ring[g_net.tx_used_idx % NUM_NET_DESC];
                uint32 desc_idx = elem->id;
                if (desc_idx < NUM_NET_DESC)
                {
                    uint8 buf_idx = g_net.tx_buf_index[desc_idx];
                    if (buf_idx < NUM_NET_DESC)
                    {
                        g_net.tx_buffers[buf_idx].in_use = false;
                        g_net.tx_buffers[buf_idx].len = 0;
                    }
                    g_net.tx_free[desc_idx] = 1;
                }
                ++g_net.tx_used_idx;
            }
        }

        bool prepare_common_state()
        {
            memset(&g_net, 0, sizeof(g_net));
            g_net.net_lock.init("virtio_net");
            reset_default_mac();
            init_queue_layout();
            return true;
        }

#ifdef LOONGARCH
        void pci_device_init(uint64 pci_base, unsigned char bus, unsigned char device, unsigned char function)
        {
            mem::k_vmm.pci_map(bus, device, function, nullptr);

            uint16 command = pci_config_read16(pci_base + PCI_STATUS_COMMAND);
            command |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_IO;
            pci_config_write16(pci_base + PCI_STATUS_COMMAND, command);

            uint64 off = (static_cast<uint64>(bus) << 16) |
                         (static_cast<uint64>(device) << 11) |
                         (static_cast<uint64>(function) << 8);
            volatile uint32 *base = reinterpret_cast<volatile uint32 *>(PCIE0_ECAM + off);
            for (int i = 0; i < 6; ++i)
            {
                uint32 old = base[4 + i];
                if ((old & 0x1) != 0)
                {
                    continue;
                }
                if ((old & 0x4) != 0)
                {
                    base[4 + i] = 0xffffffff;
                    base[4 + i + 1] = 0xffffffff;
                    __sync_synchronize();

                    uint64 size = (static_cast<uint64>(base[4 + i + 1]) << 32) | base[4 + i];
                    size = ~(size & 0xfffffffffffffff0ULL) + 1;
                    uint64 mem_addr = pci_alloc_mmio(size);
                    base[4 + i] = static_cast<uint32>(mem_addr);
                    base[4 + i + 1] = static_cast<uint32>(mem_addr >> 32);
                    __sync_synchronize();
                    ++i;
                }
            }
        }
#endif
    } // namespace

    bool virtio_net_init(void)
    {
        // 底层 VirtIO 网卡初始化入口。上层 adapter_init 会先调用这里，
        // 成功后 ONPS 才能通过 virtio_net_send/recv 发送和接收以太网帧。
        if (g_net.initialized)
        {
            return true;
        }

        // 初始化通用内存布局、descriptor/ring 指针、空闲表和默认 MAC 等状态。
        prepare_common_state();
#ifdef RISCV
        // RISC-V virt 机器使用 virtio-mmio 设备。
        bool ok = virtio_net_init_mmio();
#elif defined(LOONGARCH)
        // LoongArch 当前使用 PCI 形式的 VirtIO 设备。
        bool ok = virtio_net_init_pci();
#else
        bool ok = false;
#endif
        if (!ok)
        {
            printf("virtio net: device initialization failed\n");
            return false;
        }

        printf("virtio net: initialized, mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               g_net.mac_addr[0], g_net.mac_addr[1], g_net.mac_addr[2],
               g_net.mac_addr[3], g_net.mac_addr[4], g_net.mac_addr[5]);
        return true;
    }

    bool virtio_net_is_initialized(void)
    {
        return g_net.initialized;
    }

#ifdef RISCV
    bool virtio_net_init_mmio(void)
    {
        // 扫描 QEMU virtio-mmio 槽位，找到 device_id=1 的网络设备。
        uint64 base = 0;
        int irq = 0;
        if (!find_net_mmio_device(&base, &irq))
        {
            printf("virtio net: mmio device not found in %p..%p\n",
                   VIRTIO_MMIO_FIRST, VIRTIO_MMIO_LAST);
            return false;
        }
        g_net.mmio_base = base;
        g_net.irq = irq;

        uint32 status = 0;
        // VirtIO 初始化第一步：清空设备状态，开始一次新的驱动协商。
        mmio_write(VIRTIO_MMIO_STATUS, 0);

        // ACKNOWLEDGE 表示 guest 发现设备。
        status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
        mmio_write(VIRTIO_MMIO_STATUS, status);
        // DRIVER 表示 guest 知道如何驱动这个设备。
        status |= VIRTIO_CONFIG_S_DRIVER;
        mmio_write(VIRTIO_MMIO_STATUS, status);

        // 读取设备支持的 feature，选择本驱动能处理的一小部分写回。
        uint64 features = negotiate_features(mmio_read(VIRTIO_MMIO_DEVICE_FEATURES));
        mmio_write(VIRTIO_MMIO_DRIVER_FEATURES, static_cast<uint32>(features));

        // FEATURES_OK 表示 feature 协商完成；设备可能拒绝不合法组合。
        status |= VIRTIO_CONFIG_S_FEATURES_OK;
        mmio_write(VIRTIO_MMIO_STATUS, status);
        if ((mmio_read(VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK) == 0)
        {
            printf("virtio net: mmio FEATURES_OK was rejected\n");
            return false;
        }

        // legacy MMIO 设备用 guest page size 和 PFN 告诉设备 ring 在物理内存哪里。
        mmio_write(VIRTIO_MMIO_GUEST_PAGE_SIZE, PGSIZE);

        // 配置 RX queue：设备收到包后，会把数据写进我们提供的 RX descriptor buffer。
        mmio_write(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_RX_QUEUE_IDX);
        uint32 max_rx = mmio_read(VIRTIO_MMIO_QUEUE_NUM_MAX);
        if (max_rx < NUM_NET_DESC)
        {
            printf("virtio net: rx queue too small, max=%d\n", max_rx);
            return false;
        }
        mmio_write(VIRTIO_MMIO_QUEUE_NUM, NUM_NET_DESC);
        mmio_write(VIRTIO_MMIO_QUEUE_ALIGN, PGSIZE);
        mmio_write(VIRTIO_MMIO_QUEUE_PFN, static_cast<uint32>(dma_addr(g_net.rx_desc) >> PGSHIFT));

        // 配置 TX queue：我们要发送包时，把 TX descriptor 放进这个队列给设备读。
        mmio_write(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_TX_QUEUE_IDX);
        uint32 max_tx = mmio_read(VIRTIO_MMIO_QUEUE_NUM_MAX);
        if (max_tx < NUM_NET_DESC)
        {
            printf("virtio net: tx queue too small, max=%d\n", max_tx);
            return false;
        }
        mmio_write(VIRTIO_MMIO_QUEUE_NUM, NUM_NET_DESC);
        mmio_write(VIRTIO_MMIO_QUEUE_ALIGN, PGSIZE);
        mmio_write(VIRTIO_MMIO_QUEUE_PFN, static_cast<uint32>(dma_addr(g_net.tx_desc) >> PGSHIFT));

        if ((features & (1ULL << VIRTIO_NET_F_MAC)) != 0)
        {
            // 如果设备提供 MAC feature，就从设备配置空间读取真实 MAC。
            volatile uint8 *config = reinterpret_cast<volatile uint8 *>(g_net.mmio_base + VIRTIO_MMIO_CONFIG);
            for (int i = 0; i < ETH_ALEN; ++i)
            {
                g_net.mac_addr[i] = config[i];
            }
        }
        g_net.features = features;
        g_net.link_up = true;

        g_net.net_lock.acquire();
        // RX 队列必须先投递一批可写 buffer，否则设备即使收到包也没地方 DMA。
        post_all_rx_desc_locked();
        g_net.net_lock.release();

        // DRIVER_OK 表示驱动准备完毕，设备可以开始收发。
        status |= VIRTIO_CONFIG_S_DRIVER_OK;
        mmio_write(VIRTIO_MMIO_STATUS, status);
        // 通知设备 RX queue 已经有可用 buffer。
        notify_queue(VIRTIO_NET_RX_QUEUE_IDX);
        g_net.initialized = true;
        printf("virtio net: mmio base=%p irq=%d\n", g_net.mmio_base, g_net.irq);
        return true;
    }

    bool virtio_net_uses_irq(int irq)
    {
        return g_net.irq != 0 && irq == g_net.irq;
    }
#endif

#ifdef LOONGARCH
    bool virtio_net_init_pci(void)
    {
        // PCI 路径和 MMIO 路径做同一件事：找到设备、协商 feature、配置 RX/TX queue。
        uint64 pci_base = pci_device_probe(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID);
        if (pci_base == 0)
        {
            printf("virtio net: pci device not found\n");
            return false;
        }

        unsigned char bus = static_cast<unsigned char>((pci_base >> 16) & 0xff);
        unsigned char device = static_cast<unsigned char>((pci_base >> 11) & 0x1f);
        unsigned char function = static_cast<unsigned char>((pci_base >> 8) & 0x7);
        pci_device_init(pci_base, bus, device, function);

        if (virtio_pci_read_caps(&g_net.virtio_net_hw, pci_base, 0) != 0)
        {
            printf("virtio net: read pci caps failed\n");
            return false;
        }

        virtio_pci_set_status(&g_net.virtio_net_hw, 0);
        uint8 status = 0;
        // VirtIO 标准状态机：ACKNOWLEDGE -> DRIVER -> FEATURES_OK -> DRIVER_OK。
        status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
        virtio_pci_set_status(&g_net.virtio_net_hw, status);
        status |= VIRTIO_CONFIG_S_DRIVER;
        virtio_pci_set_status(&g_net.virtio_net_hw, status);

        uint64 features = negotiate_features(virtio_pci_get_device_features(&g_net.virtio_net_hw));
        // 写回驱动选择的 feature 集合。
        virtio_pci_set_driver_features(&g_net.virtio_net_hw, features);

        status |= VIRTIO_CONFIG_S_FEATURES_OK;
        virtio_pci_set_status(&g_net.virtio_net_hw, status);
        if ((virtio_pci_get_status(&g_net.virtio_net_hw) & VIRTIO_CONFIG_S_FEATURES_OK) == 0)
        {
            printf("virtio net: pci FEATURES_OK was rejected\n");
            return false;
        }

        if (virtio_pci_get_queue_enable(&g_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX) ||
            virtio_pci_get_queue_enable(&g_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX))
        {
            printf("virtio net: pci queues are already enabled\n");
            return false;
        }

        uint32 max_rx = virtio_pci_get_queue_size(&g_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX);
        uint32 max_tx = virtio_pci_get_queue_size(&g_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX);
        if (max_rx < NUM_NET_DESC || max_tx < NUM_NET_DESC)
        {
            printf("virtio net: pci queue too small, rx=%d tx=%d\n", max_rx, max_tx);
            return false;
        }

        // PCI modern 设备用 capability 暴露 queue 配置寄存器，这里分别配置 RX/TX queue 地址。
        virtio_pci_set_queue_size(&g_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX, NUM_NET_DESC);
        virtio_pci_set_queue_addr2(&g_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX,
                                   g_net.rx_desc, g_net.rx_avail, g_net.rx_used);
        virtio_pci_set_queue_enable(&g_net.virtio_net_hw, VIRTIO_NET_RX_QUEUE_IDX);

        virtio_pci_set_queue_size(&g_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX, NUM_NET_DESC);
        virtio_pci_set_queue_addr2(&g_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX,
                                   g_net.tx_desc, g_net.tx_avail, g_net.tx_used);
        virtio_pci_set_queue_enable(&g_net.virtio_net_hw, VIRTIO_NET_TX_QUEUE_IDX);

        if ((features & (1ULL << VIRTIO_NET_F_MAC)) != 0 && g_net.virtio_net_hw.device_cfg != nullptr)
        {
            volatile uint8 *config = reinterpret_cast<volatile uint8 *>(g_net.virtio_net_hw.device_cfg);
            for (int i = 0; i < ETH_ALEN; ++i)
            {
                g_net.mac_addr[i] = config[i];
            }
        }

        g_net.features = features;
        g_net.link_up = true;
        g_net.net_lock.acquire();
        // 和 MMIO 一样，先把所有 RX descriptor 投递给设备。
        post_all_rx_desc_locked();
        g_net.net_lock.release();

        // 最后 DRIVER_OK，设备开始工作。
        status |= VIRTIO_CONFIG_S_DRIVER_OK;
        virtio_pci_set_status(&g_net.virtio_net_hw, status);
        notify_queue(VIRTIO_NET_RX_QUEUE_IDX);
        g_net.initialized = true;
        return true;
    }

    int virtio_net_probe_pci(void)
    {
        return pci_device_probe(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID) == 0 ? -1 : 0;
    }
#endif

    void virtio_net_poll(void)
    {
        // poll 用于主动回收已经发送完成的 TX descriptor。
        // 接收线程每轮都会调用它，避免 TX descriptor 长期不释放。
        if (!g_net.initialized)
        {
            return;
        }
        g_net.net_lock.acquire();
        process_tx_used_locked();
        g_net.net_lock.release();
    }

    int virtio_net_send(const void *data, uint32 len)
    {
        // 发送一个完整以太网帧。上层 adapter 已经把 ONPS buf_list 合并成连续内存。
        if (!g_net.initialized || data == nullptr || len == 0 || len > ETH_FRAME_LEN)
        {
            return -1;
        }

        g_net.net_lock.acquire();
        // 发送前先回收已完成的 TX descriptor，尽量腾出空位。
        process_tx_used_locked();

        int desc_idx = -1;
        for (int i = 0; i < NUM_NET_DESC; ++i)
        {
            // 找一个空闲 TX descriptor。
            if (g_net.tx_free[i])
            {
                desc_idx = i;
                g_net.tx_free[i] = 0;
                break;
            }
        }
        if (desc_idx < 0)
        {
            g_net.net_lock.release();
            return -1;
        }

        int buf_idx = desc_idx;
        g_net.tx_buf_index[desc_idx] = static_cast<uint8>(buf_idx);
        g_net.tx_buffers[buf_idx].in_use = true;

        // VirtIO net 要求每个包前面有 virtio_net_hdr；当前不使用校验和/GSO，所以全清零。
        virtio_net_hdr *hdr = reinterpret_cast<virtio_net_hdr *>(g_net.tx_buffers[buf_idx].data);
        memset(hdr, 0, sizeof(*hdr));
        // 把以太网帧放在 virtio_net_hdr 后面。
        memcpy(g_net.tx_buffers[buf_idx].data + k_net_header_len, data, len);
        g_net.tx_buffers[buf_idx].len = len + k_net_header_len;

        // 填 descriptor，告诉设备这段物理内存在哪里、长度多少。
        g_net.tx_desc[desc_idx].addr = dma_addr(g_net.tx_buffers[buf_idx].data);
        g_net.tx_desc[desc_idx].len = g_net.tx_buffers[buf_idx].len;
        // flags=0 表示设备只读这段 buffer，用于发送。
        g_net.tx_desc[desc_idx].flags = 0;
        g_net.tx_desc[desc_idx].next = 0;

        // 把 descriptor index 放进 avail ring，表示“驱动有一个包要你发送”。
        uint16 idx = avail_idx(g_net.tx_avail);
        g_net.tx_avail[2 + (idx % NUM_NET_DESC)] = static_cast<uint16>(desc_idx);
        // 内存屏障保证 descriptor 内容先写完，再更新 avail idx 给设备看。
        __sync_synchronize();
        set_avail_idx(g_net.tx_avail, idx + 1);
        // 敲门通知设备处理 TX queue。
        notify_queue(VIRTIO_NET_TX_QUEUE_IDX);
        g_net.net_lock.release();
        return 0;
    }

    int virtio_net_recv(void *data, uint32 *len)
    {
        // 接收一个完整以太网帧。adapter 的接收线程循环调用这里。
        if (!g_net.initialized || data == nullptr || len == nullptr || *len == 0)
        {
            return -1;
        }

        g_net.net_lock.acquire();
        // 先同步设备写入的 used ring。
        __sync_synchronize();
        if (g_net.rx_used_idx == g_net.rx_used->idx)
        {
            // used_idx 追上设备 idx，表示当前没有新包。
            g_net.net_lock.release();
            return -1;
        }

        // 设备把已经写好数据的 RX descriptor 放到 used ring。
        VRingUsedElem *elem = &g_net.rx_used->ring[g_net.rx_used_idx % NUM_NET_DESC];
        uint32 desc_idx = elem->id;
        uint32 used_len = elem->len;
        ++g_net.rx_used_idx;

        if (desc_idx >= NUM_NET_DESC || used_len <= k_net_header_len)
        {
            // 异常包也要尽量回收 descriptor 并重新投递给 RX queue。
            if (desc_idx < NUM_NET_DESC)
            {
                g_net.rx_free[desc_idx] = 1;
                g_net.rx_buffers[g_net.rx_buf_index[desc_idx]].in_use = false;
                post_rx_desc_locked(desc_idx);
                notify_queue(VIRTIO_NET_RX_QUEUE_IDX);
            }
            g_net.net_lock.release();
            return -1;
        }

        uint8 buf_idx = g_net.rx_buf_index[desc_idx];
        // 设备写入的数据包含 virtio_net_hdr，真正的以太网帧在其后。
        uint32 data_len = used_len - k_net_header_len;
        uint32 copy_len = data_len > *len ? *len : data_len;
        memcpy(data, g_net.rx_buffers[buf_idx].data + k_net_header_len, copy_len);
        *len = copy_len;

        // 当前 descriptor 的数据已经复制给上层，可以重新作为 RX buffer 投递给设备。
        g_net.rx_buffers[buf_idx].in_use = false;
        g_net.rx_buffers[buf_idx].len = 0;
        g_net.rx_free[desc_idx] = 1;
        post_rx_desc_locked(desc_idx);
        // 通知设备 RX queue 又有可写 buffer。
        notify_queue(VIRTIO_NET_RX_QUEUE_IDX);
        g_net.net_lock.release();
        return 0;
    }

    void virtio_net_intr(void)
    {
#ifdef RISCV
        // 初始化窗口内也先确认中断，避免 DRIVER_OK 附近的早期中断反复触发。
        ack_interrupt();
#endif
        if (!g_net.initialized)
        {
            return;
        }
#ifdef LOONGARCH
        // LoongArch PCI 路径也需要确认设备中断。
        ack_interrupt();
#endif
        g_net.net_lock.acquire();
        // 当前中断处理只回收 TX 完成项；RX 由接收线程轮询 virtio_net_recv 拉取。
        process_tx_used_locked();
        g_net.net_lock.release();
        // 唤醒可能等待 TX descriptor 空闲的路径。
        proc::k_pm.wakeup(&g_net.tx_free[0]);
    }

    bool virtio_net_link_up(void)
    {
        return g_net.initialized && g_net.link_up;
    }

    void virtio_net_get_mac(uint8 mac[ETH_ALEN])
    {
        // 返回初始化时缓存的 MAC 地址。
        if (mac == nullptr)
        {
            return;
        }
        memcpy(mac, g_net.mac_addr, ETH_ALEN);
    }

    int virtio_net_test_send(void)
    {
        return -1;
    }

    int virtio_net_test_recv(void)
    {
        return -1;
    }

    void virtio_net_debug_status(void)
    {
        // 简单状态输出，方便确认网卡是否初始化、link 是否 up、ring 消费进度。
        printf("virtio net: ready=%d link=%d features=%lx rx_used=%d tx_used=%d\n",
               g_net.initialized ? 1 : 0,
               g_net.link_up ? 1 : 0,
               g_net.features,
               g_net.rx_used_idx,
               g_net.tx_used_idx);
    }
}
