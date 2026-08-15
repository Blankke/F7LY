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
PERF_DIAG ?= 0
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

# run/shell/debug 会先递归构建再占用终端。MAKEFLAGS 中的 jobserver 参数在
# Makefile 解析完成后才稳定，因此这里必须延迟到 recipe 展开时判断。父 make
# 已经并行时直接继承它的令牌；只有普通串行调用才为构建子 make 补并行度。
BUILD_SUBMAKE_JOBS = $(if $(strip \
  $(findstring --jobserver-auth,$(MAKEFLAGS)) \
  $(findstring --jobserver-fds,$(MAKEFLAGS)) \
  $(filter -j%,$(MAKEFLAGS))),,-j$(NPROC))

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
PROFILE_CODEGEN_FLAGS :=
PROFILE_SRCS :=
PROFILE_KERNEL_SUFFIX :=
PROFILE_MAX_CPUS :=
PROFILE_SECONDARY_START_TIMEOUT_US :=
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
ifeq ($(strip $(PROFILE_MAX_CPUS)),)
  $(error $(PROFILE_FILE) 缺少 PROFILE_MAX_CPUS)
endif
ifeq ($(strip $(PROFILE_SECONDARY_START_TIMEOUT_US)),)
  $(error $(PROFILE_FILE) 缺少 PROFILE_SECONDARY_START_TIMEOUT_US)
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
  # 内核使用特权 CSR，新版工具链不再由基础 I 隐式启用 Zicsr。
  KERNEL_ABI_FLAGS := -march=rv64imac_zicsr -mabi=lp64
  # Linux 交叉 sysroot 只安装 lp64d glibc 头文件。下列宏只让头文件
  # 选择现有的 stubs-lp64d.h；-mabi=lp64 仍唯一决定内核代码生成 ABI。
  KERNEL_SYSROOT_HEADER_FLAGS := \
      -U__riscv_float_abi_soft -D__riscv_float_abi_double
  KERNEL_ARCH_FLAGS := -mcmodel=medany $(KERNEL_ABI_FLAGS) \
                       $(KERNEL_SYSROOT_HEADER_FLAGS)
  INITCODE_ABI_FLAGS := -march=rv64imafdc -mabi=lp64d
  INITCODE_ARCH_FLAGS := -mcmodel=medany $(INITCODE_ABI_FLAGS)
  # 上下文汇编允许保存浮点寄存器，但继续使用内核 soft-float 调用 ABI。
  CONTEXT_ASM_FLAGS := -march=rv64imafdc_zicsr -mabi=lp64
  KERNEL_ARCH_NAME := rv
else ifeq ($(PROFILE_ARCH),loongarch)
  DEFAULT_CROSS_COMPILE := loongarch64-linux-gnu-
  ARCH_DEFINES := -DLOONGARCH
  # 对齐能力属于平台而非 ISA ABI：支持 UAL 的 QEMU 应保留原生宽访存，
  # 不支持未对齐访问的真机画像再通过 PROFILE_CODEGEN_FLAGS 收紧代码生成。
  # 这对 packed 文件系统元数据尤为重要，避免一次宽访问被展开为多次字节访问。
  KERNEL_ABI_FLAGS := -march=loongarch64 -mabi=lp64s -mfpu=none
  # Linux 交叉 sysroot 同样只提供 lp64d glibc 头；头文件选择不改变
  # -mabi=lp64s -mfpu=none 约束的内核 soft-float 代码生成。
  KERNEL_SYSROOT_HEADER_FLAGS := \
      -U__loongarch_soft_float -D__loongarch_double_float
  KERNEL_ARCH_FLAGS := $(KERNEL_ABI_FLAGS) $(PROFILE_CODEGEN_FLAGS) -mcmodel=normal \
                       $(KERNEL_SYSROOT_HEADER_FLAGS)
  INITCODE_ABI_FLAGS := -march=loongarch64 -mabi=lp64d -mfpu=64
  INITCODE_ARCH_FLAGS := $(INITCODE_ABI_FLAGS) -mcmodel=normal
  INITCODE_ARCH_FLAGS += $(PROFILE_CODEGEN_FLAGS)
  CONTEXT_ASM_FLAGS := -march=loongarch64 -mabi=lp64s -mfpu=64
  KERNEL_ARCH_NAME := la
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

ifeq ($(PERF_DIAG),0)
  KERNEL_DIAG_SUFFIX :=
  BUILD_DIAG_SUFFIX :=
else ifeq ($(PERF_DIAG),1)
  KERNEL_DIAG_SUFFIX := -perf
  BUILD_DIAG_SUFFIX := -perf
else
  $(error 不支持的 PERF_DIAG=$(PERF_DIAG)，请使用 0 或 1)
endif

BUILD_DIR := $(BUILD_ROOT)/$(PROFILE)$(BUILD_MODE_SUFFIX)$(BUILD_DIAG_SUFFIX)
KERNEL_ELF := kernel-$(KERNEL_ARCH_NAME)$(PROFILE_KERNEL_SUFFIX)$(KERNEL_MODE_SUFFIX)$(KERNEL_DIAG_SUFFIX)
KERNEL_BIN := $(KERNEL_ELF).bin

CPPFLAGS := $(ARCH_DEFINES) $(PROFILE_CPPFLAGS) \
            -DF7LY_MAX_CPUS=$(PROFILE_MAX_CPUS) \
            -DF7LY_SECONDARY_START_TIMEOUT_US=$(PROFILE_SECONDARY_START_TIMEOUT_US)
ifeq ($(DIS_PRINTF),1)
  CPPFLAGS += -DDIS_PRINTF
endif
ifeq ($(PERF_DIAG),1)
  CPPFLAGS += -DF7LY_PERF_DIAG=1
endif

CFLAGS := -Wall -Werror -ffreestanding -O2 -g \
          -fno-stack-protector $(KERNEL_ARCH_FLAGS)
ifeq ($(PERF_DIAG),1)
  CFLAGS += -fno-omit-frame-pointer -fno-optimize-sibling-calls
endif
CXXFLAGS := $(CFLAGS) -std=c++23 -Wno-deprecated-declarations \
            -Wno-strict-aliasing -fno-exceptions -fno-rtti \
            -Wno-maybe-uninitialized -Wno-volatile \
            -Wno-tautological-compare -Wno-unused-but-set-variable
LDFLAGS := $(KERNEL_ABI_FLAGS) -static -nostdlib \
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
