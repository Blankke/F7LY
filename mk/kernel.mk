# 内核源码组合、编译与链接。

# 叶子目录只取当前层，递归目录则明确列出。构建系统不先收集所有硬件驱动
# 再排除错误实现；通用门面和当前画像驱动都由下面的正向清单组合。
source_files = $(wildcard $(1)/*.c) $(wildcard $(1)/*.cc) \
               $(wildcard $(1)/*.cpp) $(wildcard $(1)/*.S) \
               $(wildcard $(1)/*.s)
recursive_source_files = $(shell find $(1) -type f \
                           \( -name "*.c" -o -name "*.cc" \
                              -o -name "*.cpp" -o -name "*.S" \
                              -o -name "*.s" \))

KERNEL_SRCS := $(foreach dir,$(ARCH_SOURCE_DIRS) $(COMMON_SOURCE_DIRS), \
                 $(call source_files,$(KERNEL_DIR)/$(dir)))
KERNEL_ROOT_SOURCE_DIRS := mem devs trap hal proc boot
KERNEL_SRCS += $(foreach dir,$(KERNEL_ROOT_SOURCE_DIRS), \
                 $(call source_files,$(KERNEL_DIR)/$(dir)))

KERNEL_COMMON_RECURSIVE_DIRS := fs/fat32 fs/lwext4 fs/vfs net/onpstack
KERNEL_SRCS += $(foreach dir,$(KERNEL_COMMON_RECURSIVE_DIRS), \
                 $(call recursive_source_files,$(KERNEL_DIR)/$(dir)))
KERNEL_SRCS += $(call source_files,$(KERNEL_DIR)/fs)
KERNEL_SRCS += $(call source_files,$(KERNEL_DIR)/net)
KERNEL_COMMON_DRIVER_SRCS := kernel/fs/drivers/platform_block.cc \
                             kernel/net/drivers/onps_adapter.cc \
                             kernel/net/drivers/platform_net_device.cc
KERNEL_SRCS += $(KERNEL_COMMON_DRIVER_SRCS)

# 每次只递归收集当前画像目录。平台控制器也归入该目录，因此源码组合
# 完全是正向选择，不需要先收进来再用排除表修正。
KERNEL_SRCS += $(call recursive_source_files,$(PROFILE_DIR))
KERNEL_SRCS += $(PROFILE_SRCS)
KERNEL_SRCS := $(sort $(KERNEL_SRCS))

ifeq ($(VERBOSE_SRCS),1)
  $(info === $(PROFILE) kernel sources ===)
  $(info $(KERNEL_SRCS))
endif

KERNEL_OBJS := $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter %.c,$(KERNEL_SRCS)))
KERNEL_OBJS += $(patsubst $(KERNEL_DIR)/%.cc,$(BUILD_DIR)/%.o,$(filter %.cc,$(KERNEL_SRCS)))
KERNEL_OBJS += $(patsubst $(KERNEL_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(KERNEL_SRCS)))
KERNEL_OBJS += $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/%.o,$(filter %.S,$(KERNEL_SRCS)))
KERNEL_OBJS += $(patsubst $(KERNEL_DIR)/%.s,$(BUILD_DIR)/%.o,$(filter %.s,$(KERNEL_SRCS)))
KERNEL_DEPS := $(KERNEL_OBJS:.o=.d)

ENTRY_OBJ := $(BUILD_DIR)/boot/$(PROFILE_ARCH)/entry.o
KERNEL_OBJS_NO_ENTRY := $(filter-out $(ENTRY_OBJ),$(KERNEL_OBJS))

# Make 默认只比较输入文件时间，不知道编译参数、工具链或链接成员集合是否
# 发生变化。用两个内容稳定的元数据文件补齐这部分依赖：内容不变时保留原
# 时间戳，内容变化时才触发必要的重编译或重链接。
BUILD_CONFIG_STAMP := $(BUILD_DIR)/build-config.stamp
KERNEL_SOURCE_MANIFEST := $(BUILD_DIR)/kernel-sources.list
BUILD_SYSTEM_FILES := Makefile mk/config.mk mk/initcode.mk mk/kernel.mk mk/qemu.mk \
                      mk/perf.mk $(PROFILE_FILE) thirdparty/EASTL/Makefile
BUILD_SYSTEM_HASH = $(shell sha256sum $(BUILD_SYSTEM_FILES) | sha256sum | cut -d' ' -f1)

.PHONY: force-build-metadata

$(BUILD_CONFIG_STAMP): force-build-metadata
	@mkdir -p $(dir $@)
	@tmp="$@.tmp.$$$$"; \
	{ \
		printf '%s\n' 'PROFILE=$(PROFILE)'; \
		printf '%s\n' 'PROFILE_ARCH=$(PROFILE_ARCH)'; \
		printf '%s\n' 'PROFILE_BOARD=$(PROFILE_BOARD)'; \
		printf '%s\n' 'MODE=$(MODE)'; \
		printf '%s\n' 'CC=$(CC)'; \
		printf '%s\n' 'CXX=$(CXX)'; \
		printf '%s\n' 'LD=$(LD)'; \
		printf '%s\n' 'OBJCOPY=$(OBJCOPY)'; \
		printf '%s\n' 'CPPFLAGS=$(CPPFLAGS)'; \
		printf '%s\n' 'CFLAGS=$(CFLAGS)'; \
		printf '%s\n' 'CXXFLAGS=$(CXXFLAGS)'; \
		printf '%s\n' 'INCLUDES=$(INCLUDES)'; \
		printf '%s\n' 'LDFLAGS=$(LDFLAGS)'; \
		printf '%s\n' 'INITCODE_CPPFLAGS=$(INITCODE_CPPFLAGS)'; \
		printf '%s\n' 'INITCODE_CXXFLAGS=$(INITCODE_CXXFLAGS)'; \
		printf '%s\n' 'INITCODE_LDFLAGS=$(INITCODE_LDFLAGS)'; \
		printf '%s\n' 'BUILD_SYSTEM_HASH=$(BUILD_SYSTEM_HASH)'; \
	} > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; else rm -f "$$tmp"; fi

$(KERNEL_SOURCE_MANIFEST): force-build-metadata
	@mkdir -p $(dir $@)
	@tmp="$@.tmp.$$$$"; \
	printf '%s\n' $(KERNEL_SRCS) > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; else rm -f "$$tmp"; fi

# 配置变化需要重编所有相关对象；源码集合变化只需要让最终 ELF 重新链接，
# 已删除源码留下的旧 .o 即使仍在构建目录中，也不会再进入链接命令。
$(KERNEL_OBJS) $(INITCODE_OBJS): $(BUILD_CONFIG_STAMP)

EASTL_LIB := $(BUILD_DIR)/$(EASTL_DIR)/libeastl.a
EASTL_INPUT_ROOTS := $(EASTL_DIR)/source $(EASTL_DIR)/include \
                     $(EASTL_DIR)/test/packages/EABase/include/Common
# 文件内容变化由文件时间戳发现；目录时间戳负责发现文件的新增、删除和改名。
# 两者都保留，才能让顶层可靠地进入 EASTL 子 make，再由 .d 精确重编对象。
EASTL_INPUT_DIRS := $(shell find $(EASTL_INPUT_ROOTS) -type d)
EASTL_INPUTS := $(shell find $(EASTL_INPUT_ROOTS) -type f \
                 \( -name "*.cpp" -o -name "*.h" -o -name "*.inl" \)) \
                 $(EASTL_INPUT_DIRS) $(EASTL_DIR)/Makefile

.PHONY: build-current
build-current: $(KERNEL_BIN)

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.s
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# 用户上下文保存汇编需要访问浮点/向量寄存器，但仍保持内核 soft-float
# 调用 ABI；其它内核对象禁止生成这些指令。
ifeq ($(PROFILE_ARCH),riscv)
$(BUILD_DIR)/mem/riscv/trampoline.o: $(KERNEL_DIR)/mem/riscv/trampoline.S $(BUILD_CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(filter-out -march=% -mabi=%,$(CFLAGS)) \
		$(CONTEXT_ASM_FLAGS) $(INCLUDES) -MMD -MP -c $< -o $@
else ifeq ($(PROFILE_ARCH),loongarch)
$(BUILD_DIR)/trap/loongarch/uservec.o: $(KERNEL_DIR)/trap/loongarch/uservec.S $(BUILD_CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(filter-out -march=% -mabi=% -mfpu=%,$(CFLAGS)) \
		$(CONTEXT_ASM_FLAGS) $(INCLUDES) -MMD -MP -c $< -o $@
endif

# 仅在实际触发误报的翻译单元上放宽 EASTL 模板诊断。
$(BUILD_DIR)/proc/proc_manager.o: CXXFLAGS += -Wno-error=uninitialized -Wno-uninitialized
$(BUILD_DIR)/sys/syscall_handler.o: CXXFLAGS += -Wno-error=uninitialized -Wno-uninitialized
$(BUILD_DIR)/fs/vfs/vfs_utils.o: CXXFLAGS += -Wno-error=uninitialized -Wno-uninitialized

KERNEL_PRELINK := $(BUILD_DIR)/kernel-perf-prelink
PERF_SYMBOL_ASM := $(BUILD_DIR)/perf_symbols.S
PERF_SYMBOL_OBJ := $(BUILD_DIR)/perf_symbols.o

$(KERNEL_ELF): $(ENTRY_OBJ) $(KERNEL_OBJS_NO_ENTRY) $(EASTL_LIB) \
               $(PROFILE_LINK_SCRIPT) $(KERNEL_SOURCE_MANIFEST) \
               scripts/generate_perf_symbols.sh
ifeq ($(PERF_DIAG),1)
	$(LD) $(LDFLAGS) \
		-Wl,--defsym,__f7ly_perf_symbols_start=0 \
		-Wl,--defsym,__f7ly_perf_symbols_end=0 \
		-Wl,--defsym,__f7ly_perf_symbol_names_start=0 \
		-o $(KERNEL_PRELINK) $(ENTRY_OBJ) $(KERNEL_OBJS_NO_ENTRY) $(EASTL_LIB)
	@scripts/generate_perf_symbols.sh generate $(CROSS_COMPILE) \
		$(KERNEL_PRELINK) $(PERF_SYMBOL_ASM)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $(PERF_SYMBOL_ASM) -o $(PERF_SYMBOL_OBJ)
	$(LD) $(LDFLAGS) -o $@ $(ENTRY_OBJ) $(KERNEL_OBJS_NO_ENTRY) \
		$(EASTL_LIB) $(PERF_SYMBOL_OBJ)
	@scripts/generate_perf_symbols.sh verify $(CROSS_COMPILE) $(KERNEL_PRELINK) $@
else
	$(LD) $(LDFLAGS) -o $@ $(ENTRY_OBJ) $(KERNEL_OBJS_NO_ENTRY) $(EASTL_LIB)
endif
	$(SIZE) $@

KERNEL_FP_GATE_STAMP := $(BUILD_DIR)/kernel-no-fp.stamp

.PHONY: check-kernel-no-fp
check-kernel-no-fp: $(KERNEL_FP_GATE_STAMP)

$(KERNEL_FP_GATE_STAMP): $(KERNEL_ELF) $(KERNEL_OBJS) $(EASTL_LIB) \
                         scripts/check_kernel_no_fp.sh
	@scripts/check_kernel_no_fp.sh $(PROFILE_ARCH) $(CROSS_COMPILE) \
		$(KERNEL_ELF) $(KERNEL_OBJS) $(EASTL_LIB)
	@touch $@

$(KERNEL_BIN): $(KERNEL_ELF) $(KERNEL_FP_GATE_STAMP)
	$(OBJCOPY) -R .note.gnu.build-id -R .comment -O binary $< $@

export BUILDPATH := $(BUILD_DIR)
# EASTL 子 make 把配置戳作为每个对象的直接依赖，因此必须先由顶层生成。
# 否则全新画像并行构建时，子 make 只会看到一个尚不存在且无法自行生成的
# 绝对路径，最终把它误报成“对象没有构建规则”。
$(EASTL_LIB): $(EASTL_INPUTS) $(BUILD_CONFIG_STAMP)
	@$(MAKE) -C $(EASTL_DIR) CROSS_COMPILE=$(CROSS_COMPILE) \
		KERNEL_CXXFLAGS='$(CPPFLAGS) $(CXXFLAGS)' \
		KERNEL_CONFIG_STAMP='$(BUILD_CONFIG_STAMP)'

-include $(KERNEL_DEPS) $(INITCODE_DEPS)
