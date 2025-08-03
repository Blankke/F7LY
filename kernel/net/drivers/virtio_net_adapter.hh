//
// VirtIO Net to ONPS Adapter Interface
// This adapter bridges virtio_net driver with onps network stack
//

#pragma once

#include "types.hh"
#include "platform.hh"
#include "virtio_net.hh"

namespace virtio_net_adapter
{
    // Adapter initialization - sets up virtio net and registers with onps
    bool adapter_init();
    
    // Cleanup function
    void adapter_cleanup();
    
    // Function to be called by onps ethernet layer for sending packets
    // This implements PFUN_EMAC_SEND interface expected by onps
    int virtio_emac_send(short buf_list_head, unsigned char *error);
    
    // Background thread function for receiving packets from virtio and 
    // forwarding them to onps ethernet layer
    void virtio_recv_thread(void *param);
    
    // Helper function to start the receive thread
    void start_recv_thread();
    
    // Stop the receive thread
    void stop_recv_thread();
    
    // Get virtio net MAC address for onps registration
    void get_mac_address(unsigned char mac[6]);
}
