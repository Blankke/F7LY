#pragma once

#include "hal/arch.hh"
#include "param.h"
#include "types.hh"

// Kernel stack configuration
// 内核栈配置常量
// SMP 下时钟中断、调度切换和深层 VFS/syscall 调用会叠加在同一任务的内核栈上。
// 8KiB 在 -smp 8 压力中已触及 guard page；统一扩为 16KiB，给两架构保留足够
// 的 trap frame 与 C++ 调用深度，同时仍只额外占用约 4MiB 的常驻物理页。
#define KSTACK_PAGES 4          // 内核栈使用的页面数 (16KB)
#define KSTACK_SIZE (KSTACK_PAGES * PGSIZE)  // 内核栈总大小
#define KSTACK_GUARD_PAGES 1    // guard page 数量
#define KSTACK_TOTAL_PAGES (KSTACK_PAGES + KSTACK_GUARD_PAGES)  // 总分配页面数

#ifdef RISCV
// 内核物理加载地址属于平台画像：QEMU 与 VisionFive2 并不相同。
// 需要判断内核地址下界的代码统一读取链接符号 kernel_start；本文件只保留
// 与具体板无关的高端虚拟地址布局。

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE ((MAXVA>>1) - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   每个线程一页 USER_TRAPFRAME(gid)（供 trampoline 保存寄存器）
//   TRAMPOLINE (the same page as in the kernel)
#define SIG_TRAMPOLINE   (TRAMPOLINE - PGSIZE)
#define TRAPFRAME (SIG_TRAMPOLINE - PGSIZE)
// 内核栈和用户页表在切换时共用 ASID=0；即使通常会刷新 TLB，也不能让
// CLONE_VM 线程的 trapframe 与任一内核栈使用相同虚拟页。否则在 refill/
// 切表窗口可能把内核栈翻译错误复用于 uservec，直接损坏用户寄存器现场。
// 因此先完整预留 NPROC 个内核栈（含 guard）所在的高地址区域，再把每线程
// trapframe 放在它的下方。该区只消耗虚拟地址，不增加物理页占用。
#define KSTACK_REGION_BOTTOM (TRAPFRAME - ((NPROC * KSTACK_TOTAL_PAGES - KSTACK_GUARD_PAGES) * PGSIZE))
#define USER_TRAPFRAME_TOP KSTACK_REGION_BOTTOM
#define USER_TRAPFRAME_BASE (USER_TRAPFRAME_TOP - (NPROC * PGSIZE))
#define USER_TRAPFRAME(global_id) (USER_TRAPFRAME_TOP - (((global_id) + 1) * PGSIZE))
#define USER_MEMORY_TOP USER_TRAPFRAME_BASE
#define KSTACK(p) (TRAPFRAME - (((p)+1)* KSTACK_TOTAL_PAGES - KSTACK_GUARD_PAGES )*PGSIZE) // 内核栈栈底
#elif defined(LOONGARCH)
#define TRAPFRAME ((MAXVA>>1 )- PGSIZE) //64TB

// map kernel stacks beneath the trampframe,
// each surrounded by invalid guard pages.
#define SIG_TRAMPOLINE   (TRAPFRAME - PGSIZE)
// LoongArch 的 SIG_TRAMPOLINE 位于 TRAPFRAME 下方。用户页表使用独立 ASID，
// 但 trap 入口在切换到内核 ASID/PGDL 前仍需访问当前线程 trapframe；继续让
// trapframe 避开内核栈虚拟区，可保持这个极窄窗口的地址契约清晰且可审计。
// 先跨过整个 NPROC 内核栈区域，再向下安排每线程 trapframe。
#define KSTACK_REGION_BOTTOM (SIG_TRAMPOLINE - ((NPROC * KSTACK_TOTAL_PAGES - KSTACK_GUARD_PAGES) * PGSIZE))
#define USER_TRAPFRAME_TOP KSTACK_REGION_BOTTOM
#define USER_TRAPFRAME_BASE (USER_TRAPFRAME_TOP - (NPROC * PGSIZE))
#define USER_TRAPFRAME(global_id) (USER_TRAPFRAME_TOP - (((global_id) + 1) * PGSIZE))
#define USER_MEMORY_TOP USER_TRAPFRAME_BASE
#define KSTACK(p) (SIG_TRAMPOLINE - (((p)+1)* KSTACK_TOTAL_PAGES - KSTACK_GUARD_PAGES )*PGSIZE)
#endif

// 两种架构使用同一套内核堆/共享内存容量策略，虚拟起点仅随各自 MAXVA
// 改变。统一定义可避免架构头继续保存两份相同常量。
enum vml : uint64
{
    vm_kernel_heap_start = MAXVA >> 1,
    vm_kernel_heap_size = _1M * 256,
};

inline constexpr uint64 SHM_SIZE = _1M * 64;
