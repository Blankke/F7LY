# 内嵌用户态入口构建。
#
# evaluation 与 shell 只在入口源文件和是否链接回归调度器上不同，其余编译
# 流程共用一套规则。二进制属于当前画像，不能让同架构的多块板共享输出。

ifeq ($(PROFILE_ARCH),riscv)
  INITCODE_LINK_SCRIPT := user/user-riscv.ld
  INITCODE_EVALUATION_SRC := user/app/initcode-rv.cc
else
  INITCODE_LINK_SCRIPT := user/user-loongarch.ld
  INITCODE_EVALUATION_SRC := user/app/initcode-la.cc
endif

ifeq ($(MODE),shell)
  INITCODE_ENTRY_SRC := user/app/shell.cc
else
  INITCODE_ENTRY_SRC := $(INITCODE_EVALUATION_SRC)
endif

INITCODE_COMMON_SRCS := user/syscall_lib/syscall.cc \
                        user/syscall_lib/printf.cc \
                        user/user_lib/fuckyou.cc
INITCODE_SRCS := $(INITCODE_ENTRY_SRC) $(INITCODE_COMMON_SRCS)
ifeq ($(MODE),evaluation)
  INITCODE_SRCS += user/user_lib/user_test.cc
endif

INITCODE_OBJS := $(patsubst user/%.cc,$(BUILD_DIR)/user/%.o,$(INITCODE_SRCS))
INITCODE_DEPS := $(INITCODE_OBJS:.o=.d)
INITCODE_ELF := $(BUILD_DIR)/user/initcode.elf
INITCODE_BIN := $(BUILD_DIR)/user/initcode.bin

INITCODE_CPPFLAGS := $(ARCH_DEFINES)
INITCODE_CXXFLAGS := -Wall -O -fno-builtin -fno-exceptions -fno-rtti \
                     -fno-stack-protector -nostdlib -ffreestanding \
                     $(ARCH_FLAGS) -Iuser/deps -Iuser/syscall_lib \
                     -Iuser/syscall_lib/arch/$(PROFILE_ARCH) -Ikernel/sys -Ikernel
INITCODE_LDFLAGS := -static -nostdlib -e main -nodefaultlibs \
                    -Wl,--no-dynamic-linker,-T,$(INITCODE_LINK_SCRIPT)

.PHONY: initcode
initcode: $(INITCODE_BIN)

$(BUILD_DIR)/user/%.o: user/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(INITCODE_CPPFLAGS) $(INITCODE_CXXFLAGS) -MMD -MP -c $< -o $@

$(INITCODE_ELF): $(INITCODE_OBJS) $(INITCODE_LINK_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(INITCODE_LDFLAGS) -o $@ $(INITCODE_OBJS)

$(INITCODE_BIN): $(INITCODE_ELF)
	$(OBJCOPY) -S -R .note.gnu.build-id -R .note.GNU-stack -R .comment \
		-O binary $< $@

# initcode.S 只在这个目标上接收嵌入文件路径，避免把内部构建路径泄漏到
# 所有内核 C/C++ 编译单元。绝对路径也不会受汇编器当前目录影响。
KERNEL_INITCODE_OBJ := $(BUILD_DIR)/boot/$(PROFILE_ARCH)/initcode.o
$(KERNEL_INITCODE_OBJ): CPPFLAGS += -DINITCODE_BIN_PATH=\"$(abspath $(INITCODE_BIN))\"
$(KERNEL_INITCODE_OBJ): $(INITCODE_BIN)
