#include "param.h"
#include "hal/arch.hh"
#include "libs/runtime_init.hh"
#include "platform/console_backend.hh"
#include "platform/profile.hh"

// 机器启动流程：open-sbi(M-mode) -> entry.S(S-mode) -> start.c -> main.c

// 操作系统启动时的栈空间；entry.S 使用同一个 8KB 常量计算每个 hart 的槽位。
// 启动栈不属于普通 BSS 清零区间：多个 hart 可能已经在各自栈上等待唯一
// 清零者。链接脚本会先单独放置 .bss.stack，再定义 bss_start。
__attribute__((aligned(16), section(".bss.stack")))
char stack0[NCPU][4096 * 2];

extern "C" void main(uint64 hartid, uint64 dtb_entry);

void trap_loop()
{
    while(1);
}

// 确保start函数具有正确的外部链接名称
extern "C"
void start(uint64 hartid, uint64 dtb_entry)
{
    // 固件交接后首先建立确定的 S-mode 执行环境。下方极早期 SBI 输出不能
    // 依赖固件遗留页表；异常向量也必须先于任何 C++ 访问落到可控死循环。
    intr_off();
    w_sie(0);
    w_satp(0);
    sfence_vma();
    w_stvec((uint64)trap_loop);

    // HSM 后续拉起的次核也会重走 start()；构造状态位于已装载的 .data，
    // 用它把极早期标记限制在冷启动阶段，避免每个次核重复刷串口。
    const bool early_diagnostics =
        platform::current_profile().verbose_boot_diagnostics &&
        !runtime::global_constructors_ready();
    if (early_diagnostics)
    {
        platform::console_backend::early_write("[boot] early runtime begin\n");
    }

    // 汇编入口已提前检查，这里保留 C 侧保护，避免未来替换入口时破坏 CPU 数组边界。
    if (hartid >= NCPU)
    {
        platform::console_backend::early_write("[boot] fatal: hartid exceeds NCPU\n");
        trap_loop();
    }
    if (dtb_entry == 0)
    {
        platform::console_backend::early_write("[boot] fatal: firmware passed null DTB\n");
        trap_loop();
    }

    // QEMU ELF loader 往往会顺便清 BSS，但 U-Boot/裸二进制加载没有该保证。
    // 显式的一次性清零让同一入口可安全复用于未来 VisionFive2 上板画像。
    runtime::initialize_zero_storage_once();

    // 全局虚表、EASTL 容器和带构造函数的设备必须先于任何 HAL/boot 对象使用。
    // 多 hart 同时由固件放行时，运行时层会选一个执行，其余在此等待。
    runtime::initialize_global_objects_once();

    if (early_diagnostics)
    {
        platform::console_backend::early_write("[boot] C++ runtime ready\n");
    }

    // // 使能S态的外设中断和时钟中断 (暂时不使用软件中断)
    // uint64 sie_val = r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE;
    // w_sie(sie_val);

    // 使用tp保存hartid以方便在S态查看
    w_tp(hartid);
    // 进入main函数完成一系列初始化
    main(hartid, dtb_entry);
}
