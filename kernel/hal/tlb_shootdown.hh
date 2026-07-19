#pragma once

#include "types.hh"

namespace hal::tlb
{
// 在本 CPU 的页表/中断入口安装完成后调用，并且必须早于发布 online。
void initialize_current_cpu();

// size==0 表示失效全部地址；range 接口允许架构后端保守扩大失效范围。
void flush_local_range(uint64 start, uint64 size);
void flush_range_all_cpus(uint64 start, uint64 size);
void flush_all_cpus();

// 自旋锁等待期间本地中断通常关闭。架构后端可在这里轮询并处理仅与 TLB
// 相关的无锁 IPI，避免两个 CPU 分别持锁/等待 shootdown 时互相饿死。
void poll_pending();

#ifdef LOONGARCH
// 由 LoongArch CPU-local IPI 中断路径调用；返回是否消费了 IPI 状态。
bool handle_ipi();
#endif
}
