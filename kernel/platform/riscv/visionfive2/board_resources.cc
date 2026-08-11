#include "hal/riscv/platform_board.hh"

#include "devs/dtb.hh"
#include "hal/cpu.hh"
#include "param.h"
#include "platform/memory.hh"
#include "printer.hh"
#include "tm/platform_clock_backend.hh"
#include "tm/time.hh"

namespace riscv::board
{
namespace
{
constexpr uint64 k_invalid_context = ~0ULL;
constexpr uint64 k_plic_context_register_base = 0x200000ULL;
constexpr uint64 k_plic_context_register_stride = 0x1000ULL;
constexpr uint64 k_plic_claim_register_end = 8ULL;
static_assert(k_plic_mmio.size >=
              k_plic_context_register_base + k_plic_claim_register_end);
uint64 g_plic_contexts[NCPU]{};
bool g_plic_contexts_ready = false;

// SYSCRG 的 clock control 是按 clock ID 索引的 32 位数组。
constexpr uint32 k_sdio1_ahb_clock_id = 92;
constexpr uint32 k_sdio1_sdcard_clock_id = 94;
constexpr uint32 k_clock_enable = 1U << 31;
constexpr uint32 k_clock_divider_mask = 0x00ffffffU;
constexpr uint32 k_sdio1_sdcard_divider = 8;

constexpr uint32 k_sdio1_reset_id = 65;
constexpr uint64 k_reset_assert_base = 0x2f8;
constexpr uint64 k_reset_status_base = 0x308;
constexpr uint64 k_reset_timeout_us = 100'000;
static_assert(static_cast<uint64>(k_sdio1_sdcard_clock_id + 1U) *
                      sizeof(uint32) <=
                  k_syscrg_mmio.size);
static_assert(k_reset_status_base +
                      (static_cast<uint64>(k_sdio1_reset_id / 32U) + 1U) *
                          sizeof(uint32) <=
                  k_syscrg_mmio.size);

volatile uint32 *syscrg_register(uint64 offset)
{
    const uint64 address = platform::memory::kernel_access_address(
        k_syscrg_mmio.physical_base + offset);
    return reinterpret_cast<volatile uint32 *>(address);
}

uint32 read_syscrg(uint64 offset)
{
    return *syscrg_register(offset);
}

void write_syscrg(uint64 offset, uint32 value)
{
    *syscrg_register(offset) = value;
}

void syscrg_io_fence()
{
    asm volatile("fence iorw, iorw" ::: "memory");
}

void initialize_plic_contexts()
{
    for (uint64 &context : g_plic_contexts)
    {
        context = k_invalid_context;
    }

    DtbRiscvPlicContext parsed[NCPU]{};
    const int parsed_count = DtbManager::get_riscv_plic_contexts(
        k_plic_mmio.physical_base, parsed, NCPU);
    if (parsed_count <= 0)
    {
        panic("[PLIC] VisionFive2 DTB has no valid S-mode context mapping");
    }

    for (int index = 0; index < parsed_count; ++index)
    {
        const uint64 hartid = parsed[index].hartid;
        const uint64 context = parsed[index].context_id;
        const bool context_fits =
            context <=
            (k_plic_mmio.size - k_plic_context_register_base -
             k_plic_claim_register_end) /
                k_plic_context_register_stride;
        if (hartid >= NCPU || !context_fits ||
            g_plic_contexts[hartid] != k_invalid_context)
        {
            panic("[PLIC] invalid/duplicate DTB context hart=%lu context=%u",
                  hartid, parsed[index].context_id);
        }
        g_plic_contexts[hartid] = context;
    }

    const uint64 possible = Cpu::possible_cpu_mask();
    for (uint64 hartid = 0; hartid < NCPU; ++hartid)
    {
        if ((possible & (1ULL << hartid)) == 0)
        {
            continue;
        }
        if (g_plic_contexts[hartid] == k_invalid_context)
        {
            panic("[PLIC] DTB lacks S-mode context for possible hart=%lu",
                  hartid);
        }
        boardPrintfInfo("[PLIC] hart=%lu raw-context=%lu\n",
                        hartid, g_plic_contexts[hartid]);
    }

    // 主核会在放行次核前完成本函数；bootstrap gate 的 release/acquire 保证
    // 次核看到完整数组后才调用本平台接口。
    g_plic_contexts_ready = true;
}
} // namespace

bool prepare_dw_mmc_hardware()
{
    constexpr uint64 ahb_clock_offset =
        static_cast<uint64>(k_sdio1_ahb_clock_id) * sizeof(uint32);
    constexpr uint64 card_clock_offset =
        static_cast<uint64>(k_sdio1_sdcard_clock_id) * sizeof(uint32);
    constexpr uint32 reset_word = k_sdio1_reset_id / 32U;
    constexpr uint32 reset_mask = 1U << (k_sdio1_reset_id % 32U);
    constexpr uint64 reset_assert_offset =
        k_reset_assert_base + static_cast<uint64>(reset_word) * sizeof(uint32);
    constexpr uint64 reset_status_offset =
        k_reset_status_base + static_cast<uint64>(reset_word) * sizeof(uint32);

    // AHB 只需打开 gate；SDCARD 时钟还需把 400 MHz 父频除以 8。
    const uint32 ahb_control = read_syscrg(ahb_clock_offset) | k_clock_enable;
    const uint32 card_control =
        (read_syscrg(card_clock_offset) & ~k_clock_divider_mask) |
        k_sdio1_sdcard_divider | k_clock_enable;
    write_syscrg(ahb_clock_offset, ahb_control);
    write_syscrg(card_clock_offset, card_control);
    syscrg_io_fence();

    const uint32 ahb_readback = read_syscrg(ahb_clock_offset);
    const uint32 card_readback = read_syscrg(card_clock_offset);
    if ((ahb_readback & k_clock_enable) == 0 ||
        (card_readback & (k_clock_enable | k_clock_divider_mask)) !=
            (k_clock_enable | k_sdio1_sdcard_divider))
    {
        platformDiagnosticError(
            "[dwmmc] SYSCRG clock readback failed: ahb=0x%x card=0x%x\n",
            ahb_readback, card_readback);
        return false;
    }

    // reset ID 65 位于 word2/bit1。assert 寄存器清零表示 deassert，
    // status 对应位变 1 才表示控制器真正离开复位。
    write_syscrg(reset_assert_offset,
                 read_syscrg(reset_assert_offset) & ~reset_mask);
    syscrg_io_fence();
    const uint32 assert_readback = read_syscrg(reset_assert_offset);
    if ((assert_readback & reset_mask) != 0)
    {
        platformDiagnosticError(
            "[dwmmc] SYSCRG reset deassert did not latch: assert=0x%x\n",
            assert_readback);
        return false;
    }

    const uint64 start = platform::clock_backend::read_ticks();
    const uint64 timeout_cycles = tmm::microseconds_to_cycles(k_reset_timeout_us);
    uint32 reset_status = 0;
    do
    {
        reset_status = read_syscrg(reset_status_offset);
        if ((reset_status & reset_mask) != 0)
        {
            platformDiagnosticInfo(
                "[dwmmc] SYSCRG ready: ahb_clock=%u card_clock=%u "
                "divider=%u reset=%u\n",
                k_sdio1_ahb_clock_id, k_sdio1_sdcard_clock_id,
                k_sdio1_sdcard_divider, k_sdio1_reset_id);
            return true;
        }
        asm volatile("nop");
    } while (platform::clock_backend::read_ticks() - start < timeout_cycles);

    platformDiagnosticError(
        "[dwmmc] SYSCRG reset status timed out: status=0x%x mask=0x%x\n",
        reset_status, reset_mask);
    return false;
}

uint64 plic_context_for_cpu(uint64 cpu_id)
{
    if (!g_plic_contexts_ready)
    {
        initialize_plic_contexts();
    }
    if (cpu_id >= NCPU || g_plic_contexts[cpu_id] == k_invalid_context)
    {
        panic("[PLIC] no raw context for hart=%lu", cpu_id);
    }
    return g_plic_contexts[cpu_id];
}
} // namespace riscv::board
