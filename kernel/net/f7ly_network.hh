//
// F7LY Network Stack Integration
// Integrates VirtIO Net driver with ONPS network stack
//

#pragma once

#include "types.hh"

namespace net
{
    // Initialize the complete network stack with VirtIO Net support
    bool init_network_stack();
    
    // Cleanup the network stack
    void cleanup_network_stack();
    
    // Get network interface status
    void print_network_status();
}
