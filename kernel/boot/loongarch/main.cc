#include "boot/kernel_init.hh"

#include "boot_args.hh"
#include "devs/dtb.hh"
#include "devs/uart.hh"
#include "hal/loongarch/platform_board.hh"
#include "hal/smp.hh"
#include "libs/runtime_init.hh"
#include "platform/profile.hh"
#include "printer.hh"

extern char end[];

extern "C" void main(uint64 hartid, uint64 fw_arg0, uint64 fw_arg1,
                     uint64 fw_arg2, uint64 fw_arg3)
{
    // 主入口先清 BSS；次核从独立入口到达，只会观察已完成的构造状态。
    runtime::initialize_global_objects_once();
    hal::smp::enter(hartid, 0);

    k_printer.init();
    boardPrintfInfo("[boot] stage=firmware begin\n");

    const uint64 dtb_addr =
        loongarch::boot::resolve_dtb(fw_arg0, fw_arg1, fw_arg2, fw_arg3);
    if (dtb_addr == 0)
    {
        panic("未找到有效 DTB：profile=%s arg0=%p arg1=%p arg2=%p arg3=%p；2K1000 必须使用 U-Boot: go <kernel> <dtb>",
              platform::current_profile().name,
              fw_arg0,
              fw_arg1,
              fw_arg2,
              fw_arg3);
    }

    DtbManager::initialize_boot_dtb(dtb_addr);
    hal::smp::configure_topology();
    boardPrintfInfo("[boot] platform=%s dtb=0x%lx kernel-end=0x%lx\n",
                    platform::current_profile().name, k_dtb_addr,
                    loongarch::board::physical_address(
                        reinterpret_cast<uint64>(end)));

    boot::initialize_kernel({
        .boot_cpu_hwid = hartid,
        .device_tree_paddr = k_dtb_addr,
    });
}
