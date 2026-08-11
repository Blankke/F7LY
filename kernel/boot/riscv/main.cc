#include "boot/kernel_init.hh"

#include "devs/dtb.hh"
#include "fuckyou.hh"
#include "hal/smp.hh"
#include "platform/profile.hh"
#include "printer.hh"

extern "C" void main(uint64 hartid, uint64 dtb_addr)
{
    // enter() 只让唯一引导核返回；次核在 HAL gate 中等待公共初始化完成。
    hal::smp::enter(hartid, dtb_addr);

    k_printer.init();
    DtbManager::initialize_boot_dtb(dtb_addr);
    hal::smp::configure_topology();

    // 保留现有 RISC-V 启动画面，但它不再承担任何初始化职责。
    printfWhite("\n\n");
    print_f7ly();
    print_fuckyou();
    printfWhite("\n\n");

    boot::initialize_kernel({
        .boot_cpu_hwid = hartid,
        .device_tree_paddr = k_dtb_addr,
    });
}
