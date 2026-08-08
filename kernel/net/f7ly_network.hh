// F7LY 网络栈集成：通过板级网卡门面连接 ONPS。

#pragma once

#include "types.hh"

namespace net
{
    // 分阶段初始化板级网卡、ONPS 核心和以太网接口。
    bool init_network_stack();

    // 当前协议栈和网卡适配器是否已经成功初始化。
    bool is_network_stack_ready();
    
    // Get network interface status
    void print_network_status();
}
