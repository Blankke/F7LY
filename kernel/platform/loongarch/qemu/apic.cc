#include "apic.hh"
#include "hal/loongarch/platform_board.hh"

//
// the loongarch 7A1000 I/O Interrupt Controller Registers.
//

namespace
{
constexpr uint64 k_mask_offset = 0x020;
constexpr uint64 k_edge_offset = 0x060;
constexpr uint64 k_clear_offset = 0x080;
constexpr uint64 k_vector_offset = 0x200;
constexpr uint64 k_polarity_offset = 0x3e0;

uint64 register_address(uint64 offset)
{
  return loongarch::board::mmio_address(
             loongarch::board::k_pch_pic_mmio.physical_base) +
         offset;
}
} // namespace

void apic_init(void)
{
  // 先屏蔽全部输入；公共 handler 表随后只开放确实已有处理函数的 source。
  *reinterpret_cast<volatile uint64 *>(register_address(k_mask_offset)) = ~0ULL;
  *reinterpret_cast<volatile uint64 *>(register_address(k_edge_offset)) = 0;
  *reinterpret_cast<volatile uint64 *>(register_address(k_polarity_offset)) = 0;
}

bool apic_enable(uint32 source)
{
  if (source >= 64)
  {
    return false;
  }

  const uint64 bit = 1ULL << source;
  // QEMU virt 的 UART 与 PCIe 输入沿用原有边沿触发配置；向量号与 hwirq
  // 保持一一对应，ExtIOI claim 后无需再做第二套编号翻译。
  *reinterpret_cast<volatile uint8 *>(register_address(k_vector_offset) + source) =
      static_cast<uint8>(source);
  volatile uint64 *edge = reinterpret_cast<volatile uint64 *>(
      register_address(k_edge_offset));
  volatile uint64 *mask = reinterpret_cast<volatile uint64 *>(
      register_address(k_mask_offset));
  *edge = *edge | bit;
  *mask = *mask & ~bit;
  return true;
}

// tell the apic we've served this IRQ.
void apic_complete(uint64 irq)

{
  *reinterpret_cast<volatile uint64 *>(register_address(k_clear_offset)) = irq;
}
