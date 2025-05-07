#include "types.hh"
#include "param.h"
#include "mem/memlayout.hh"
#include "platform.hh"
#include "plic.hh"



plic_manager plic_mgr;

void plic_manager::init()
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32*)(PLIC + UART0_IRQ*4) = 1;
  *(uint32*)(PLIC + VIRTIO0_IRQ*4) = 1;
  *(uint32*)(PLIC + VIRTIO1_IRQ*4) = 1;
}

void plic_manager::inithart()
{
    // !!后续修改
    // int hart = cpuid();
    int hart = 0;
  
    // set enable bits for this hart's S-mode
    // for the uart and virtio disk.
    *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ) | (1 << VIRTIO1_IRQ);
  
    // set this hart's S-mode priority threshold to 0.
    *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

int plic_manager::claim()
{
    // !!后续修改
    // int hart = cpuid();
    int hart = 0;

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

