//
// 通用以太网设备到 ONPS 的协议适配接口。
// 本层不拥有 VirtIO/GMAC，也不负责平台 IP 配置的来源。
//

#pragma once

#include "types.hh"
#include "hal/arch.hh"

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
