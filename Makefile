EASTL_DIR := thirdparty/EASTL
# ===== 架构选择 =====
ARCH ?= riscv

ifeq ($(ARCH),riscv)
  CROSS_COMPILE := riscv64-unknown-elf-
  ARCH_CFLAGS := -DRISCV -mcmodel=medany
  OUTPUT_PREFIX := riscv
  QEMU_CMD := qemu-system-riscv64 -machine virt -m 128M -nographic -smp 1 -bios default -kernel
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

# ===== 路径定义 =====
KERNEL_DIR := kernel
BUILD_DIR := build/$(OUTPUT_PREFIX)
ARCH_DIRS := boot/$(ARCH) hal/$(ARCH) link/$(ARCH) mem/$(ARCH) proc/$(ARCH) trap/$(ARCH) sys/$(ARCH) devs/$(ARCH)
COMMON_DIRS := mem libs devs trap hal proc sys 
SUBDIRS := $(ARCH_DIRS) $(COMMON_DIRS)

LINK_SCRIPT := $(KERNEL_DIR)/link/$(ARCH)/kernel.ld
CFLAGS := -Wall -Werror -ffreestanding -O2 -fno-builtin -g -fno-stack-protector $(ARCH_CFLAGS)
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS := -z max-page-size=4096 -nostdlib -T $(LINK_SCRIPT) --gc-sections
INCLUDES := -I$(KERNEL_DIR) $(foreach dir,$(SUBDIRS),-I$(KERNEL_DIR)/$(dir))
INCLUDES += -I$(EASTL_DIR)/include -I$(EASTL_DIR)/include/EASTL

# ===== 文件收集规则 =====
SRCS := $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.[csS])) \
        $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.cpp)) \
        $(foreach dir,$(SUBDIRS),$(wildcard $(KERNEL_DIR)/$(dir)/*.cc))

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

.PHONY: all clean dirs build riscv loongarch run debug

all: riscv

riscv:
	@$(MAKE) ARCH=riscv build

loongarch:
	@$(MAKE) ARCH=loongarch build

build: dirs $(EASTL_DIR)/libeastl.a $(KERNEL_BIN)

dirs:
	@mkdir -p $(BUILD_DIR)
	@for dir in $(SUBDIRS); do mkdir -p $(BUILD_DIR)/$$dir; done

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

$(KERNEL_ELF): $(ENTRY_OBJ) $(OBJS_NO_ENTRY) $(EASTL_DIR)/libeastl.a
	$(LD) $(LDFLAGS) -o $@ $(ENTRY_OBJ) $(OBJS_NO_ENTRY) $(EASTL_DIR)/libeastl.a
	$(SIZE) $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

export BUILDPATH := $(BUILD_DIR)
$(EASTL_DIR)/libeastl.a:
	@$(MAKE) -C $(EASTL_DIR)

run: build
	@if [ "$(ARCH)" = "riscv" ]; then \
		$(QEMU_CMD) $(KERNEL_ELF); \
	elif [ "$(ARCH)" = "loongarch" ]; then \
		$(QEMU_CMD) $(KERNEL_ELF) -m 128M -nographic -smp 1; \
	fi
	# 以下是LoongArch可选参数（已注释）：
	# -drive file=rootfs.img,if=none,format=raw,id=x0
	# -device virtio-blk-pci,drive=x0,bus=virtio-mmio-bus.0
	# -no-reboot
	# -device virtio-net-pci,netdev=net0
	# -netdev user,id=net0,hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
	# -rtc base=utc
	# -drive file=disk-la.img,if=none,format=raw,id=x1
	# -device virtio-blk-pci,drive=x1,bus=virtio-mmio-bus.1

debug: build
	@if [ "$(ARCH)" = "riscv" ]; then \
		$(QEMU_CMD) $(KERNEL_ELF) -S -gdb tcp::1234; \
	elif [ "$(ARCH)" = "loongarch" ]; then \
		$(QEMU_CMD) $(KERNEL_ELF) -S -gdb tcp::1234; \
	fi

clean:
	rm -rf build
	find . -name "*.o" -o -name "*.d" -exec rm -f {} \;
	$(MAKE) clean -C thirdparty/EASTL

-include $(DEPS)
