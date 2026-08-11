# LoongArch QEMU virt 的完整构建画像。
PROFILE_ARCH := loongarch
PROFILE_BOARD := qemu
PROFILE_NAME := LoongArch QEMU virt
PROFILE_DIR := kernel/platform/loongarch/qemu
PROFILE_LINK_SCRIPT := kernel/link/loongarch/kernel.ld
PROFILE_KERNEL_SUFFIX :=
PROFILE_SRCS := kernel/fs/drivers/loongarch/virtio_disk.cc \
                 kernel/devs/virtio/pci.cc \
                 kernel/fs/drivers/virtio_blk_device.cc \
                 kernel/fs/drivers/virtio_blk_queue.cc \
                 kernel/fs/drivers/virtio_mclock_scheduler.cc \
                 kernel/net/drivers/virtio_net.cc \
                 kernel/net/drivers/loongarch/virtio_net_pci_transport.cc \
                 kernel/platform/common/no_rtc_backend.cc
