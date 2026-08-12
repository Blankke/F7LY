#!/usr/bin/env bash
set -Eeuo pipefail

# 2K1000LA 日常下板入口，只保留两个命令：
#   build：重新编译后下板；send：复用已有内核直接下板。
# 两条命令最终都会打开 minicom，逐条驱动 U-Boot 装载 DTB/内核并执行。

SCRIPT_PATH=${BASH_SOURCE[0]:-$0}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
# 脚本位于 scripts/board/，所有构建产物、资源和日志都以项目根目录为准。
# 这样无论从哪个工作目录调用，移动后的脚本都不会把自身目录误当成仓库根目录。
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$PROJECT_ROOT"

readonly KERNEL_ELF="$PROJECT_ROOT/kernel-la-2k1000-shell"
readonly KERNEL_BIN="$PROJECT_ROOT/kernel-la-2k1000-shell.bin"
readonly KERNEL_TFTP_NAME="kernel-la-2k1000-shell.bin"
readonly DTB_SOURCE="$PROJECT_ROOT/ref/tgoskits/os/StarryOS/configs/board/jl-lsgd2k10.dtb"
readonly DTB_TFTP_NAME="jl-lsgd2k10.dtb"
readonly KERNEL_ADDR="0x9000000000200000"
readonly FDT_ADDR="0x900000000a000000"

# 开发环境参数可以通过同名环境变量临时覆盖。
BOARD_IP=${BOARD_IP:-192.168.5.20}
SERVER_IP=${SERVER_IP:-192.168.5.17}
NETMASK=${NETMASK:-255.255.255.0}
GATEWAY_IP=${GATEWAY_IP:-192.168.5.1}
DNS_IP=${DNS_IP:-$GATEWAY_IP}
BROADCAST_IP=${BROADCAST_IP:-192.168.5.255}
SERIAL_DEVICE=${SERIAL_DEVICE:-/dev/ttyUSB0}
TFTP_DIR=${TFTP_DIR:-}

usage() {
    cat <<'EOF'
只支持两条指令：

  ./scripts/board/2k1000-dev.sh build
      编译 Shell 内核，发布内核和 DTB，然后打开 minicom 自动下板并记录日志。

  ./scripts/board/2k1000-dev.sh send
      复用当前 Shell 内核，发布内核和 DTB，然后打开 minicom 自动下板并记录日志。

运行后若板子不在 U-Boot，请按开发板 RESET。脚本会等待 U-Boot，并在
每条命令返回 => 后再发下一条，最终停留在 F7LY 的 minicom 串口界面。

可选环境变量：
  BOARD_IP=192.168.5.20
  SERVER_IP=192.168.5.17
  SERIAL_DEVICE=/dev/ttyUSB0
  TFTP_DIR=/srv/tftp
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

validate_ipv4_text() {
    local name=$1 value=$2
    case "$value" in
        *[!0-9.]*|'') die "$name 只能包含 IPv4 数字和点：$value" ;;
    esac
}

resolve_tftp_dir() {
    if [ -n "$TFTP_DIR" ]; then
        printf '%s\n' "$TFTP_DIR"
        return
    fi

    local config=/etc/default/tftpd-hpa
    [ -r "$config" ] || die "无法读取 $config；请设置 TFTP_DIR=/srv/tftp"

    local line value
    line=$(grep -E '^[[:space:]]*TFTP_DIRECTORY=' "$config" | tail -n 1 || true)
    [ -n "$line" ] || die "$config 中没有 TFTP_DIRECTORY"
    value=${line#*=}
    value=${value#\"}
    value=${value%\"}
    value=${value#\'}
    value=${value%\'}
    [ -n "$value" ] || die "TFTP_DIRECTORY 为空"
    printf '%s\n' "$value"
}

build_kernel() {
    require_command make

    echo "[编译] 2K1000LA Shell 内核"
    make build PROFILE=loongarch-2k1000 MODE=shell \
        LS2K1000_IPV4="$BOARD_IP" \
        LS2K1000_NETMASK="$NETMASK" \
        LS2K1000_GATEWAY="$GATEWAY_IP" \
        LS2K1000_DNS="$DNS_IP" \
        LS2K1000_BROADCAST="$BROADCAST_IP"

    require_file "$KERNEL_ELF"
    require_file "$KERNEL_BIN"
    ls -lh "$KERNEL_ELF" "$KERNEL_BIN"
}

publish_assets() {
    require_command install
    require_file "$KERNEL_BIN"
    require_file "$DTB_SOURCE"

    local tftp_dir
    tftp_dir=$(resolve_tftp_dir)
    [ -d "$tftp_dir" ] || die "TFTP 目录不存在：$tftp_dir"

    echo "[发布] 内核和 DTB -> $tftp_dir"
    sudo install -m 0644 "$KERNEL_BIN" "$tftp_dir/$KERNEL_TFTP_NAME"
    sudo install -m 0644 "$DTB_SOURCE" "$tftp_dir/$DTB_TFTP_NAME"
    ls -lh "$tftp_dir/$KERNEL_TFTP_NAME" "$tftp_dir/$DTB_TFTP_NAME"
}

create_uboot_runscript() {
    local output=$1
    validate_ipv4_text SERVER_IP "$SERVER_IP"
    validate_ipv4_text BOARD_IP "$BOARD_IP"
    validate_ipv4_text NETMASK "$NETMASK"

    # minicom 的 runscript 会等待每条命令重新出现提示符，避免批量粘贴时
    # UART FIFO 丢字符。任何关键检查失败都会停止下发，保留现场供人工观察。
    {
        printf '%s\n' 'verbose on'
        printf '%s\n' 'timeout 300'

        # 板子已经停在 U-Boot 时，空回车会重新打印提示符；板子随后 RESET 时，
        # 脚本也能识别启动提示并按 c 中断自动启动。
        printf '%s\n' 'send ""'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "=>" goto uboot_ready'
        printf '%s\n' '    "Press c to enter u-boot console" goto interrupt_autoboot'
        printf '%s\n' '    timeout 300 exit 10'
        printf '%s\n' '}'
        printf '%s\n' 'interrupt_autoboot:'
        printf '%s\n' 'send "c"'
        printf '%s\n' 'expect "=>"'
        printf '%s\n' 'uboot_ready:'

        printf 'send "setenv serverip %s"\n' "$SERVER_IP"
        printf '%s\n' 'expect "=>"'
        printf 'send "setenv ipaddr %s"\n' "$BOARD_IP"
        printf '%s\n' 'expect "=>"'
        printf 'send "setenv netmask %s"\n' "$NETMASK"
        printf '%s\n' 'expect "=>"'

        printf '%s\n' 'send "ping ${serverip}"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "is alive"'
        printf '%s\n' '    "ARP Retry count exceeded" exit 20'
        printf '%s\n' '    timeout 30 exit 21'
        printf '%s\n' '}'
        printf '%s\n' 'expect "=>"'

        printf 'send "setenv fdt_addr %s"\n' "$FDT_ADDR"
        printf '%s\n' 'expect "=>"'
        printf 'send "tftpboot ${fdt_addr} %s"\n' "$DTB_TFTP_NAME"
        printf '%s\n' 'expect {'
        printf '%s\n' '    "Bytes transferred ="'
        printf '%s\n' '    "File not found" exit 30'
        printf '%s\n' '    "TFTP error" exit 31'
        printf '%s\n' '    timeout 120 exit 32'
        printf '%s\n' '}'
        printf '%s\n' 'expect "=>"'
        printf '%s\n' 'send "fdt addr ${fdt_addr}"'
        printf '%s\n' 'expect "=>"'

        printf '%s\n' 'send "fdt print / model"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "LS2K1000-DP-V10"'
        printf '%s\n' '    timeout 10 exit 40'
        printf '%s\n' '}'
        printf '%s\n' 'expect "=>"'
        printf '%s\n' 'send "fdt print / compatible"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "loongson,ls2k"'
        printf '%s\n' '    timeout 10 exit 41'
        printf '%s\n' '}'
        printf '%s\n' 'expect "=>"'

        printf 'send "setenv kernel_addr %s"\n' "$KERNEL_ADDR"
        printf '%s\n' 'expect "=>"'
        printf 'send "tftpboot ${kernel_addr} %s"\n' "$KERNEL_TFTP_NAME"
        printf '%s\n' 'expect {'
        printf '%s\n' '    "Bytes transferred ="'
        printf '%s\n' '    "File not found" exit 50'
        printf '%s\n' '    "TFTP error" exit 51'
        printf '%s\n' '    timeout 120 exit 52'
        printf '%s\n' '}'
        printf '%s\n' 'expect "=>"'
        printf '%s\n' 'send "go ${kernel_addr} ${fdt_addr}"'

        # runscript 在看到内核入口后结束，但 minicom 本身继续运行，用户会直接
        # 留在 F7LY 串口界面中查看日志或操作 Shell。
        printf '%s\n' 'expect {'
        printf '%s\n' '    "entry reached" exit 0'
        printf '%s\n' '    timeout 30 exit 60'
        printf '%s\n' '}'
    } > "$output"
}

send_to_board() {
    local action=$1
    require_command minicom
    require_command mktemp
    [ -e "$SERIAL_DEVICE" ] || die "串口设备不存在：$SERIAL_DEVICE"

    local runscript_file timestamp git_head log_dir log_file latest_log minicom_rc=0
    runscript_file=$(mktemp /tmp/f7ly-2k1000-uboot.XXXXXX.runscript)
    chmod 0600 "$runscript_file"
    create_uboot_runscript "$runscript_file"

    # capture 使用无缓冲模式，minicom 运行期间日志也会实时落盘，方便另一终端
    # 或 agent 直接读取最新现场，不必等用户退出 minicom。
    timestamp=$(date +%Y%m%d-%H%M%S)
    git_head=$(git rev-parse --short HEAD 2>/dev/null || printf '%s' nogit)
    log_dir="$PROJECT_ROOT/logs/run"
    log_file="$log_dir/output_2k1000_${timestamp}_${action}_${git_head}.txt"
    latest_log="$log_dir/output_2k1000_latest.txt"
    mkdir -p "$log_dir"
    touch "$log_file"
    chmod 0644 "$log_file"
    ln -sfn "$(basename -- "$log_file")" "$latest_log"

    echo "[下板] 打开 $SERIAL_DEVICE 并自动装载 DTB/内核"
    echo "[日志] $log_file"
    echo "[最新] $latest_log"
    echo "如果板子当前不在 U-Boot，请按 RESET；退出 minicom：Ctrl-A，然后按 X。"
    sudo minicom -D "$SERIAL_DEVICE" -b 115200 \
        -S "$runscript_file" \
        -C "$log_file" \
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
