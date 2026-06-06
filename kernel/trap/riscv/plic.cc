#include "types.hh"
#include "param.h"
#include "mem/memlayout.hh"
#include "platform.hh"
#include "plic.hh"
#include "printer.hh"
#include "cpu.hh"

plic_manager plic_mgr;

void plic_manager::init()
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32*)(PLIC + UART0_IRQ*4) = 1;
  for (int irq = VIRTIO_MMIO_IRQ_FIRST; irq <= VIRTIO_MMIO_IRQ_LAST; ++irq)
  {
    *(uint32*)(PLIC + irq * 4) = 1;
  }
  printfGreen("[trap] Plic Manager Init\n");
}

void plic_manager::inithart()
{
    // !!后续修改
    // int hart = cpuid();
    int hart = 0;
  
    // 打开所有 virtio-mmio 槽位中断，具体设备由 trap 分发到块设备或网卡。
    uint32 enable_mask = (1 << UART0_IRQ);
    for (int irq = VIRTIO_MMIO_IRQ_FIRST; irq <= VIRTIO_MMIO_IRQ_LAST; ++irq)
    {
      enable_mask |= (1 << irq);
    }
    *(uint32*)PLIC_SENABLE(hart) = enable_mask;
  
    // set this hart's S-mode priority threshold to 0.
    *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

int plic_manager::claim()
{
    // !!后续修改
    // int hart = cpuid();
    int hart =r_tp();

    int irq = *(uint32*)PLIC_SCLAIM(hart);
    return irq;
}

void plic_manager::complete(int irq)
{
    // !!后续修改
    // int hart = cpuid();
    int hart = 0;

    *(uint32*)PLIC_SCLAIM(hart) = irq;
}
