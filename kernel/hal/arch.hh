#pragma once

// 该文件是通用 C++ 代码选择架构机制的唯一入口。板级 UART、IRQ、PCI 等
// 资源仍由 Makefile 选中的 kernel/platform/<arch>/<board>/ 提供。
#include "mem/page.hh"

#if defined(RISCV)
#include "hal/riscv/arch.hh"
#include "mem/riscv/page_table_defs.hh"
#elif defined(LOONGARCH)
#include "hal/loongarch/arch.hh"
#include "mem/loongarch/page_table_defs.hh"
#else
#error "必须选择 RISCV 或 LOONGARCH 架构"
#endif
