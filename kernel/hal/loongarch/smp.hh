#pragma once

#include "types.hh"

namespace loongarch::smp
{
    // 通过 QEMU LoongArch virt 的 IOCSR mailbox/IPI 协议唤醒停在 slave
    // boot ROM 中的次核。entry 可以是内核链接虚拟地址，函数会转换成次核
    // 在直接地址模式下可执行的缓存别名。
    void start_secondary_cpu(uint64 cpu_id, uint64 entry, uint64 argument);
}
