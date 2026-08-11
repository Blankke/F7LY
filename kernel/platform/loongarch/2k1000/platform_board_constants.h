#pragma once

// 该头同时供 C++ 与预处理后的汇编使用，只放不带类型/命名空间的板级常量。
// UART 地址必须只有这一份权威定义，避免早期串口和运行时串口悄悄漂移。
#define LS2K1000_UART_PHYSICAL 0x1fe20000
#define LS2K1000_UART_UNCACHED_DMW \
    (0x8000000000000000 + LS2K1000_UART_PHYSICAL)
