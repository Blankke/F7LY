#ifdef LOONGARCH
#include "types.hh"
#include "param.h"
#include "hal/loongarch/platform_board.hh"
#include "extioi.hh"
#include "printer.hh"
namespace
{
    uint64 g_enabled_sources = 0;

    volatile uint64 *register_address(uint64 physical)
    {
        return reinterpret_cast<volatile uint64 *>(
            loongarch::board::mmio_address(physical));
    }

    void write_register(uint64 physical, uint64 value)
    {
        *register_address(physical) = value;
    }

    uint64 read_register(uint64 physical)
    {
        return *register_address(physical);
    }
}

void extioi_init(void)
{
			g_enabled_sources = 0;
			write_register(loongarch::board::k_extioi_enable_physical, 0);

			write_register(loongarch::board::k_extioi_map_physical, 0x01UL);

			write_register(loongarch::board::k_extioi_route_physical, 0x10000UL);

			// 7. 设置节点类型（HT 向量）
			write_register(loongarch::board::k_extioi_nodetype_physical, 0x1);
}

bool extioi_enable(uint32 source)
{
    if (source >= 64)
    {
        return false;
    }
    g_enabled_sources |= 1ULL << source;
    write_register(loongarch::board::k_extioi_enable_physical, g_enabled_sources);
    return true;
}

// ask the extioi what interrupt we should serve.
uint64
extioi_claim(void)
{
    return read_register(loongarch::board::k_extioi_isr_physical);

}

void extioi_complete(uint64 irq)
{
    write_register(loongarch::board::k_extioi_isr_physical, irq);
}
#endif
