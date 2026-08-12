#pragma once

#ifdef LOONGARCH
#include "types.hh"

namespace proc
{
class Pcb;
}

namespace loongarch::unaligned
{
enum class Result
{
    Complete,
    MemoryFault,
    UnsupportedInstruction,
};

// 模拟用户态普通/浮点 load、store 的非对齐访存。成功时已经更新目标寄存器
// 和 ERA；失败时调用方仍按同步地址错误投递 SIGSEGV。
Result emulate_user_access(proc::Pcb &process, uint64 address, uint32 instruction);
}
#endif
