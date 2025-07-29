#pragma once

// Kernel stack configuration
// 内核栈配置常量
#define KSTACK_PAGES 2          // 内核栈使用的页面数 (8KB)
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

// virtio mmio interface

#define VIRTIO0 0x10001000
#define VIRTIO1 0x10002000
#define VIRTIO0_IRQ 1
#define VIRTIO1_IRQ 2


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
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define SIG_TRAMPOLINE   (TRAMPOLINE - PGSIZE)
#define TRAPFRAME (SIG_TRAMPOLINE - PGSIZE)
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
#define PHYSBASE (0x90000000UL | DMWIN_MASK)
#define PHYSTOP (PHYSBASE + 512*1024*1024) //512MB physical memory

#define TRAPFRAME ((MAXVA>>1 )- PGSIZE) //64TB

// map kernel stacks beneath the trampframe,
// each surrounded by invalid guard pages.
#define SIG_TRAMPOLINE   (TRAPFRAME - PGSIZE)
#define KSTACK(p) (SIG_TRAMPOLINE - (((p)+1)* KSTACK_TOTAL_PAGES - KSTACK_GUARD_PAGES )*PGSIZE)
#define PA2VA(pa) ((pa) & (~(DMWIN_MASK)))



#endif