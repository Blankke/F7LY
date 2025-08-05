EASTL_DIR := thirdparty/EASTL
# ===== 架构选择 =====
ARCH ?= riscv
KERNEL_PREFIX=`pwd`
DIS_PRINTF ?= 0

# 检查是否通过目标名称指定架构
ifneq (,$(filter l loongarch,$(MAKECMDGOALS)))
  ARCH := loongarch
endif
ifneq (,$(filter r riscv,$(MAKECMDGOALS)))
  ARCH := riscv
endif

# 架构别名目标（这些目标不执行任何操作，仅用于设置 ARCH 变量）
r riscv l loongarch:
    @:


ifeq ($(ARCH),riscv)
  CROSS_COMPILE := riscv64-linux-gnu-
  ARCH_CFLAGS := -DRISCV -mcmodel=medany
  OUTPUT_PREFIX := riscv
  QEMU_CMD := qemu-system-riscv64 -machine virt -m 128M -nographic -smp 1 -bios default -hdb ${KERNEL_PREFIX}/sdcard-rv.img -kernel
else ifeq ($(ARCH),loongarch)
  CROSS_COMPILE := loongarch64-linux-gnu-
  ARCH_CFLAGS := -DLOONGARCH -mcmodel=normal -Wno-error=use-after-free
  OUTPUT_PREFIX := loongarch
  QEMU_CMD := qemu-system-loongarch64 -machine virt -cpu la464-loongarch-cpu
else
  $(error 不支持的架构: $(ARCH)，请使用 make riscv 或 make loongarch)
endif

ifeq ($(DIS_PRINTF),1)
  ARCH_CFLAGS += -DDIS_PRINTF
endif

# ===== 工具链配置 =====
CC      := $(CROSS_COMPILE)gcc
CXX     := $(CROSS_COMPILE)g++
LD      := $(CROSS_COMPILE)g++
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size
OBJDUMP := $(CROSS_COMPILE)objdump

# ===== 路径定义 =====
KERNEL_DIR := kernel
BUILD_DIR := $(shell pwd)/build/$(OUTPUT_PREFIX)
# 有架构特定子目录的文件夹
ARCH_DIRS := boot/$(ARCH) hal/$(ARCH) link/$(ARCH) mem/$(ARCH) proc/$(ARCH) trap/$(ARCH) devs/$(ARCH)
# 只有通用文件的文件夹
COMMON_DIRS := libs tm sys shm
SUBDIRS := $(ARCH_DIRS) $(COMMON_DIRS)

LINK_SCRIPT := $(KERNEL_DIR)/link/$(ARCH)/kernel.ld

CFLAGS := -Wall -Werror -ffreestanding -O2 -fno-builtin -g -fno-stack-protector $(ARCH_CFLAGS)
ifeq ($(ARCH),riscv)
  EA_PLATFORM := -DEA_PROCESSOR_RISCV
else ifeq ($(ARCH),loongarch)
  EA_PLATFORM := -DEA_PROCESSOR_LOONGARCH64
endif
CXXFLAGS := $(CFLAGS) -std=c++23 -nostdlib \
			-DEA_PLATFORM_LINUX -DEA_PLATFORM_POSIX \
            $(EA_PLATFORM) -DEA_ENDIAN_LITTLE=1 \
            -Wno-deprecated-declarations -Wno-strict-aliasing \
            -fno-exceptions -fno-rtti -Wno-maybe-uninitialized \
			-Wno-volatile

LDFLAGS := -static -nostdlib -nostartfiles -nodefaultlibs -Wl,-z,max-page-size=4096 -Wl,-T,$(LINK_SCRIPT) -Wl,--gc-sections
# 包含头文件路径：架构特定目录 + 通用目录 + 有架构子目录的文件夹根目录
INCLUDES := -I$(KERNEL_DIR) $(foreach dir,$(SUBDIRS),-I$(KERNEL_DIR)/$(dir))
INCLUDES += -I$(KERNEL_DIR)/mem -I$(KERNEL_DIR)/devs -I$(KERNEL_DIR)/trap -I$(KERNEL_DIR)/hal -I$(KERNEL_DIR)/proc -I$(KERNEL_DIR)/boot
INCLUDES += -I$(KERNEL_DIR)/fs -I$(KERNEL_DIR)/net
INCLUDES += -I$(KERNEL_DIR)/net/onpstack/include
INCLUDES += -I$(EASTL_DIR)/include -I$(EASTL_DIR)/include/EASTL -I$(EASTL_DIR)/test/packages/EABase/include/Common
INCLUDES += -I$(KERNEL_DIR)/fs
# ===== 文件收集规则 =====
# 收集架构特定目录和通用目录的源文件
SRCS := $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.[csS])) \
        $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.cpp)) \
        $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.cc))

# 收集有架构子目录的文件夹中的通用文件（排除架构特定子目录）
SRCS += $(shell find $(KERNEL_DIR)/mem -maxdepth 1 -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))
SRCS += $(shell find $(KERNEL_DIR)/devs -maxdepth 1 -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))
SRCS += $(shell find $(KERNEL_DIR)/trap -maxdepth 1 -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))
SRCS += $(shell find $(KERNEL_DIR)/hal -maxdepth 1 -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))
SRCS += $(shell find $(KERNEL_DIR)/proc -maxdepth 1 -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))
SRCS += $(shell find $(KERNEL_DIR)/boot -maxdepth 1 -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))

# 收集 fs 目录中的所有文件（fs 没有架构特定子目录）
SRCS += $(shell find $(KERNEL_DIR)/fs -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))

# 收集 net 目录中的所有文件（net 没有架构特定子目录）
SRCS += $(shell find $(KERNEL_DIR)/net -type f \
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
ifeq ($(ARCH),riscv)
  KERNEL_ELF := kernel-rv
  KERNEL_BIN := build/$(OUTPUT_PREFIX)/kernel-rv.bin
else ifeq ($(ARCH),loongarch)
  KERNEL_ELF := kernel-la
  KERNEL_BIN := build/$(OUTPUT_PREFIX)/kernel-la.bin
endif

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

# 新增 syscall 编译规则
SYSCALL_SRC := user/syscall_lib/syscall.cc
SYSCALL_OBJ := build/$(OUTPUT_PREFIX)/syscall.o

# 新增 printf 编译规则
PRINTF_SRC := user/syscall_lib/printf.cc
PRINTF_OBJ := build/$(OUTPUT_PREFIX)/printf.o



USER_TEST_SRC := user/user_lib/user_test.cc
USER_TEST_OBJ := build/$(OUTPUT_PREFIX)/user_test.o

# 编译参数

INITCODE_CFLAGS := -Wall -O -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector -nostdlib -ffreestanding $(ARCH_CFLAGS) -Iuser/deps -Iuser/syscall_lib -Iuser/syscall_lib/arch/$(ARCH) -Ikernel/sys -Ikernel
ifeq ($(ARCH),riscv)
INITCODE_LDFLAGS := -static -nostdlib -e main -nodefaultlibs -static -Wl,--no-dynamic-linker,-T,user/user-riscv.ld
else ifeq ($(ARCH),loongarch)
INITCODE_LDFLAGS := -static -nostdlib -e main -nodefaultlibs -static -Wl,--no-dynamic-linker,-T,user/user-loongarch.ld
endif
.PHONY: all clean dirs build riscv loongarch run debug initcode build-la


all: loongarch riscv

riscv:
	@$(MAKE) ARCH=riscv build

loongarch:
	@$(MAKE) ARCH=loongarch build-la

build: initcode dirs $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a $(KERNEL_BIN)
build-la: dirs $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a $(KERNEL_BIN)


dirs:
	@mkdir -p $(BUILD_DIR)
	@for dir in $(SUBDIRS); do mkdir -p $(BUILD_DIR)/$$dir; done
	@mkdir -p $(BUILD_DIR)/fs $(BUILD_DIR)/net
	@find $(KERNEL_DIR)/fs -type d | sed 's|$(KERNEL_DIR)/|$(BUILD_DIR)/|' | xargs mkdir -p
	@find $(KERNEL_DIR)/net -type d | sed 's|$(KERNEL_DIR)/|$(BUILD_DIR)/|' | xargs mkdir -p

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

$(KERNEL_ELF): $(ENTRY_OBJ) $(OBJS_NO_ENTRY) $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a
	$(LD) $(LDFLAGS) -o $@ $(ENTRY_OBJ) $(OBJS_NO_ENTRY) $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a
	$(SIZE) $@
# 	$(OBJDUMP) -D $@ > kernel.asm
# 	riscv64-linux-gnu-objdump -D build/riscv/kernel.elf > kernel.asm

# 只有 riscv 架构需要依赖 initcode

$(KERNEL_ELF): $(INITCODE_BIN)


$(KERNEL_BIN): $(KERNEL_ELF) 
	$(OBJCOPY) -O binary $< $@

export BUILDPATH := $(BUILD_DIR)
$(BUILD_DIR)/$(EASTL_DIR)/libeastl.a:
	@$(MAKE) -C $(EASTL_DIR) CROSS_COMPILE=$(CROSS_COMPILE)


run: build
ifeq ($(ARCH),riscv)
	$(MAKE) run-riscv ARCH=$(ARCH)
else ifeq ($(ARCH),loongarch)
	$(MAKE) run-loongarch ARCH=$(ARCH)
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
		-drive file=$(KERNEL_PREFIX)/sdcard-rv-final.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-no-reboot \
		-device virtio-net-device,netdev=net \
		-netdev user,id=net \
		-rtc base=utc


run-loongarch:
	qemu-system-loongarch64 \
	    -machine virt \
	    -kernel $(KERNEL_ELF) \
	    -m 1G \
	    -nographic \
	    -smp 1 \
		-drive file=$(KERNEL_PREFIX)/sdcard-la-final.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-no-reboot \
		-rtc base=utc \




debug: build
	@if [ "$(ARCH)" = "riscv" ]; then \
	$(MAKE) debug-riscv ARCH=$(ARCH);\
	elif [ "$(ARCH)" = "loongarch" ]; then \
		$(MAKE) debug-loongarch ARCH=$(ARCH); \
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

debug-loongarch:
	qemu-system-loongarch64 \
	    -machine virt \
	    -kernel $(KERNEL_ELF) \
	    -m 1G \
	    -nographic \
	    -smp 1 \
		-drive file=$(KERNEL_PREFIX)/sdcard-la.img,if=none,format=raw,id=x0 \
		-device virtio-blk-pci,drive=x0 \
		-no-reboot \
		-rtc base=utc \
	    -S -gdb tcp::1234;


initcode: $(INITCODE_BIN)

# 编译 initcode 源文件为目标文件
$(INITCODE_OBJ): $(INITCODE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(INITCODE_CFLAGS) -c $< -o $@
	
# initcode.o 显式依赖 initcode-rv 文件
$(BUILD_DIR)/boot/$(ARCH)/initcode.o: $(INITCODE_BIN)

# 编译 syscall.o
$(SYSCALL_OBJ): $(SYSCALL_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(INITCODE_CFLAGS) -c $< -o $@

# 编译 printf.o
$(PRINTF_OBJ): $(PRINTF_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(INITCODE_CFLAGS) -c $< -o $@

# 编译 user_test.o
$(USER_TEST_OBJ): $(USER_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(INITCODE_CFLAGS) -c $< -o $@

# 链接生成 initcode.elf
$(INITCODE_ELF): $(INITCODE_OBJ) $(SYSCALL_OBJ) $(PRINTF_OBJ) $(USER_TEST_OBJ)
	$(LD) $(INITCODE_LDFLAGS) -o $@ $^

ifeq ($(ARCH),riscv)
  OBJDUMP_INITCODE := riscv64-unknown-elf-objdump -D -b binary -m riscv:rv64 -EL
else ifeq ($(ARCH),loongarch)
  OBJDUMP_INITCODE := loongarch64-linux-gnu-objdump -D -b binary -m loongarch64
endif

# 生成二进制 initcode 文件 + 反汇编
$(INITCODE_BIN): $(INITCODE_ELF)
	$(OBJCOPY) -S -O binary $< $@
	# $(OBJDUMP_INITCODE) $@ > user/disasm_initcode.asm


clean:
	rm -rf build
	find . -name "*.o" -o -name "*.d" -exec rm -f {} \;
	$(MAKE) clean -C thirdparty/EASTL
	rm -f user/initcode-*
	rm -f user/disasm_initcode.asm, kernel.asm


-include $(DEPS)