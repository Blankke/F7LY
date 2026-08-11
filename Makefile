# F7LY 构建入口。
#
# 顶层文件只回答“用户想做什么”；具体的配置、initcode、内核和 QEMU
# 构建规则分别放在 mk/ 下。这样新增开发板时只需要增加平台画像，不需要
# 继续扩大这个入口文件。
.DEFAULT_GOAL := all

PROJECT_ROOT := $(CURDIR)

include mk/config.mk
include mk/initcode.mk
include mk/kernel.mk
include mk/qemu.mk

.PHONY: all competition-riscv competition-loongarch \
        build build-current initcode run shell debug qemu-run qemu-debug \
        prepare-image help profiles print-config force-build-metadata \
        clean clean-current cleanlog

# 大赛固定入口：一次 make all 必须完整构建两套 QEMU evaluation 内核。
# 两个子 make 各自重新解析一套平台画像，不能合并为当前画像的单一依赖。
all:
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) competition-riscv competition-loongarch

competition-riscv:
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=riscv-qemu MODE=evaluation build-current

competition-loongarch:
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=loongarch-qemu MODE=evaluation build-current

# build 是稳定的公开入口；实际依赖图在并行子 make 的 build-current 中展开。
# 这样普通 `make build` 仍自动并行，又不需要在 Makefile 内篡改 MAKEFLAGS。
build:
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=$(PROFILE) MODE=$(MODE) build-current

help:
	@printf '%s\n' \
		'F7LY 构建命令：' \
		'  make all' \
		'  make build PROFILE=<画像> [MODE=evaluation|shell]' \
		'  make run   PROFILE=<qemu画像>' \
		'  make shell PROFILE=<qemu画像>' \
		'  make debug PROFILE=<qemu画像> [MODE=evaluation|shell]' \
		'  make prepare-image PROFILE=<qemu画像> [MODE=evaluation|shell]' \
		'  make profiles' \
		'  make print-config PROFILE=<画像> [MODE=...]'

profiles:
	@printf '%s\n' $(AVAILABLE_PROFILES)

print-config:
	@printf 'PROFILE=%s\nPROFILE_NAME=%s\nPROFILE_ARCH=%s\nPROFILE_BOARD=%s\nMODE=%s\nBUILD_DIR=%s\nKERNEL_ELF=%s\n' \
		'$(PROFILE)' '$(PROFILE_NAME)' '$(PROFILE_ARCH)' '$(PROFILE_BOARD)' \
		'$(MODE)' '$(BUILD_DIR)' '$(KERNEL_ELF)'

# 只删除本构建系统拥有的画像目录。build/ 下还可能保存 Docker 评测包、
# selfbuild 镜像和专项日志，不能把整个目录当作一次性输出删除。
clean:
	@set -e; for profile in $(AVAILABLE_PROFILES); do \
		$(MAKE) --no-print-directory PROFILE=$$profile MODE=evaluation clean-current; \
		$(MAKE) --no-print-directory PROFILE=$$profile MODE=shell clean-current; \
	done
	rm -f user/disasm_initcode.asm kernel.asm

# 只清理由当前画像和模式推导出的产物；clean 通过画像列表调用本目标。
clean-current:
	rm -rf "$(BUILD_DIR)"
	rm -f "$(KERNEL_ELF)" "$(KERNEL_BIN)"

cleanlog:
	rm -rf "$(PROJECT_ROOT)/logs/run" "$(PROJECT_ROOT)/logs/tmp"
