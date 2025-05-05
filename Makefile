# 简洁可靠的 RISC-V 操作系统 Makefile，保证 _entry 地址正确

# ===== 工具链配置 =====
CROSS_COMPILE := riscv64-unknown-elf-
CC      := $(CROSS_COMPILE)gcc
CXX     := $(CROSS_COMPILE)g++
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size

# ===== 编译与链接选项 =====
CFLAGS   := -Wall -Werror -ffreestanding -O2 -mcmodel=medany -fno-builtin -g -fno-stack-protector
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS  := -z max-page-size=4096 -nostdlib -T $(LINK_SCRIPT) --gc-sections
CXXFLAGS += -DRISCV
# ===== 目录结构定义 =====
KERNEL_DIR := kernel
BUILD_DIR := build
LINK_SCRIPT := $(KERNEL_DIR)/link/kernel.ld
SUBDIRS := boot hal mem libs devs
INCLUDES := -I$(KERNEL_DIR) $(foreach dir,$(SUBDIRS),-I$(KERNEL_DIR)/$(dir))

# ===== 手动指定 entry.o 放首位，确保链接顺序正确 =====
ENTRY_OBJ := $(BUILD_DIR)/boot/entry.o

# ===== 自动搜集源码文件并生成中间目标文件名 =====
SRCS := $(shell find $(KERNEL_DIR) -type f \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.S" -o -name "*.s" \))
OBJS := $(patsubst $(KERNEL_DIR)/%.c,   $(BUILD_DIR)/%.o, $(filter %.c,   $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.cc,  $(BUILD_DIR)/%.o, $(filter %.cc,  $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(filter %.cpp, $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.S,   $(BUILD_DIR)/%.o, $(filter %.S,   $(SRCS)))
OBJS += $(patsubst $(KERNEL_DIR)/%.s,   $(BUILD_DIR)/%.o, $(filter %.s,   $(SRCS)))

# 去除 entry.o，避免重复链接，手动加在链接时第一位
OBJS_NO_ENTRY := $(filter-out $(ENTRY_OBJ), $(OBJS))

DEPS := $(OBJS:.o=.d)

# ===== 目标文件定义 =====
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin

.PHONY: all clean run debug dirs

all: dirs $(KERNEL_BIN)

dirs:
	@mkdir -p $(BUILD_DIR)
	@for dir in $(SUBDIRS); do mkdir -p $(BUILD_DIR)/$$dir; done
	@mkdir -p $(BUILD_DIR)/link

# ===== 编译规则 =====
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

# ===== 链接顺序显式控制：entry.o 放最前 =====
$(KERNEL_ELF): $(ENTRY_OBJ) $(OBJS_NO_ENTRY)
	$(LD) -z max-page-size=4096 -nostdlib -T $(LINK_SCRIPT) --gc-sections -o $@ $^
	$(SIZE) $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# ===== QEMU 启动选项 =====
run: all
	qemu-system-riscv64 -machine virt -m 128M -nographic -smp 1 -bios default -kernel $(KERNEL_ELF)

debug: all
	qemu-system-riscv64 -machine virt -m 128M -nographic -smp 1 -bios default -kernel $(KERNEL_ELF) -S -gdb tcp::1234

# ===== 清理规则 =====
clean:
	rm -rf $(BUILD_DIR)
	find . -name "*.o" -exec rm -f {} \;
-include $(DEPS)
