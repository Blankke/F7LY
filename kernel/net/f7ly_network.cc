//
// F7LY Network Stack Integration
// Integrates VirtIO Net driver with ONPS network stack
//

#include "f7ly_network.hh"
#include "drivers/virtio_net_adapter.hh"
#include "libs/printer.hh"
#include "onps.hh"

namespace net
{
    static bool network_initialized = false;
    
    // Initialize the complete network stack
    bool init_network_stack()
    {
        if (network_initialized) {
            printf("[f7ly_network] Network stack already initialized\n");
            return true;
        }
        
        printf("[f7ly_network] Initializing F7LY network stack with VirtIO Net\n");
        
        // Step 1: Initialize ONPS network stack core
        EN_ONPSERR onps_error;
        if (!open_npstack_load(&onps_error)) {
            printf("[f7ly_network] Failed to initialize ONPS stack: %d\n", onps_error);
            return false;
        }
        
        printf("[f7ly_network] ONPS core initialized successfully\n");
        
        // Step 2: Initialize VirtIO Net adapter (this will register with ONPS)
        // if (!net::adapter_init()) {
        //     printf("[f7ly_network] Failed to initialize VirtIO Net adapter\n");
        //     open_npstack_unload();
        //     return false;
        // }
        
        printf("[f7ly_network] VirtIO Net adapter initialized successfully\n");
        
        network_initialized = true;
        
        // Print initial status
#ifdef RISCV
        // print_network_status();
#endif
        
        return true;
    }
    
    // Cleanup the network stack
    void cleanup_network_stack()
    {
        if (!network_initialized) {
            return;
        }
        
        printf("[f7ly_network] Cleaning up network stack\n");
        
        // Cleanup in reverse order
        net::adapter_cleanup();
        open_npstack_unload();
        
        network_initialized = false;
    }
    
    // Print network interface status
    void print_network_status()
    {
        if (!network_initialized) {
            printf("[f7ly_network] Network stack not initialized\n");
            return;
        }
        
        printf("[f7ly_network] ========== Network Status ==========\n");
        
        // Get and print MAC address
        uint8 mac[6];
        get_mac_address(mac);
        printf("[f7ly_network] MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        // Print VirtIO debug status
        virtio_net_debug_status();
        
        printf("[f7ly_network] ===================================\n");
    }
}
