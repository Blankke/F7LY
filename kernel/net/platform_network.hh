#pragma once

namespace net
{
    // 初始化当前板级后端。没有受支持网卡时返回 false，但不影响 AF_UNIX/loopback。
    bool init_platform_network();
}
