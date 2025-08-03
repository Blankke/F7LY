#ifdef LOONGARCH
#include "types.hh"
#include "param.h"
#include "mem/memlayout.hh"
#include "platform.hh"
#include "extioi.hh"
#include "trap/loongarch/pci.h"
#include "printer.hh"
void extioi_init(void)
{
			write_itr_cfg_64b(LOONGARCH_IOCSR_EXTIOI_EN_BASE, (0x1UL << UART0_IRQ)
					| (0x1UL << PCIE_IRQ));  

			write_itr_cfg_64b(LOONGARCH_IOCSR_EXTIOI_MAP_BASE, 0x01UL);

			write_itr_cfg_64b(LOONGARCH_IOCSR_EXTIOI_ROUTE_BASE, 0x10000UL);

			// 7. 设置节点类型（HT 向量）
			write_itr_cfg_64b(LOONGARCH_IOCSR_EXRIOI_NODETYPE_BASE, 0x1);
			printfGreen("[extioi]  ExtIOI initialized.\n");
}

// ask the extioi what interrupt we should serve.
uint64
extioi_claim(void)
{
    return read_itr_cfg_64b(LOONGARCH_IOCSR_EXTIOI_ISR_BASE);

}

void extioi_complete(uint64 irq)
{
    write_itr_cfg_64b( LOONGARCH_IOCSR_EXTIOI_ISR_BASE,irq);
}
#endif