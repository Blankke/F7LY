# 当前构建画像与工具链配置。
#
# 公开配置只保留 PROFILE 和 MODE：PROFILE 是一套不可拆分的平台组合，
# MODE 决定内嵌评测入口还是交互 shell。架构和开发板是画像内部事实，
# 不允许用户分别拼装，以免产生没有意义的组合。

EASTL_DIR := thirdparty/EASTL
KERNEL_DIR := kernel
BUILD_ROOT := $(PROJECT_ROOT)/build

PROFILE ?= riscv-qemu
MODE ?= evaluation
DIS_PRINTF ?= 0
VERBOSE_SRCS ?= 0

# 旧的 ARCH/BOARD/INITCODE_MODE 会绕过画像边界。明确报错比静默忽略更安全。
ifneq ($(filter command line override,$(origin ARCH)),)
  $(error ARCH 已废弃，请改用 PROFILE=<arch>-<board>)
endif
ifneq ($(filter command line override,$(origin BOARD)),)
  $(error BOARD 已废弃，请改用 PROFILE=<arch>-<board>)
endif
ifneq ($(filter command line override,$(origin INITCODE_MODE)),)
  $(error INITCODE_MODE 已废弃，请改用 MODE=evaluation 或 MODE=shell)
endif

# 公开构建入口会在递归子 make 中启用并行；QEMU 仍由串行父 make 独占终端。
NPROC ?= $(shell nproc)

# run/shell/debug 会先递归构建再占用终端。父 make 已有 jobserver 时必须继承
# 它的并行上限；只有普通串行调用才为构建子 make 补上宿主 CPU 数。
ifneq ($(strip $(findstring --jobserver-auth,$(MAKEFLAGS))$(findstring --jobserver-fds,$(MAKEFLAGS))),)
  BUILD_SUBMAKE_JOBS :=
else ifneq ($(filter -j%,$(MAKEFLAGS)),)
  BUILD_SUBMAKE_JOBS :=
else
  BUILD_SUBMAKE_JOBS := -j$(NPROC)
endif

# PROFILE 直接选择完整画像，不再允许架构和开发板自由组合。
AVAILABLE_PROFILES := $(sort $(basename $(notdir $(wildcard $(PROJECT_ROOT)/mk/platform/*.mk))))
PROFILE_FILE := $(PROJECT_ROOT)/mk/platform/$(PROFILE).mk
ifeq ($(wildcard $(PROFILE_FILE)),)
  $(error 不支持的 PROFILE=$(PROFILE)，可用画像: $(AVAILABLE_PROFILES))
endif

# 可选字段先给出空值，既明确平台画像契约，也让
# `make --warn-undefined-variables` 保持干净。
PROFILE_ARCH :=
PROFILE_BOARD :=
PROFILE_CPPFLAGS :=
PROFILE_SRCS :=
PROFILE_KERNEL_SUFFIX :=
include $(PROFILE_FILE)

# 平台画像只声明必要事实。驱动源码和宏可以为空，不引入复杂 schema。
ifeq ($(strip $(PROFILE_ARCH)),)
  $(error $(PROFILE_FILE) 缺少 PROFILE_ARCH)
endif
ifeq ($(strip $(PROFILE_BOARD)),)
  $(error $(PROFILE_FILE) 缺少 PROFILE_BOARD)
endif
ifeq ($(strip $(PROFILE_NAME)),)
  $(error $(PROFILE_FILE) 缺少 PROFILE_NAME)
endif
ifeq ($(strip $(PROFILE_DIR)),)
  $(error $(PROFILE_FILE) 缺少 PROFILE_DIR)
endif
ifeq ($(strip $(PROFILE_LINK_SCRIPT)),)
  $(error $(PROFILE_FILE) 缺少 PROFILE_LINK_SCRIPT)
endif
ifeq ($(wildcard $(PROFILE_LINK_SCRIPT)),)
  $(error 平台链接脚本不存在: $(PROFILE_LINK_SCRIPT))
endif

ifeq ($(PROFILE_ARCH),riscv)
  DEFAULT_CROSS_COMPILE := riscv64-linux-gnu-
  ARCH_DEFINES := -DRISCV
  ARCH_FLAGS := -mcmodel=medany
  KERNEL_ARCH_NAME := rv
  EA_PLATFORM := -DEA_PROCESSOR_RISCV
else ifeq ($(PROFILE_ARCH),loongarch)
  DEFAULT_CROSS_COMPILE := loongarch64-linux-gnu-
  ARCH_DEFINES := -DLOONGARCH
  ARCH_FLAGS := -march=loongarch64 -mabi=lp64d -mcmodel=normal \
                -Wno-error=use-after-free
  KERNEL_ARCH_NAME := la
  EA_PLATFORM := -DEA_PROCESSOR_LOONGARCH64
else
  $(error $(PROFILE_FILE) 的 PROFILE_ARCH=$(PROFILE_ARCH) 不受支持)
endif

CROSS_COMPILE ?= $(DEFAULT_CROSS_COMPILE)
CC      := $(CROSS_COMPILE)gcc
CXX     := $(CROSS_COMPILE)g++
LD      := $(CROSS_COMPILE)g++
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size
OBJDUMP := $(CROSS_COMPILE)objdump

ifeq ($(MODE),evaluation)
  KERNEL_MODE_SUFFIX :=
  BUILD_MODE_SUFFIX :=
else ifeq ($(MODE),shell)
  KERNEL_MODE_SUFFIX := -shell
  BUILD_MODE_SUFFIX := -shell
else
  $(error 不支持的 MODE=$(MODE)，请使用 evaluation 或 shell)
endif

BUILD_DIR := $(BUILD_ROOT)/$(PROFILE)$(BUILD_MODE_SUFFIX)
KERNEL_ELF := kernel-$(KERNEL_ARCH_NAME)$(PROFILE_KERNEL_SUFFIX)$(KERNEL_MODE_SUFFIX)
KERNEL_BIN := $(KERNEL_ELF).bin

CPPFLAGS := $(ARCH_DEFINES) $(PROFILE_CPPFLAGS)
ifeq ($(DIS_PRINTF),1)
  CPPFLAGS += -DDIS_PRINTF
endif

CFLAGS := -Wall -Werror -ffreestanding -O2 -fno-builtin -g \
          -fno-stack-protector $(ARCH_FLAGS)
CXXFLAGS := $(CFLAGS) -std=c++23 -nostdlib \
            -DEA_PLATFORM_LINUX -DEA_PLATFORM_POSIX $(EA_PLATFORM) \
            -DEA_ENDIAN_LITTLE=1 -Wno-deprecated-declarations \
            -Wno-strict-aliasing -fno-exceptions -fno-rtti \
            -Wno-maybe-uninitialized -Wno-volatile \
            -Wno-tautological-compare -Wno-unused-but-set-variable
LDFLAGS := -static -nostdlib -nostartfiles -nodefaultlibs \
           -Wl,-z,max-page-size=4096 -Wl,-T,$(PROFILE_LINK_SCRIPT) \
           -Wl,--gc-sections

ARCH_SOURCE_DIRS := boot/$(PROFILE_ARCH) hal/$(PROFILE_ARCH) \
                    link/$(PROFILE_ARCH) mem/$(PROFILE_ARCH) \
                    proc/$(PROFILE_ARCH) trap/$(PROFILE_ARCH)
COMMON_SOURCE_DIRS := libs tm sys shm
KERNEL_INCLUDE_DIRS := $(ARCH_SOURCE_DIRS) $(COMMON_SOURCE_DIRS) \
                       mem devs trap hal proc boot fs net platform \
                       net/onpstack/include
INCLUDES := -I$(KERNEL_DIR) \
            $(foreach dir,$(KERNEL_INCLUDE_DIRS),-I$(KERNEL_DIR)/$(dir)) \
            -I$(PROFILE_DIR) \
            -I$(EASTL_DIR)/include -I$(EASTL_DIR)/include/EASTL \
            -I$(EASTL_DIR)/test/packages/EABase/include/Common
