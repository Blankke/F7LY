#pragma once

#include "param.h"

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
// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel
//这里宏定义的都是物理地址，我们实际用到的都是虚拟地址，会在vmm里面进行映射
// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// RISC-V virtio-mmio 传输页，QEMU virt 机器最多提供 8 个槽位。
// 设备顺序由 QEMU 命令行决定，网卡不能固定假设一定在最后一个槽位。
#define VIRTIO0 0x10001000
#define VIRTIO1 0x10002000
#define VIRTIO_NET 0x10008000
#define VIRTIO_MMIO_FIRST VIRTIO0
#define VIRTIO_MMIO_STRIDE 0x1000
#define VIRTIO_MMIO_COUNT 8
#define VIRTIO_MMIO_LAST (VIRTIO_MMIO_FIRST + (VIRTIO_MMIO_COUNT - 1) * VIRTIO_MMIO_STRIDE)
#define VIRTIO0_IRQ 1
#define VIRTIO1_IRQ 2
#define VIRTIO_NET_IRQ 8
#define VIRTIO_MMIO_IRQ_FIRST VIRTIO0_IRQ
#define VIRTIO_MMIO_IRQ_LAST VIRTIO_NET_IRQ


// core local interruptor (CLINT), which contains the timer.
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // cycles since boot.
#define CLINT_INTERVAL 1000000

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80200000
#define PHYSTOP 0xaf000000

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
// Physical memory layout

// 0x00200000 -- bios loads kernel here and jumps here
// 0x10000000 --
// 0x1c000000 -- reset address
// 0x1fe00000 -- I/O interrupt base address
// 0x1fe001e0 -- uart16550 serial port
// 0x90000000 -- RAM used by user pages


#define DMWIN_MASK 0x9UL << 60
#define DMWIN1_MASK 0x8UL << 60
#define VIRT_DMWIN_MASK 0xf000000000000000

#define VIRT2PHY(addr) ((addr) & ~VIRT_DMWIN_MASK)

// qemu puts UART registers here in virtual memory.
#define UART0 (0x1fe001e0UL | DMWIN_MASK)
#define UART0_IRQ 2

/* ============== LS7A registers =============== */
#define LS7A_PCH_REG_BASE		(0x10000000UL | DMWIN_MASK)

#define LS7A_INT_MASK_REG		LS7A_PCH_REG_BASE + 0x020
#define LS7A_INT_EDGE_REG		LS7A_PCH_REG_BASE + 0x060
#define LS7A_INT_CLEAR_REG		LS7A_PCH_REG_BASE + 0x080
#define LS7A_INT_HTMSI_VEC_REG		LS7A_PCH_REG_BASE + 0x200
#define LS7A_INT_STATUS_REG		LS7A_PCH_REG_BASE + 0x3a0
#define LS7A_INT_POL_REG		LS7A_PCH_REG_BASE + 0x3e0

// the kernel expects there to be RAM
// for use by user pages
// from physical address 0x90000000 to PHYSTOP.
#define PHYSBASE (0x0UL | DMWIN_MASK)
#define PHYSTOP (PHYSBASE + 128*1024*1024) //128MB physical memory

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
#define PA2VA(pa) ((pa) & (~(DMWIN_MASK)))



#endif
