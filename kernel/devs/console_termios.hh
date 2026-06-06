#pragma once

#include "syscall_abi.hh"

namespace dev
{
    namespace abi = syscall::abi;

    class ConsoleTermiosController
    {
    public:
        // 保存用户可见 termios 状态，并把它同步到 console line discipline。
        // TCGETS/TCSETS 与 TCGETA/TCSETA 共用同一个控制器，避免两套状态漂移。
        ConsoleTermiosController();

        abi::KernelTermios snapshot() const;
        abi::KernelTermio legacy_snapshot() const;
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
