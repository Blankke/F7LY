//
// 板级网卡到 ONPS 的适配接口。文件名为历史遗留，接口本身不依赖 VirtIO。
//

#pragma once

#include "types.hh"
#include "platform.hh"

namespace net
{
    // 初始化当前板级网卡并注册到 ONPS。
    bool adapter_init();
    
    // Function to be called by onps ethernet layer for sending packets
    // This implements PFUN_EMAC_SEND interface expected by onps
    int platform_emac_send(short buf_list_head, unsigned char *error);
    
    // 从当前板级网卡收包并转交 ONPS 以太网层的后台线程。
    void platform_recv_thread(void *param);
    
    // 获取当前板级网卡 MAC。
    void get_mac_address(unsigned char mac[6]);
}
