EASTL_DIR := thirdparty/EASTL
# ===== 架构选择 =====
ARCH ?= riscv
KERNEL_PREFIX=`pwd`


ifeq ($(ARCH),riscv)
  CROSS_COMPILE := riscv64-linux-gnu-
  ARCH_CFLAGS := -DRISCV -mcmodel=medany
  OUTPUT_PREFIX := riscv
  QEMU_CMD := qemu-system-riscv64 -machine virt -m 128M -nographic -smp 1 -bios default -hdb ${KERNEL_PREFIX}/sdcard-rv.img -kernel
else ifeq ($(ARCH),loongarch)
  CROSS_COMPILE := loongarch64-unknown-linux-gnu-
  ARCH_CFLAGS := -DLOONGARCH -mcmodel=normal -Wno-error=use-after-free
  OUTPUT_PREFIX := loongarch
  QEMU_CMD := qemu-system-loongarch64 -kernel
else
  $(error 不支持的架构: $(ARCH)，请使用 make riscv 或 make loongarch)
endif

# ===== 工具链配置 =====
CC      := $(CROSS_COMPILE)gcc
CXX     := $(CROSS_COMPILE)g++
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size
OBJDUMP := $(CROSS_COMPILE)objdump

# ===== 路径定义 =====
KERNEL_DIR := kernel
BUILD_DIR := $(shell pwd)/build/$(OUTPUT_PREFIX)
ARCH_DIRS := boot/$(ARCH) hal/$(ARCH) link/$(ARCH) mem/$(ARCH) proc/$(ARCH) trap/$(ARCH) sys/$(ARCH) devs/$(ARCH) fs/$(ARCH) 
COMMON_DIRS := mem libs devs trap hal proc sys  tm
SUBDIRS := $(ARCH_DIRS) $(COMMON_DIRS)

LINK_SCRIPT := $(KERNEL_DIR)/link/$(ARCH)/kernel.ld

CFLAGS := -Wall -Werror -ffreestanding -O2 -fno-builtin -g -fno-stack-protector $(ARCH_CFLAGS)
CXXFLAGS := $(CFLAGS) -std=c++23 -nostdlib \
			-DEA_PLATFORM_LINUX -DEA_PLATFORM_POSIX \
            -DEA_PROCESSOR_RISCV -DEA_ENDIAN_LITTLE=1 \
            -Wno-deprecated-declarations -Wno-strict-aliasing \
            -fno-exceptions -fno-rtti -Wno-maybe-uninitialized

LDFLAGS := -z max-page-size=4096 -nostdlib -T $(LINK_SCRIPT) --gc-sections
INCLUDES := -I$(KERNEL_DIR) $(foreach dir,$(SUBDIRS),-I$(KERNEL_DIR)/$(dir))
INCLUDES += -I$(EASTL_DIR)/include -I$(EASTL_DIR)/include/EASTL -I$(EASTL_DIR)/test/packages/EABase/include/Common

# ===== 文件收集规则 =====
SRCS := $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.[csS])) \
        $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.cpp)) \
        $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.cc)) 
SRCS += $(shell find $(KERNEL_DIR)/fs -type f \
\( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))
$(info === SRCS collected ===)
$(info $(SRCS))

OBJS := $(patsubst $(KERNEL_DIR)/%.c,   $(BUILD_DIR)/%.o, $(filter %.c,   $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.cc,  $(BUILD_DIR)/%.o, $(filter %.cc,  $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(filter %.cpp, $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.S,   $(BUILD_DIR)/%.o, $(filter %.S,   $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.s,   $(BUILD_DIR)/%.o, $(filter %.s,   $(SRCS)))

ENTRY_OBJ := $(BUILD_DIR)/boot/$(ARCH)/entry.o
OBJS_NO_ENTRY := $(filter-out $(ENTRY_OBJ), $(OBJS))
DEPS := $(OBJS:.o=.d)

# ===== 输出目标 =====
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin

# ===== initcode 用户进程编译相关 =====
# 支持 riscv 和 loongarch 架构，自动选择交叉工具链和参数

ifeq ($(ARCH),riscv)
  INITCODE_SRC := user/app/initcode-rv.cc
else ifeq ($(ARCH),loongarch)
  INITCODE_SRC := user/app/initcode-la.cc
endif
INITCODE_OBJ := build/$(OUTPUT_PREFIX)/initcode.o
INITCODE_ELF := build/$(OUTPUT_PREFIX)/initcode.elf

# 根据架构选择不同的输出文件名
ifeq ($(ARCH),riscv)
  INITCODE_BIN := user/initcode-rv
else ifeq ($(ARCH),loongarch)
  INITCODE_BIN := user/initcode-la
endif

# 编译参数
INITCODE_CFLAGS := -Wall -O -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector -nostdlib -ffreestanding $(ARCH_CFLAGS) -Iuser/deps -Iuser/syscall_lib -Iuser/syscall_lib/arch/$(ARCH) -Ikernel/sys -Ikernel
INITCODE_LDFLAGS := -N -e start -Ttext 0

# ===== 用户态代码统一编译变量 =====
USER_BUILD_DIR := build/$(OUTPUT_PREFIX)

# 自动收集所有用户源文件
USER_SRCS := $(shell find user -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' \))
USER_OBJS := $(patsubst user/%.c,   $(USER_BUILD_DIR)/%.o, $(filter %.c,   $(USER_SRCS)))
USER_OBJS += $(patsubst user/%.cc,  $(USER_BUILD_DIR)/%.o, $(filter %.cc,  $(USER_SRCS)))
USER_OBJS += $(patsubst user/%.cpp, $(USER_BUILD_DIR)/%.o, $(filter %.cpp, $(USER_SRCS)))

USER_DEPS := $(USER_OBJS:.o=.d)
USER_INCLUDES := -Iuser/deps -Iuser/syscall_lib -Iuser/syscall_lib/arch/$(ARCH) -Ikernel/sys -Ikernel
USER_CFLAGS := -Wall -O -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector -nostdlib -ffreestanding $(ARCH_CFLAGS) $(USER_INCLUDES)

# 添加用户目录创建
USER_DIRS := $(sort $(dir $(USER_OBJS)))

.PHONY: all clean dirs build riscv loongarch run debug initcode user-dirs debug-user-files

all: riscv

riscv:
	@$(MAKE) ARCH=riscv build

loongarch:
	@$(MAKE) ARCH=loongarch build

# 修改 build 依赖，确保用户代码在内核编译之前完成
build: user-dirs initcode $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a $(KERNEL_BIN)

# 修改 dirs 目标，包含用户目录
dirs: user-dirs
	@mkdir -p $(BUILD_DIR)
	@for dir in $(SUBDIRS); do mkdir -p $(BUILD_DIR)/$$dir; done

user-dirs:
	@mkdir -p $(USER_DIRS)

# 内核编译规则
$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/boot/$(ARCH)/initcode.o: $(INITCODE_BIN)

# 用户代码编译规则 - 支持所有文件类型
$(USER_BUILD_DIR)/%.o: user/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling user C file: $<"
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(USER_BUILD_DIR)/%.o: user/%.cc
	@mkdir -p $(dir $@)
	@echo "Compiling user C++ file: $<"
	$(CXX) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(USER_BUILD_DIR)/%.o: user/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling user C++ file: $<"
	$(CXX) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(KERNEL_ELF): $(ENTRY_OBJ) $(OBJS_NO_ENTRY) $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a
	$(LD) $(LDFLAGS) -o $@ $(ENTRY_OBJ) $(OBJS_NO_ENTRY) $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a
	$(SIZE) $@
	# $(OBJDUMP) -D $@ > kernel.asm

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

export BUILDPATH := $(BUILD_DIR)
$(BUILD_DIR)/$(EASTL_DIR)/libeastl.a:
	@$(MAKE) -C $(EASTL_DIR) CROSS_COMPILE=$(CROSS_COMPILE)

run: build initcode
ifeq ($(ARCH),riscv)
	$(MAKE) run-riscv
else ifeq ($(ARCH),loongarch)
	$(MAKE) run-loongarch
else
	$(error Unsupported ARCH=$(ARCH))
endif

run-riscv:
	qemu-system-riscv64 \
		-machine virt \
		-kernel $(KERNEL_ELF) \
		-m 1G \
		-nographic \
		-smp 1 \
		-bios default \
		-drive file=$(KERNEL_PREFIX)/sdcard-rv.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-no-reboot \
		-device virtio-net-device,netdev=net \
		-netdev user,id=net \
		-rtc base=utc

run-loongarch:
	$(QEMU_CMD) $(KERNEL_ELF) -m 128M -nographic -smp 1

debug: build
	@if [ "$(ARCH)" = "riscv" ]; then \
	$(MAKE) debug-riscv; \
	elif [ "$(ARCH)" = "loongarch" ]; then \
		$(QEMU_CMD) $(KERNEL_ELF) -S -gdb tcp::1234; \
	fi

debug-riscv:
	qemu-system-riscv64 \
		-machine virt \
		-kernel $(KERNEL_ELF) \
		-m 1G \
		-nographic \
		-smp 1 \
		-bios default \
		-drive file=$(KERNEL_PREFIX)/sdcard-rv.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-no-reboot \
		-device virtio-net-device,netdev=net \
		-netdev user,id=net \
		-rtc base=utc \
		-S -gdb tcp::1234;

# initcode 相关规则
initcode: $(INITCODE_BIN)

$(INITCODE_ELF): $(USER_OBJS)
	@echo "Linking initcode with objects: $(USER_OBJS)"
	$(LD) $(INITCODE_LDFLAGS) -o $@ $^

$(INITCODE_BIN): $(INITCODE_ELF)
	$(OBJCOPY) -S -O binary $< $@
	riscv64-unknown-elf-objdump -D -b binary -m riscv:rv64 -EL $@ > user/disasm_initcode.asm
