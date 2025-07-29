#include"param.h"
#include "../hal/riscv/rv_csr.hh"

// 机器启动流程：open-sbi(M-mode) -> entry.S(S-mode) -> start.c -> main.c

// 操作系统启动时的栈空间(每个核心占4KB)
__attribute__ ((aligned (16))) char stack0[NCPU][4096];

extern void main();

void trap_loop()
{
    while(1);
}

// 确保start函数具有正确的外部链接名称
extern "C"
void start(uint64 hartid, uint64 dtb_entry)
{
    // 不进行分页(使用物理内存)
    riscv::csr::_write_csr_(riscv::csr::satp, 0);
        
    // // 使能S态的外设中断和时钟中断 (暂时不使用软件中断)
    // uint64 sie_val = riscv::csr::_read_csr_(riscv::csr::sie);
    // sie_val |= riscv::csr::sie_seie | riscv::csr::sie_stie | riscv::csr::sie_ssie;
    // riscv::csr::_write_csr_(riscv::csr::sie, sie_val);

    // trap响应程序设为死循环
    riscv::csr::_write_csr_(riscv::csr::stvec, (uint64)trap_loop);

    // 使用tp保存hartid以方便在S态查看
    riscv::w_tp(hartid);
    // 进入main函数完成一系列初始化
    main();
}