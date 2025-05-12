#include "types.hh"
#include "param.h"
#include "mem/memlayout.hh"
#include "platform.hh"
#include "extioi.hh"
#include "pci.hh"

void extioi_init(void)
{
    // iocsr_writeq((0x1UL << UART0_IRQ) | (0x1UL << PCIE_IRQ), LOONGARCH_IOCSR_EXTIOI_EN_BASE);

    // iocsr_writeq(0x01UL,LOONGARCH_IOCSR_EXTIOI_MAP_BASE);

    // iocsr_writeq(0x10000UL,LOONGARCH_IOCSR_EXTIOI_ROUTE_BASE);


    // iocsr_writeq(0x1,LOONGARCH_IOCSR_EXRIOI_NODETYPE_BASE);
}

// ask the extioi what interrupt we should serve.
uint64
extioi_claim(void)
{
    // return iocsr_readq(LOONGARCH_IOCSR_EXTIOI_ISR_BASE);
    return 0;
}

void extioi_complete(uint64 irq)
{
    // iocsr_writeq(irq, LOONGARCH_IOCSR_EXTIOI_ISR_BASE);
}