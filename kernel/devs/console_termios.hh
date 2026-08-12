#pragma once

#include "syscall_abi.hh"

namespace dev
{
    namespace abi = syscall::abi;

    struct ConsoleReadSettings
    {
        bool canonical = true;
        unsigned char min_bytes = 1;
        unsigned char timeout_deciseconds = 0;
    };

    class ConsoleTermiosController
    {
    public:
        // 构造阶段只保存用户可见 termios 默认值，不访问其他全局对象。
        // TCGETS/TCSETS 与 TCGETA/TCSETA 共用同一个控制器，避免两套状态漂移。
        ConsoleTermiosController();

        // Console 自身的锁和状态建立后显式调用，消除跨翻译单元的全局构造顺序依赖。
        void initialize_line_discipline();
        abi::KernelTermios snapshot() const;
        abi::KernelTermio legacy_snapshot() const;
        ConsoleReadSettings read_settings() const;
        bool map_output_newline() const;
        void apply(const abi::KernelTermios &termios);
        void apply_legacy(const abi::KernelTermio &termio);

    private:
        static abi::KernelTermios make_default_termios();
        static void sync_to_line_discipline(const abi::KernelTermios &termios);
        static abi::KernelTermio to_legacy(const abi::KernelTermios &termios);
        static void merge_legacy(abi::KernelTermios &termios,
                                 const abi::KernelTermio &legacy);

        abi::KernelTermios _termios;
    };

    extern ConsoleTermiosController k_console_termios;
}
