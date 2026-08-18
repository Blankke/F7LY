#!/usr/bin/env bash
set -Eeuo pipefail

# VisionFive 2 日常下板入口：
#   build：构建 shell 内核、发布到已挂载 FAT 分区并自动启动；
#   send：复用已有 shell 内核，发布后自动启动。

SCRIPT_PATH=${BASH_SOURCE[0]:-$0}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$PROJECT_ROOT"

readonly KERNEL_ELF="$PROJECT_ROOT/kernel-rv-visionfive2-shell"
readonly KERNEL_BIN="$PROJECT_ROOT/kernel-rv-visionfive2-shell.bin"

KERNEL_FAT_NAME=${KERNEL_FAT_NAME:-kernel-rv-visionfive2-shell.bin}
DTB_FAT_NAME=${DTB_FAT_NAME:-jh7110-starfive-visionfive-2-v1.3b.dtb}
DTB_SOURCE=${DTB_SOURCE:-}
BOOT_DIR=${BOOT_DIR:-/mnt/f7ly-vf2-boot}
SERIAL_DEVICE=${SERIAL_DEVICE:-/dev/ttyUSB0}
MMC_DEVICE=${MMC_DEVICE:-1}
MMC_PARTITION=${MMC_PARTITION:-1}
KERNEL_ADDR=${KERNEL_ADDR:-0x40200000}
FDT_ADDR=${FDT_ADDR:-0x46000000}

BOARD_IP=${BOARD_IP:-192.168.1.2}
NETMASK=${NETMASK:-255.255.255.0}
GATEWAY_IP=${GATEWAY_IP:-192.168.1.1}
DNS_IP=${DNS_IP:-$GATEWAY_IP}
BROADCAST_IP=${BROADCAST_IP:-192.168.1.255}

usage() {
    cat <<'EOF'
只支持两条指令：

  ./scripts/board/visionfive2-dev.sh build
      构建 Shell 内核，发布到 FAT 启动分区，打开 minicom 自动启动并记录日志。

  ./scripts/board/visionfive2-dev.sh send
      复用当前 Shell 内核，发布到 FAT 启动分区，打开 minicom 自动启动并记录日志。

默认采用已验证的 U-Boot 契约：
  mmc 1:1
  kernel  0x40200000 kernel-rv-visionfive2-shell.bin
  DTB     0x46000000 jh7110-starfive-visionfive-2-v1.3b.dtb
  booti 0x40200000 - 0x46000000

运行前把 SD 卡 FAT 分区挂载到 BOOT_DIR。仓库不保存板型相关 DTB：若目标
分区已有 DTB，可不设置 DTB_SOURCE；需要更新时显式指定正确的 v1.3b DTB。

常用覆盖：
  BOOT_DIR=/media/$USER/boot
  DTB_SOURCE=/path/to/jh7110-starfive-visionfive-2-v1.3b.dtb
  SERIAL_DEVICE=/dev/ttyUSB0
  MMC_DEVICE=1 MMC_PARTITION=1
EOF
}

die() {
    echo "错误：$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1"
}

require_file() {
    [ -s "$1" ] || die "文件不存在或为空：$1"
}

validate_uint() {
    local name=$1 value=$2
    case "$value" in
        *[!0-9]*|'') die "$name 必须是非负整数：$value" ;;
    esac
}

build_kernel() {
    require_command make
    echo "[编译] VisionFive 2 Shell 内核"
    make build PROFILE=riscv-visionfive2 MODE=shell \
        VISIONFIVE2_IPV4="$BOARD_IP" \
        VISIONFIVE2_NETMASK="$NETMASK" \
        VISIONFIVE2_GATEWAY="$GATEWAY_IP" \
        VISIONFIVE2_DNS="$DNS_IP" \
        VISIONFIVE2_BROADCAST="$BROADCAST_IP"
    require_file "$KERNEL_ELF"
    require_file "$KERNEL_BIN"
    ls -lh "$KERNEL_ELF" "$KERNEL_BIN"
}

publish_assets() {
    require_command install
    require_file "$KERNEL_BIN"
    [ -d "$BOOT_DIR" ] || die "FAT 启动分区未挂载：$BOOT_DIR"

    echo "[发布] 内核 -> $BOOT_DIR/$KERNEL_FAT_NAME"
    install -m 0644 "$KERNEL_BIN" "$BOOT_DIR/$KERNEL_FAT_NAME" 2>/dev/null ||
        sudo install -m 0644 "$KERNEL_BIN" "$BOOT_DIR/$KERNEL_FAT_NAME"

    if [ -n "$DTB_SOURCE" ]; then
        require_file "$DTB_SOURCE"
        echo "[发布] DTB -> $BOOT_DIR/$DTB_FAT_NAME"
        install -m 0644 "$DTB_SOURCE" "$BOOT_DIR/$DTB_FAT_NAME" 2>/dev/null ||
            sudo install -m 0644 "$DTB_SOURCE" "$BOOT_DIR/$DTB_FAT_NAME"
    else
        require_file "$BOOT_DIR/$DTB_FAT_NAME"
        echo "[复用] $BOOT_DIR/$DTB_FAT_NAME"
    fi
    sync "$BOOT_DIR/$KERNEL_FAT_NAME" "$BOOT_DIR/$DTB_FAT_NAME"
    ls -lh "$BOOT_DIR/$KERNEL_FAT_NAME" "$BOOT_DIR/$DTB_FAT_NAME"
}

emit_prompt_expect() {
    cat <<'EOF'
expect {
    "StarFive #"
    "=>"
    timeout 30 exit 11
}
EOF
}

create_uboot_runscript() {
    local output=$1
    validate_uint MMC_DEVICE "$MMC_DEVICE"
    validate_uint MMC_PARTITION "$MMC_PARTITION"

    {
        printf '%s\n' 'verbose on'
        printf '%s\n' 'timeout 300'
        printf '%s\n' 'send ""'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "StarFive #" goto uboot_ready'
        printf '%s\n' '    "=>" goto uboot_ready'
        printf '%s\n' '    "Hit any key to stop autoboot" goto interrupt_autoboot'
        printf '%s\n' '    "Autoboot in" goto interrupt_autoboot'
        printf '%s\n' '    timeout 300 exit 10'
        printf '%s\n' '}'
        printf '%s\n' 'interrupt_autoboot:'
        printf '%s\n' 'send " "'
        emit_prompt_expect
        printf '%s\n' 'uboot_ready:'

        printf 'send "mmc dev %s"\n' "$MMC_DEVICE"
        emit_prompt_expect
        printf '%s\n' 'send "mmc rescan"'
        emit_prompt_expect

        printf 'send "fatload mmc %s:%s %s %s"\n' \
            "$MMC_DEVICE" "$MMC_PARTITION" "$KERNEL_ADDR" "$KERNEL_FAT_NAME"
        printf '%s\n' 'expect {'
        printf '%s\n' '    "bytes read"'
        printf '%s\n' '    "Unable to read file" exit 20'
        printf '%s\n' '    "File not found" exit 21'
        printf '%s\n' '    timeout 120 exit 22'
        printf '%s\n' '}'
        emit_prompt_expect

        printf 'send "fatload mmc %s:%s %s %s"\n' \
            "$MMC_DEVICE" "$MMC_PARTITION" "$FDT_ADDR" "$DTB_FAT_NAME"
        printf '%s\n' 'expect {'
        printf '%s\n' '    "bytes read"'
        printf '%s\n' '    "Unable to read file" exit 30'
        printf '%s\n' '    "File not found" exit 31'
        printf '%s\n' '    timeout 120 exit 32'
        printf '%s\n' '}'
        emit_prompt_expect

        printf 'send "fdt addr %s"\n' "$FDT_ADDR"
        emit_prompt_expect
        printf '%s\n' 'send "fdt print / model"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "VisionFive 2"'
        printf '%s\n' '    "VisionFive2"'
        printf '%s\n' '    timeout 15 exit 40'
        printf '%s\n' '}'
        emit_prompt_expect

        printf 'send "booti %s - %s"\n' "$KERNEL_ADDR" "$FDT_ADDR"
        printf '%s\n' 'expect {'
        printf '%s\n' '    "[boot] early runtime begin" exit 0'
        printf '%s\n' '    timeout 30 exit 50'
        printf '%s\n' '}'
    } > "$output"
}

send_to_board() {
    local action=$1
    require_command minicom
    require_command mktemp
    [ -e "$SERIAL_DEVICE" ] || die "串口设备不存在：$SERIAL_DEVICE"

    local runscript_file timestamp git_head log_dir log_file latest_log minicom_rc=0
    runscript_file=$(mktemp /tmp/f7ly-vf2-uboot.XXXXXX.runscript)
    chmod 0600 "$runscript_file"
    create_uboot_runscript "$runscript_file"

    timestamp=$(date +%Y%m%d-%H%M%S)
    git_head=$(git rev-parse --short HEAD 2>/dev/null || printf '%s' nogit)
    log_dir="$PROJECT_ROOT/logs/run"
    log_file="$log_dir/output_visionfive2_${timestamp}_${action}_${git_head}.txt"
    latest_log="$log_dir/output_visionfive2_latest.txt"
    mkdir -p "$log_dir"
    touch "$log_file"
    chmod 0644 "$log_file"
    ln -sfn "$(basename -- "$log_file")" "$latest_log"

    echo "[下板] 打开 $SERIAL_DEVICE，等待 StarFive U-Boot"
    echo "[日志] $log_file"
    echo "[最新] $latest_log"
    echo "若板子不在 U-Boot，请按 RESET；退出 minicom：Ctrl-A，然后按 X。"
    sudo minicom -D "$SERIAL_DEVICE" -b 115200 \
        -S "$runscript_file" -C "$log_file" \
        --capturefile-buffer-mode=N || minicom_rc=$?
    rm -f -- "$runscript_file"

    if [ "$minicom_rc" -ne 0 ]; then
        die "minicom 下板流程失败，退出码：$minicom_rc"
    fi
}

main() {
    [ "$#" -eq 1 ] || { usage; exit 2; }
    case "$1" in
        build)
            build_kernel
            publish_assets
            send_to_board build
            ;;
        send)
            publish_assets
            send_to_board send
            ;;
        *)
            usage
            exit 2
            ;;
    esac
}

if [ "$SCRIPT_PATH" = "$0" ]; then
    main "$@"
fi
