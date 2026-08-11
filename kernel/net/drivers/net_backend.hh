#pragma once

#include "platform_net_device.hh"

// 当前构建目标必须且只能提供一份本接口实现。
//
// 这一层只描述“以太网设备能做什么”，不携带 IP、网关等网络层配置。
// 公共网卡门面因此不需要知道底层使用 VirtIO、GMAC 或未来的新驱动。
namespace net::backend
{
bool initialize();
int send(const void *data, uint32 length);
int receive(void *data, uint32 *length);
void poll();
void get_mac(uint8 mac[platform_device::k_mac_address_length]);
const char *name();
void debug_status();
} // namespace net::backend
