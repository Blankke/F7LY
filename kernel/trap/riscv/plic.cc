#include "types.hh"
#include "param.h"
#include "hal/irq.hh"
#include "hal/riscv/platform_board.hh"
#include "plic.hh"
#include "cpu.hh"

namespace riscv::plic
{
namespace
{
  constexpr uint32 k_max_public_sources = hal::irq::k_max_sources;

  constexpr uint64 plic_register(uint64 offset)
  {
    return riscv::board::k_plic_mmio.physical_base + offset;
  }

  volatile uint32 *enable_word(uint64 context, uint32 source)
  {
    return reinterpret_cast<volatile uint32 *>(
        plic_register(0x2000 + context * 0x80) +
        (source / 32U) * sizeof(uint32));
  }

  volatile uint32 *priority_register(uint32 source)
  {
    return reinterpret_cast<volatile uint32 *>(
        plic_register(static_cast<uint64>(source) * sizeof(uint32)));
  }

  volatile uint32 *threshold_register(uint64 context)
  {
    return reinterpret_cast<volatile uint32 *>(
        plic_register(0x200000 + context * 0x1000));
  }

  volatile uint32 *claim_register(uint64 context)
  {
    return reinterpret_cast<volatile uint32 *>(
        plic_register(0x200004 + context * 0x1000));
  }
}

void initialize_global()
{
  // 启动时先关闭公共范围内全部 source，随后由 handler 注册表逐项启用。
  // 固件可能留下优先级状态，显式归零可避免未注册设备提前打断启动流程。
  for (uint32 source = 1; source <= riscv::board::k_plic_source_count; ++source)
  {
    *priority_register(source) = 0;
  }
}

void initialize_current_cpu(uint64 enabled_sources)
{
  const uint64 context =
      riscv::board::plic_context_for_cpu(Cpu::current_cpu_id());
  // 固件可能给公共 registry 范围之外的设备留下 enable 位。先清空当前
  // context 的完整硬件位图，再只恢复已有 handler owner 的公共 source。
  const uint32 enable_word_count =
      riscv::board::k_plic_source_count / 32U + 1U;
  for (uint32 word = 0; word < enable_word_count; ++word)
  {
    *enable_word(context, word * 32U) = 0;
  }
  // source 0 是 PLIC 的“无中断”保留值，不能被写入 enable bitmap。
  enabled_sources &= ~1ULL;
  *enable_word(context, 0) = static_cast<uint32>(enabled_sources);
  *enable_word(context, 32) = static_cast<uint32>(enabled_sources >> 32);
  *threshold_register(context) = 0;
}

bool enable_source(uint32 source)
{
  if (source == 0 || source >= k_max_public_sources ||
      source > riscv::board::k_plic_source_count)
  {
    return false;
  }

  *priority_register(source) = 1;

  // 后注册的设备（例如首次创建 socket 时探测到的网卡）也必须能在已经
  // online 的任意 PLIC context 上触发；次核尚未启动时写它的 context 也安全。
  const uint64 possible = Cpu::possible_cpu_mask();
  for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
  {
    if ((possible & (1ULL << cpu_id)) == 0)
    {
      continue;
    }
    const uint64 context = riscv::board::plic_context_for_cpu(cpu_id);
    volatile uint32 *word = enable_word(context, source);
    *word = *word | (1U << (source % 32U));
  }
  return true;
}

uint32 claim()
{
  const uint64 context =
      riscv::board::plic_context_for_cpu(Cpu::current_cpu_id());
  return *claim_register(context);
}

void complete(uint32 source)
{
  const uint64 context =
      riscv::board::plic_context_for_cpu(Cpu::current_cpu_id());
  *claim_register(context) = source;
}
} // namespace riscv::plic
