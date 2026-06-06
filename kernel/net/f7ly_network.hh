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

    // 当前协议栈和网卡适配器是否已经成功初始化。
    bool is_network_stack_ready();
    
    // Cleanup the network stack
    void cleanup_network_stack();
    
    // Get network interface status
    void print_network_status();
}
