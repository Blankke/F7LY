#pragma once

#include "types.hh"

namespace platform::console_backend
{
    // 极早期诊断接口：实现不得依赖 BSS 初值、全局构造、锁或软件队列。
    // 架构启动代码只在当前画像开启 verbose bring-up 或遇到致命错误时调用。
    void early_write(const char *message);

    /**
     * 平台控制台可能没有外部中断，例如只通过 SBI 轮询输入的开发板。
     * 这里显式表达“无中断”，避免用 0 等可能合法的硬件源号当哨兵。
     */
    struct InterruptSource
    {
        bool present;
        uint32 source;
    };

    /**
     * CharDevice 需要的最小线路状态。raw/new_errors 只用于低频诊断，
     * 通用 UART 管理层不解释具体控制器的寄存器位。
     */
    struct LineStatus
    {
        bool transmitter_empty;
        uint8 raw;
        uint8 new_errors;
    };

    // 每个平台目录恰好提供一份实现；启动期只允许调用一次 initialize()。
    bool initialize();

    // 非阻塞地搬运一个字节。返回 false 表示当前没有输入/发送器仍忙。
    bool try_getc(uint8 &character);
    bool try_putc(uint8 character);

    // 软件发送队列由 UartManager 所有，后端只负责硬件 TX-ready 通知开关。
    void set_transmit_interrupt_enabled(bool enabled);

    // 丢弃传输端尚未消费的字节；不改变波特率和线路格式。
    void flush_input();
    void flush_output();

    LineStatus line_status();
    InterruptSource interrupt_source();
    const char *name();
} // namespace platform::console_backend
