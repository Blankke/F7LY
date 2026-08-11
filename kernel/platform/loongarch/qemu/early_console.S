.section .text
.global platform_early_boot_marker
platform_early_boot_marker:
        # QEMU 启动日志由固件和运行时 Console 提供，早期阶段无需直接碰设备。
        jr          $ra
