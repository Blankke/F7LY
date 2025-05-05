#include "plic.hh"
#include "memlayout.hh"


//
// Platform-Level Interrupt Controller (PLIC) 
//

void Plic::init() {
    // 设置中断优先级为非零
    *(volatile uint32*)(PLIC + UART0_IRQ * 4) = 1;
    *(volatile uint32*)(PLIC + VIRTIO0_IRQ * 4) = 1;
    *(volatile uint32*)(PLIC + VIRTIO1_IRQ * 4) = 1;
}

void Plic::init_hart() {
    int hart = cpuid();

    // 启用 S-mode 的中断源（UART、VirtIO）
    *(volatile uint32*)PLIC_SENABLE(hart) =
        (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ) | (1 << VIRTIO1_IRQ);

    // 设置优先级门槛为 0
    *(volatile uint32*)PLIC_SPRIORITY(hart) = 0;
}

int Plic::claim() {
    int hart = cpuid();
    return *(volatile uint32*)PLIC_SCLAIM(hart);
}

void Plic::complete(int irq) {
    int hart = cpuid();
    *(volatile uint32*)PLIC_SCLAIM(hart) = irq;
}
