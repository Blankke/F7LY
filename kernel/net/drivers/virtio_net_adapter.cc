//
// VirtIO Net to ONPS Adapter Implementation
// This adapter bridges virtio_net driver with onps network stack
//

#include "virtio_net_adapter.hh"
#include "virtio_net.hh"
#include "platform.hh"
#include "mem/memlayout.hh"
#include "libs/string.hh"
#include "libs/klib.hh"
#include "libs/printer.hh"
#include "proc_manager.hh"
#include "sys/syscall_defs.hh"

// ONPS network stack includes
#include "onps.hh"
#include "netif/netif.hh"
#include "ethernet/ethernet.hh"
#include "mmu/buf_list.hh"
#include "port/os_adapter.hh"

extern "C" void kernel_thread_wrapper();

namespace net
{
    // Forward declarations
    static void start_recv_thread_wrapper(void *param);
    extern void virtio_recv_thread(void *param);
    
    // Static variables for adapter state
    static bool adapter_initialized = false;
    static PST_NETIF onps_netif = nullptr;
    static bool recv_thread_running = false;
    
    // 收发路径可能并发执行，不能复用同一块临时帧缓存。
    static uint8 tx_packet_buffer[ETH_FRAME_LEN];
    static uint8 rx_packet_buffer[ETH_FRAME_LEN];

    // Initialize the adapter
    bool adapter_init()
    {
        if (adapter_initialized) {
            printf("[virtio_net_adapter] Already initialized\n");
            return true;
        }
        
        printf("[virtio_net_adapter] Initializing VirtIO Net to ONPS adapter\n");
        
        // Initialize virtio net driver first
        if (!net::virtio_net_init()) {
            printf("[virtio_net_adapter] VirtIO Net driver init failed\n");
            return false;
        }
        
        // Get MAC address from virtio net
        uint8 mac_addr[ETH_ALEN];
        net::virtio_net_get_mac(mac_addr);
        
        printf("[virtio_net_adapter] VirtIO MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac_addr[0], mac_addr[1], mac_addr[2], 
               mac_addr[3], mac_addr[4], mac_addr[5]);
        
        // Setup IPv4 configuration for the interface
        ST_IPV4 ipv4_config;
        memset(&ipv4_config, 0, sizeof(ipv4_config));
        
        // QEMU user-mode 网络默认拓扑：guest=10.0.2.15，gateway/DNS=10.0.2.2/10.0.2.3。
        ipv4_config.unAddr = inet_addr("10.0.2.15");
        ipv4_config.unSubnetMask = inet_addr("255.255.255.0");
        ipv4_config.unGateway = inet_addr("10.0.2.2");
        ipv4_config.unPrimaryDNS = inet_addr("10.0.2.3");
        ipv4_config.unBroadcast = inet_addr("10.0.2.255");

        // Register ethernet interface with onps
        EN_ONPSERR error;
        onps_netif = ethernet_add("virtio0",              // Interface name
                                  mac_addr,               // MAC address
                                  &ipv4_config,          // IPv4 config
                                  virtio_emac_send,      // Send function
                                  start_recv_thread_wrapper, // Receive thread starter
                                  &onps_netif,           // Output netif pointer
                                  &error);               // Error output
        
        if (!onps_netif) {
            printf("[virtio_net_adapter] Failed to add ethernet interface to onps: %d\n", error);
            return false;
        }
        
        printf("[virtio_net_adapter] Successfully registered interface with onps\n");
        
        adapter_initialized = true;
        return true;
    }
    
    // Cleanup function
    void adapter_cleanup()
    {
        if (!adapter_initialized) {
            return;
        }
        
        printf("[virtio_net_adapter] Cleaning up adapter\n");
        
        // Stop receive thread
        stop_recv_thread();
        
        // Remove interface from onps (if function exists)
        if (onps_netif) {
            // netif_del_ext(onps_netif); // Uncomment if this function exists
            onps_netif = nullptr;
        }
        
        adapter_initialized = false;
    }
    
    // Implementation of PFUN_EMAC_SEND for onps
    // This function receives data from onps and sends it via virtio net
    int virtio_emac_send(short buf_list_head, unsigned char *error)
    {
        if (!adapter_initialized) {
            if (error) *error = 1; // Generic error
            return -1;
        }
        
        // Get total length of the packet
        UINT total_len = buf_list_get_len(buf_list_head);
        if (total_len <= 0 || total_len > ETH_FRAME_LEN) {
            printf("[virtio_net_adapter] Invalid packet length: %d\n", total_len);
            if (error) *error = 1;
            return -1;
        }
        
        // Merge buffer list into contiguous packet
        buf_list_merge_packet(buf_list_head, tx_packet_buffer);
        
        // Send via virtio net
        int result = net::virtio_net_send(tx_packet_buffer, total_len);
        
        if (result != 0) {
            printf("[virtio_net_adapter] virtio_net_send failed: %d\n", result);
            if (error) *error = 1;
            return -1;
        }
        
        if (error) *error = 0; // Success
        return total_len;
    }
    
    // Background thread for receiving packets
    void virtio_recv_thread(void *param)
    {
        PST_NETIF netif = static_cast<PST_NETIF>(param);
        if (netif == nullptr) {
            printf("[virtio_net_adapter] Receive thread got null netif\n");
            return;
        }

        recv_thread_running = true;
        printf("[virtio_net_adapter] Receive thread started\n");
        while (recv_thread_running) {
            bool received_any = false;
            for (;;) {
                uint32 packet_len = sizeof(rx_packet_buffer);
                int result = net::virtio_net_recv(rx_packet_buffer, &packet_len);
                if (result != 0 || packet_len == 0) {
                    break;
                }

                received_any = true;
                ethernet_ii_recv(netif, rx_packet_buffer, static_cast<INT>(packet_len));
            }

            net::virtio_net_poll();
            if (!received_any) {
                os_sleep_ms(1);
            }
        }
        printf("[virtio_net_adapter] Receive thread stopped\n");
    }
    
    // Start the receive thread  
    void start_recv_thread()
    {
        if (recv_thread_running) {
            return; // Already running
        }
        
        printf("[virtio_net_adapter] Starting receive thread\n");
        recv_thread_running = true;
        
        // For now, we'll just mark as running
        // The actual thread will be created by the wrapper function
    }
    
    // Wrapper function for ethernet_add interface
    void start_recv_thread_wrapper(void *param) 
    {
        printf("[virtio_net_adapter] Receive thread wrapper called\n");
        PST_NETIF *netif_slot = static_cast<PST_NETIF *>(param);
        PST_NETIF netif = netif_slot != nullptr ? *netif_slot : nullptr;
        if (netif == nullptr) {
            printf("[virtio_net_adapter] Receive thread wrapper got null netif\n");
            return;
        }
        onps_netif = netif;
        if (recv_thread_running) {
            return;
        }

        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr) {
            printf("[virtio_net_adapter] No current process for receive thread\n");
            return;
        }

        uint64 flags = syscall::CLONE_VM | syscall::CLONE_FILES |
                       syscall::CLONE_SIGHAND | syscall::CLONE_THREAD;
        proc::Pcb *thread_pcb = proc::k_pm.fork(current_proc, flags, 0, 0, false);
        if (thread_pcb == nullptr) {
            printf("[virtio_net_adapter] Failed to fork receive thread\n");
            return;
        }

        thread_pcb->_context.ra = reinterpret_cast<uint64>(kernel_thread_wrapper);
        thread_pcb->_context.s0 = reinterpret_cast<uint64>(virtio_recv_thread);
        thread_pcb->_context.s1 = reinterpret_cast<uint64>(netif);
        strncpy(thread_pcb->_name, "virtio-net-rx", sizeof(thread_pcb->_name) - 1);
        thread_pcb->_name[sizeof(thread_pcb->_name) - 1] = '\0';

        recv_thread_running = true;
        thread_pcb->_lock.release();
    }
    
    // Stop the receive thread
    void stop_recv_thread()
    {
        if (!recv_thread_running) {
            return;
        }
        
        printf("[virtio_net_adapter] Stopping receive thread\n");
        recv_thread_running = false;
        
        // Give the thread time to exit
        os_sleep_ms(100);
        
    }
    
    // Get MAC address for onps registration
    void get_mac_address(unsigned char mac[6])
    {
        net::virtio_net_get_mac(mac);
    }
}
