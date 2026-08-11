# QEMU 运行、交互 shell 与调试入口。
# 物理板画像只能构建产物，所有 QEMU 入口都会先检查画像内部的平台类型。

IMAGE_DIR := $(PROJECT_ROOT)/images
RISCV_PRELIMINARY_IMAGE ?= $(IMAGE_DIR)/oscomp-preliminary-riscv64.img
LOONGARCH_PRELIMINARY_IMAGE ?= $(IMAGE_DIR)/oscomp-preliminary-loongarch64.img
RISCV_FINAL_IMAGE ?= $(IMAGE_DIR)/oscomp-final-riscv64.img
LOONGARCH_FINAL_IMAGE ?= $(IMAGE_DIR)/oscomp-final-loongarch64.img
QEMU_IMAGE_PREPARE := $(PROJECT_ROOT)/scripts/images/prepare-qemu-image.sh

# 磁盘套件与内嵌用户程序模式是两个独立维度。run 默认使用初赛盘，决赛
# 测试显式传 QEMU_DISK=final；禁止再通过覆盖同名文件来偷偷切换测试集合。
QEMU_DISK ?= preliminary

QEMU_MEM ?= 8G
QEMU_DEBUG_MEM ?= 8G
QEMU_SMP ?= 8
QEMU_ACCEL ?= -accel tcg,thread=multi
QEMU_RUN_SNAPSHOT ?= -snapshot
QEMU_SHELL_SNAPSHOT ?=
QEMU_DEBUG_SNAPSHOT ?= -snapshot
QEMU_SNAPSHOT ?=

ifeq ($(PROFILE_ARCH),riscv)
  QEMU_PRELIMINARY_IMAGE := $(RISCV_PRELIMINARY_IMAGE)
  QEMU_FINAL_IMAGE := $(RISCV_FINAL_IMAGE)
  QEMU_IMAGE_ARCH := riscv
  QEMU_BLOCK_DEVICE_ARGS := -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
else
  QEMU_PRELIMINARY_IMAGE := $(LOONGARCH_PRELIMINARY_IMAGE)
  QEMU_FINAL_IMAGE := $(LOONGARCH_FINAL_IMAGE)
  QEMU_IMAGE_ARCH := loongarch
  QEMU_BLOCK_DEVICE_ARGS := -device virtio-blk-pci,drive=x0
endif

ifeq ($(QEMU_DISK),preliminary)
  QEMU_DEFAULT_STORAGE_IMAGE := $(QEMU_PRELIMINARY_IMAGE)
  QEMU_IMAGE_KIND := $(QEMU_IMAGE_ARCH)-preliminary
else ifeq ($(QEMU_DISK),final)
  QEMU_DEFAULT_STORAGE_IMAGE := $(QEMU_FINAL_IMAGE)
  QEMU_IMAGE_KIND := $(QEMU_IMAGE_ARCH)-final
else
  $(error 不支持的 QEMU_DISK=$(QEMU_DISK)，请使用 preliminary 或 final)
endif
QEMU_STORAGE_IMAGE ?= $(QEMU_DEFAULT_STORAGE_IMAGE)

ifeq ($(MODE),shell)
  QEMU_CONSOLE_ARGS ?= -display none -chardev stdio,id=shell_stdio,signal=off \
                       -serial chardev:shell_stdio -monitor none
else
  QEMU_CONSOLE_ARGS ?= -nographic
endif

QEMU_STORAGE_ARGS = -drive file=$(QEMU_STORAGE_IMAGE),if=none,format=raw,id=x0 \
                    $(QEMU_BLOCK_DEVICE_ARGS)

# 架构相关 QEMU 参数只在这里选择一次，运行和调试共用同一份机器描述。
ifeq ($(PROFILE_ARCH),riscv)
  QEMU_SYSTEM := qemu-system-riscv64
  QEMU_FIRMWARE_ARGS := -bios default
  QEMU_NETWORK_ARGS := -device virtio-net-device,netdev=net -netdev user,id=net
else
  QEMU_SYSTEM := qemu-system-loongarch64
  QEMU_FIRMWARE_ARGS :=
  QEMU_NETWORK_ARGS := -netdev user,id=net -device virtio-net-pci,netdev=net
endif

QEMU_COMMON_TAIL = $(QEMU_ACCEL) -smp $(QEMU_SMP) $(QEMU_FIRMWARE_ARGS) \
                   $(QEMU_SNAPSHOT) $(QEMU_STORAGE_ARGS) -no-reboot \
                   $(QEMU_NETWORK_ARGS) -rtc base=utc

.PHONY: prepare-image

# 只有本地 QEMU 入口依赖磁盘准备；build/build-current/all 永远不检查镜像，
# 因此纯编译环境不需要网络，也不需要存在 images/。
prepare-image:
	@if [ "$(PROFILE_BOARD)" != "qemu" ]; then \
		echo "错误：prepare-image 只接受 QEMU 画像"; exit 2; \
	fi
	@$(QEMU_IMAGE_PREPARE) "$(QEMU_IMAGE_KIND)" "$(QEMU_STORAGE_IMAGE)"

run:
	@if [ "$(PROFILE_BOARD)" != "qemu" ]; then \
		echo "错误：PROFILE=$(PROFILE) 是物理板画像，禁止进入 QEMU run"; exit 2; \
	fi
	@if [ "$(MODE)" != "evaluation" ]; then \
		echo "错误：run 固定运行评测模式；交互终端请使用 make shell PROFILE=$(PROFILE)"; exit 2; \
	fi
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=$(PROFILE) MODE=evaluation build-current
	@$(MAKE) PROFILE=$(PROFILE) MODE=evaluation \
		QEMU_DISK=$(QEMU_DISK) \
		QEMU_SNAPSHOT="$(QEMU_RUN_SNAPSHOT)" qemu-run

shell:
	@if [ "$(PROFILE_BOARD)" != "qemu" ]; then \
		echo "错误：PROFILE=$(PROFILE) 是物理板画像，不能进入 QEMU shell"; exit 2; \
	fi
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=$(PROFILE) MODE=shell build-current
	@$(MAKE) PROFILE=$(PROFILE) MODE=shell \
		QEMU_DISK=final \
		QEMU_SNAPSHOT="$(QEMU_SHELL_SNAPSHOT)" qemu-run

debug:
	@if [ "$(PROFILE_BOARD)" != "qemu" ]; then \
		echo "错误：PROFILE=$(PROFILE) 是物理板画像，禁止进入 QEMU debug"; exit 2; \
	fi
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=$(PROFILE) MODE=$(MODE) build-current
	@$(MAKE) PROFILE=$(PROFILE) MODE=$(MODE) \
		QEMU_DISK=$(QEMU_DISK) \
		QEMU_SNAPSHOT="$(QEMU_DEBUG_SNAPSHOT)" qemu-debug

# qemu-run/qemu-debug 是脚本可复用的低层入口：它们只运行指定产物，不构建。
qemu-run: prepare-image
	@if [ "$(PROFILE_BOARD)" != "qemu" ]; then \
		echo "错误：qemu-run 只接受 QEMU 画像"; exit 2; \
	fi
	$(QEMU_SYSTEM) -machine virt -kernel $(KERNEL_ELF) -m $(QEMU_MEM) \
		$(QEMU_COMMON_TAIL) $(QEMU_CONSOLE_ARGS)

qemu-debug: prepare-image
	@if [ "$(PROFILE_BOARD)" != "qemu" ]; then \
		echo "错误：qemu-debug 只接受 QEMU 画像"; exit 2; \
	fi
	$(QEMU_SYSTEM) -machine virt -kernel $(KERNEL_ELF) -m $(QEMU_DEBUG_MEM) \
		$(QEMU_COMMON_TAIL) -nographic -S -gdb tcp::1234
