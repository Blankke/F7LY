# RISC-V QEMU virt 的完整构建画像。
# 平台画像只负责“选哪些实现”，设备行为仍由对应驱动实现。
PROFILE_ARCH := riscv
PROFILE_BOARD := qemu
PROFILE_NAME := RISC-V QEMU virt
PROFILE_DIR := kernel/platform/riscv/qemu
PROFILE_LINK_SCRIPT := kernel/link/riscv/kernel.ld
PROFILE_KERNEL_SUFFIX :=
PROFILE_SRCS := kernel/fs/drivers/riscv/virtio_disk2.cc \
                 kernel/fs/drivers/virtio_blk_device.cc \
                 kernel/fs/drivers/virtio_blk_queue.cc \
                 kernel/fs/drivers/virtio_mclock_scheduler.cc \
                 kernel/net/drivers/virtio_net.cc \
                 kernel/net/drivers/riscv/virtio_net_mmio_transport.cc \
                 kernel/platform/common/no_rtc_backend.cc
