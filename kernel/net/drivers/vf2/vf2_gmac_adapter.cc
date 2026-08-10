/* SPDX-License-Identifier: GPL-3.0-only */
#include "vf2_gmac_adapter.hh"

#include "libs/klib.hh"
#include "libs/printer.hh"
#include "onps.hh"
#include "netif/netif.hh"
#include "ethernet/ethernet.hh"
#include "port/os_adapter.hh"
#include "proc_manager.hh"
#include "sys/syscall_defs.hh"
#include "vf2_gmac.hh"
#include "vf2_gmac_platform.hh"

extern "C" void kernel_thread_wrapper();

namespace net
{
    namespace
    {
        static bool adapter_initialized = false;
        static PST_NETIF onps_netif = nullptr;
        static uint8 tx_buffer[vf2::k_max_frame_size];
        static uint8 rx_buffer[vf2::k_max_frame_size];
        static volatile bool recv_running = false;

        void start_recv_thread_wrapper(void *param);
    }

    bool vf2_adapter_init()
    {
#ifndef VISIONFIVE2
        return false;
#else
        if (adapter_initialized)
            return true;

        printf("[vf2_adapter] Initializing VF2 GMAC to ONPS adapter\n");
        if (!vf2_gmac_init())
        {
            printf("[vf2_adapter] VF2 GMAC initialization failed\n");
            return false;
        }

        uint8 mac_addr[6] = {};
        vf2_gmac_get_mac(mac_addr);
        printf("[vf2_adapter] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

        // VF2 首版使用板端静态地址，便于在同一局域网直接进行 ARP/ping 验证。
        ST_IPV4 ipv4_config;
        memset(&ipv4_config, 0, sizeof(ipv4_config));
        ipv4_config.unAddr = inet_addr("192.168.1.100");
        ipv4_config.unSubnetMask = inet_addr("255.255.255.0");
        ipv4_config.unGateway = inet_addr("192.168.1.1");
        ipv4_config.unPrimaryDNS = inet_addr("8.8.8.8");
        ipv4_config.unBroadcast = inet_addr("192.168.1.255");

        EN_ONPSERR error;
        // NETIF_NAME_LEN 为 7，接口名必须控制在 6 个字符以内。
        onps_netif = ethernet_add("vf2", mac_addr, &ipv4_config,
                                  vf2_emac_send, start_recv_thread_wrapper,
                                  &onps_netif, &error);
        if (onps_netif == nullptr)
        {
            printf("[vf2_adapter] Failed to register interface with ONPS: %d\n", error);
            return false;
        }

        adapter_initialized = true;
        printf("[vf2_adapter] Registered vf2 interface at 192.168.1.100\n");
        return true;
#endif
    }

    void vf2_adapter_cleanup()
    {
        if (!adapter_initialized)
            return;

        recv_running = false;
        os_sleep_ms(100);
        if (onps_netif != nullptr)
            ethernet_del(&onps_netif);
        adapter_initialized = false;
    }

    int vf2_emac_send(short buf_list_head, unsigned char *error)
    {
        if (!vf2_gmac_is_initialized())
        {
            if (error) *error = 1;
            return -1;
        }

        uint32 len = buf_list_get_len(buf_list_head);
        if (len == 0 || len > vf2::k_max_frame_size)
        {
            if (error) *error = 1;
            return -1;
        }
        buf_list_merge_packet(buf_list_head, tx_buffer);
        int ret = vf2_gmac_send(tx_buffer, len);
        if (error) *error = ret < 0 ? 1 : 0;
        return ret < 0 ? -1 : static_cast<int>(len);
    }

    void vf2_recv_thread(void *param)
    {
        PST_NETIF netif = static_cast<PST_NETIF>(param);
        if (netif == nullptr)
            return;

        recv_running = true;
        printf("[vf2_adapter] Receive thread started\n");
        while (recv_running)
        {
            bool received = false;
            for (;;)
            {
                uint32 len = sizeof(rx_buffer);
                int ret = vf2_gmac_recv(rx_buffer, &len);
                if (ret < 0 || len == 0)
                    break;
                ethernet_ii_recv(netif, rx_buffer, static_cast<INT>(len));
                received = true;
            }
            vf2_gmac_poll();
            if (!received)
                os_sleep_ms(1);
        }
        printf("[vf2_adapter] Receive thread stopped\n");
    }

    namespace
    {
        void start_recv_thread_wrapper(void *param)
        {
            PST_NETIF *netif_slot = static_cast<PST_NETIF *>(param);
            PST_NETIF netif = netif_slot != nullptr ? *netif_slot : nullptr;
            if (netif == nullptr)
            {
                printf("[vf2_adapter] Receive thread callback got null netif\n");
                return;
            }
            if (recv_running)
                return;

            proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
            if (current_proc == nullptr)
            {
                printf("[vf2_adapter] No current process for receive thread\n");
                return;
            }

            uint64 flags = syscall::CLONE_VM | syscall::CLONE_FILES |
                           syscall::CLONE_SIGHAND | syscall::CLONE_THREAD;
            proc::Pcb *thread_pcb = proc::k_pm.fork(current_proc, flags, 0, 0, false);
            if (thread_pcb == nullptr)
            {
                printf("[vf2_adapter] Failed to create receive thread\n");
                return;
            }

            thread_pcb->_context.ra = reinterpret_cast<uint64>(kernel_thread_wrapper);
            thread_pcb->_context.s0 = reinterpret_cast<uint64>(vf2_recv_thread);
            thread_pcb->_context.s1 = reinterpret_cast<uint64>(netif);
            strncpy(thread_pcb->_name, "vf2-gmac-rx", sizeof(thread_pcb->_name) - 1);
            thread_pcb->_name[sizeof(thread_pcb->_name) - 1] = '\0';
            recv_running = true;
            thread_pcb->_lock.release();
        }
    }

    void vf2_get_mac_address(unsigned char mac[6])
    {
        if (mac != nullptr)
            vf2_gmac_get_mac(mac);
    }

    void vf2_adapter_debug_status()
    {
        vf2_gmac_debug_status();
    }
}
