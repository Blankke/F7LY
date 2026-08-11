#include "virtio_net.hh"

#include "libs/klib.hh"
#include "libs/printer.hh"
#include "libs/string.hh"
#include "hal/irq.hh"
#include "net/drivers/virtio_net_transport.hh"
#include "param.h"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"

namespace net
{
    namespace
    {
        constexpr uint32 k_net_header_len = sizeof(virtio_net_hdr);

        static virtio_net_device g_net;

        void virtio_net_irq_handler(void *)
        {
            virtio_net_intr();
        }

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

        void notify_queue(uint16 queue_index)
        {
            virtio_transport::notify_queue(queue_index);
        }

        void ack_interrupt()
        {
            virtio_transport::acknowledge_interrupt();
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

    } // namespace

    bool virtio_net_init(void)
    {
        // 底层 VirtIO 网卡初始化入口。上层 adapter_init 会先调用这里，
        // 成功后 ONPS 才能通过 virtio_net_send/recv 发送和接收以太网帧。
        if (g_net.initialized)
        {
            return true;
        }

        // 公共层只准备 split ring、descriptor 和包缓冲。设备发现、寄存器状态机
        // 以及 IRQ 来源全部由当前画像唯一选择的 transport 提供。
        prepare_common_state();

        g_net.net_lock.acquire();
        // 在 DRIVER_OK 之前投递 RX buffer，设备启动后立即拥有合法的 DMA 目标。
        post_all_rx_desc_locked();
        g_net.net_lock.release();

        const virtio_transport::PrepareArgs transport_args{
            .receive_queue = {
                .descriptor = g_net.rx_desc,
                .available = g_net.rx_avail,
                .used = g_net.rx_used,
                .descriptor_dma = dma_addr(g_net.rx_desc),
                .available_dma = dma_addr(g_net.rx_avail),
                .used_dma = dma_addr(g_net.rx_used),
            },
            .transmit_queue = {
                .descriptor = g_net.tx_desc,
                .available = g_net.tx_avail,
                .used = g_net.tx_used,
                .descriptor_dma = dma_addr(g_net.tx_desc),
                .available_dma = dma_addr(g_net.tx_avail),
                .used_dma = dma_addr(g_net.tx_used),
            },
            .queue_size = NUM_NET_DESC,
            // 当前公共收发逻辑只理解普通以太网帧，因此只接受设备 MAC，
            // 不启用 checksum、GSO 或 mergeable buffer 等额外语义。
            .driver_features = 1ULL << virtio_transport::k_feature_mac,
            .mac_address = g_net.mac_addr,
            .mac_address_length = ETH_ALEN,
        };
        uint64 negotiated_features = 0;
        if (!virtio_transport::prepare(transport_args, &negotiated_features))
        {
            printf("virtio net: device initialization failed\n");
            return false;
        }

        const uint32 interrupt_source = virtio_transport::interrupt_source();
        if (!hal::irq::register_handler(
                interrupt_source, virtio_net_irq_handler, nullptr, "virtio-net"))
        {
            panic("failed to register virtio net IRQ source %u", interrupt_source);
        }

        g_net.features = negotiated_features;
        g_net.link_up = true;
        virtio_transport::activate();
        g_net.initialized = true;
        notify_queue(virtio_transport::k_receive_queue_index);

        printf("virtio net: initialized, mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               g_net.mac_addr[0], g_net.mac_addr[1], g_net.mac_addr[2],
               g_net.mac_addr[3], g_net.mac_addr[4], g_net.mac_addr[5]);
        return true;
    }

    bool virtio_net_is_initialized(void)
    {
        return g_net.initialized;
    }

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
        notify_queue(virtio_transport::k_transmit_queue_index);
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
                notify_queue(virtio_transport::k_receive_queue_index);
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
        notify_queue(virtio_transport::k_receive_queue_index);
        g_net.net_lock.release();
        return 0;
    }

    void virtio_net_intr(void)
    {
        // 无论公共状态是否已经发布，都先确认 transport 中断，避免 DRIVER_OK
        // 附近的早期中断在共享/电平触发线路上反复进入。
        ack_interrupt();
        if (!g_net.initialized)
        {
            return;
        }
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
