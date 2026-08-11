#include "hal/riscv/platform_board.hh"

namespace riscv::board
{
uint64 plic_context_for_cpu(uint64 cpu_id)
{
    // QEMU virt 的 interrupts-extended 按每个 hart 的 M、S context 交错排列，
    // 因此 S-mode 的规范 raw context 是 2 * hartid + 1。
    return 2 * cpu_id + 1;
}
} // namespace riscv::board
