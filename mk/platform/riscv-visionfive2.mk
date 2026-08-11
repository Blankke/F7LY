# StarFive VisionFive 2/JH7110 实机的完整构建画像。
PROFILE_ARCH := riscv
PROFILE_BOARD := visionfive2
VISIONFIVE2_IPV4 ?= 192.168.1.2
VISIONFIVE2_NETMASK ?= 255.255.255.0
VISIONFIVE2_GATEWAY ?= 192.168.1.1
VISIONFIVE2_DNS ?= 192.168.1.1
VISIONFIVE2_BROADCAST ?= 192.168.1.255

PROFILE_NAME := StarFive VisionFive 2
PROFILE_CPPFLAGS := -DVISIONFIVE2_IPV4=\"$(VISIONFIVE2_IPV4)\" \
                     -DVISIONFIVE2_NETMASK=\"$(VISIONFIVE2_NETMASK)\" \
                     -DVISIONFIVE2_GATEWAY=\"$(VISIONFIVE2_GATEWAY)\" \
                     -DVISIONFIVE2_DNS=\"$(VISIONFIVE2_DNS)\" \
                     -DVISIONFIVE2_BROADCAST=\"$(VISIONFIVE2_BROADCAST)\"
PROFILE_DIR := kernel/platform/riscv/visionfive2
PROFILE_LINK_SCRIPT := kernel/link/riscv/visionfive2.ld
PROFILE_KERNEL_SUFFIX := -visionfive2
PROFILE_SRCS := kernel/fs/drivers/riscv/jh7110_dwmmc.cc \
                 kernel/net/drivers/riscv/jh7110_gmac.cc \
                 kernel/platform/common/no_rtc_backend.cc
