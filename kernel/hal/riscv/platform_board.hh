#pragma once

#include "types.hh"

// 具体文件由 Makefile 选中的 PROFILE_DIR 提供。RISC-V 架构代码只依赖
// riscv::board 这组窄资源，不需要知道当前机器是 QEMU 还是未来的实机。
#include "platform_board_config.hh"

namespace riscv::board
{
// 返回 PLIC 规范寄存器布局中的 raw context。QEMU 的 S-mode context 是
// 2*hartid+1；VisionFive2 必须从 DTB interrupts-extended 建立映射表。
uint64 plic_context_for_cpu(uint64 cpu_id);
} // namespace riscv::board
