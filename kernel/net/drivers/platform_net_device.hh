#pragma once

#include "types.hh"

namespace net::platform_device
{
constexpr uint32 k_mac_address_length = 6;
constexpr uint32 k_max_ethernet_frame = 1514;
// LS2K1000 GMAC 的接收描述符按 2KiB 缓冲工作，长度字段可能包含硬件尾部信息。
// 上层按实际描述符长度处理，不能用发送侧的 1514 字节上限截断 DMA 数据。
constexpr uint32 k_receive_buffer_size = 2048;

bool initialize();
int send(const void *data, uint32 length);
int receive(void *data, uint32 *length);
void poll();
void get_mac(uint8 mac[k_mac_address_length]);
const char *name();
void debug_status();
} // namespace net::platform_device
