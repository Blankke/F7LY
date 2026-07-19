#include "types.hh"
#include "param.h"
#include "mem/memlayout.hh"
#include "platform.hh"
#include "plic.hh"
#include "printer.hh"
#include "cpu.hh"

plic_manager plic_mgr;

namespace
{
  // PLIC 的 enable 区域是位图，每 32 个中断号占一个 32 位寄存器。
  // VF2 的 UART IRQ=32，不能再用单个 uint32 和 (1 << irq) 表示。
  void enable_supervisor_irq(volatile uint32 *enable, int irq)
  {
    if (irq <= 0)
    {
      return;
    }
    enable[irq / 32] |= (1u << (irq % 32));
  }
}

void plic_manager::init()
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32*)(PLIC + UART0_IRQ*4) = 1;
#ifndef VISIONFIVE2
  for (int irq = VIRTIO_MMIO_IRQ_FIRST; irq <= VIRTIO_MMIO_IRQ_LAST; ++irq)
  {
    *(uint32*)(PLIC + irq * 4) = 1;
  }
#endif
  printfGreen("[trap] Plic Manager Init\n");
}

void plic_manager::inithart()
{
    // 使用启动入口写入 tp 的 hartid；VF2 的启动 hart 不保证是 hart0。
    int hart = r_tp();
    volatile uint32 *enable = reinterpret_cast<volatile uint32 *>(PLIC_SENABLE(hart));
    for (int word = 0; word < 32; ++word)
    {
      enable[word] = 0;
    }

    enable_supervisor_irq(enable, UART0_IRQ);
#ifndef VISIONFIVE2
    // QEMU virt 打开的 virtio-mmio 槽位由 trap 分发到块设备或网卡。
    for (int irq = VIRTIO_MMIO_IRQ_FIRST; irq <= VIRTIO_MMIO_IRQ_LAST; ++irq)
    {
      enable_supervisor_irq(enable, irq);
    }
#endif
  
    // set this hart's S-mode priority threshold to 0.
    *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

int plic_manager::claim()
{
    int hart = r_tp();

    int irq = *(uint32*)PLIC_SCLAIM(hart);
    return irq;
}

void plic_manager::complete(int irq)
{
    int hart = r_tp();

    *(uint32*)PLIC_SCLAIM(hart) = irq;
}
