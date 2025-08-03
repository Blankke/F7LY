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

// ONPS network stack includes
#include "onps.hh"
#include "netif/netif.hh"
#include "ethernet/ethernet.hh"
#include "mmu/buf_list.hh"
#include "port/os_adapter.hh"

namespace virtio_net_adapter
{
    // Forward declarations
    static void start_recv_thread_wrapper(void *param);
    
    // Static variables for adapter state
    static bool adapter_initialized = false;
    static PST_NETIF onps_netif = nullptr;
    static bool recv_thread_running = false;
    static uint64 recv_thread_id = 0;
    
    // Buffer for packet processing
    static uint8 packet_buffer[ETH_FRAME_LEN];
    
    // Initialize the adapter
    bool adapter_init()
    {
        if (adapter_initialized) {
            printf("[virtio_net_adapter] Already initialized\n");
            return true;
        }
        
        printf("[virtio_net_adapter] Initializing VirtIO Net to ONPS adapter\n");
        
        // Initialize virtio net driver first
        virtio_net::virtio_net_init();
        
        // Get MAC address from virtio net
        uint8 mac_addr[ETH_ALEN];
        virtio_net::virtio_net_get_mac(mac_addr);
        
        printf("[virtio_net_adapter] VirtIO MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac_addr[0], mac_addr[1], mac_addr[2], 
               mac_addr[3], mac_addr[4], mac_addr[5]);
        
        // Setup IPv4 configuration for the interface
        ST_IPV4 ipv4_config;
        memset(&ipv4_config, 0, sizeof(ipv4_config));
        
        // Set default IP configuration (you can modify these)
        ipv4_config.unAddr = htonl(0xC0A80102);         // 192.168.1.2
        ipv4_config.unSubnetMask = htonl(0xFFFFFF00);   // 255.255.255.0
        ipv4_config.unGateway = htonl(0xC0A80101);      // 192.168.1.1
        ipv4_config.unPrimaryDNS = htonl(0x08080808);   // 8.8.8.8
        ipv4_config.unBroadcast = htonl(0xC0A801FF);    // 192.168.1.255
        
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
        buf_list_merge_packet(buf_list_head, packet_buffer);
        
        // Send via virtio net
        int result = virtio_net::virtio_net_send(packet_buffer, total_len);
        
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
        printf("[virtio_net_adapter] Receive thread started\n");
        
        PST_NETIF netif = (PST_NETIF)param;
        if (!netif) {
            printf("[virtio_net_adapter] Invalid netif parameter\n");
            return;
        }
        
        recv_thread_running = true;
        
        while (recv_thread_running) {
            uint32 packet_len = sizeof(packet_buffer);
            
            // Try to receive a packet from virtio net
            int result = virtio_net::virtio_net_recv(packet_buffer, &packet_len);
            
            if (result == 0 && packet_len > 0) {
                // Successfully received a packet
                printf("[virtio_net_adapter] Received packet of length %d\n", packet_len);
                
                // Forward to onps ethernet layer
                ethernet_ii_recv(netif, packet_buffer, packet_len);
            } else {
                // No packet available, sleep briefly to avoid busy waiting
                os_sleep_ms(1); // Sleep for 1ms
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
        PST_NETIF netif = (PST_NETIF)param;
        if (netif) {
            onps_netif = netif; // Update the netif pointer
        }
        start_recv_thread();
        
        // Start the actual receive loop
        virtio_recv_thread(netif);
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
        
        recv_thread_id = 0;
    }
    
    // Get MAC address for onps registration
    void get_mac_address(unsigned char mac[6])
    {
        virtio_net::virtio_net_get_mac(mac);
    }
}
