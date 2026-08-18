#!/usr/bin/env bash
set -Eeuo pipefail

# VisionFive 2 日常 TFTP 下板入口，只保留两个命令：
#   build：重新编译后下板；send：复用已有内核直接下板。
# 两条命令都会打开 minicom，逐条等待 U-Boot 响应，避免批量粘贴丢字符。

SCRIPT_PATH=${BASH_SOURCE[0]:-$0}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$PROJECT_ROOT"

readonly KERNEL_ELF="$PROJECT_ROOT/kernel-rv-visionfive2-shell"
readonly KERNEL_BIN="$PROJECT_ROOT/kernel-rv-visionfive2-shell.bin"
readonly KERNEL_TFTP_NAME="kernel-rv-visionfive2-shell.bin"

# 当前 VF2 固件自带的 control DTB 已核对为完整 JH7110/VisionFive 2 DTB。
# U-Boot 将它放在高地址，启动前先复制到 F7LY 已验证的低端 RAM 窗口。
readonly KERNEL_ADDR="0x40200000"
readonly FDT_ADDR="0x46000000"
readonly FDT_COPY_SIZE="0x10000"

# 默认值对应当前开发网络，可通过同名环境变量临时覆盖。
BOARD_IP=${BOARD_IP:-192.168.88.243}
SERVER_IP=${SERVER_IP:-192.168.88.244}
NETMASK=${NETMASK:-255.255.255.0}
GATEWAY_IP=${GATEWAY_IP:-192.168.88.1}
DNS_IP=${DNS_IP:-$GATEWAY_IP}
BROADCAST_IP=${BROADCAST_IP:-192.168.88.255}
ETHACT=${ETHACT:-ethernet@16040000}
SERIAL_DEVICE=${SERIAL_DEVICE:-/dev/ttyUSB0}
TFTP_DIR=${TFTP_DIR:-}

usage() {
    cat <<'EOF'
只支持两条指令：

  ./scripts/board/visionfive2-dev.sh build
      编译 Shell 内核、发布到 TFTP，然后打开 minicom 自动下板并记录日志。

  ./scripts/board/visionfive2-dev.sh send
      复用当前 Shell 内核、发布到 TFTP，然后打开 minicom 自动下板并记录日志。

脚本启动后若板子不在 U-Boot，请按一次 RESET。它会自动完成：
  1. 等待并截停 StarFive U-Boot；
  2. 把板载 control DTB 搬到 0x46000000 并校验板型；
  3. 配置临时网络并检查 TFTP 主机连通性；
  4. TFTP 下载内核到 0x40200000；
  5. 执行 booti，并把交互式 minicom 留给用户。

常用覆盖：
  BOARD_IP=192.168.88.243
  SERVER_IP=192.168.88.244
  ETHACT=ethernet@16040000
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

    local tftp_dir target
    tftp_dir=$(resolve_tftp_dir)
    [ -d "$tftp_dir" ] || die "TFTP 目录不存在：$tftp_dir"
    target="$tftp_dir/$KERNEL_TFTP_NAME"

    echo "[发布] 内核 -> $target"
    install -m 0644 "$KERNEL_BIN" "$target" 2>/dev/null ||
        sudo install -m 0644 "$KERNEL_BIN" "$target"
    sync "$target"
    ls -lh "$target"
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
    validate_ipv4_text SERVER_IP "$SERVER_IP"
    validate_ipv4_text BOARD_IP "$BOARD_IP"
    validate_ipv4_text NETMASK "$NETMASK"

    # runscript 每次只发送一条命令并等待提示符，避免 UART FIFO 因整段粘贴
    # 丢字符。任何关键检查失败都会停止自动下发并保留串口现场。
    {
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

        # control DTB 的内容来自当前固件，只把地址搬到 F7LY 管理的低端 RAM。
        printf 'send "fdt move ${fdtcontroladdr} %s %s"\n' \
            "$FDT_ADDR" "$FDT_COPY_SIZE"
        emit_prompt_expect
        printf 'send "setenv fdt_addr_r %s"\n' "$FDT_ADDR"
        emit_prompt_expect
        printf '%s\n' 'send "fdt addr ${fdt_addr_r}"'
        emit_prompt_expect
        printf '%s\n' 'send "fdt print / model"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "StarFive VisionFive V2"'
        printf '%s\n' '    "StarFive VisionFive 2"'
        printf '%s\n' '    timeout 15 exit 20'
        printf '%s\n' '}'
        emit_prompt_expect
        printf '%s\n' 'send "fdt print / compatible"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "starfive,jh7110"'
        printf '%s\n' '    timeout 15 exit 21'
        printf '%s\n' '}'
        emit_prompt_expect

        printf 'send "setenv ethact %s"\n' "$ETHACT"
        emit_prompt_expect
        printf 'send "setenv serverip %s"\n' "$SERVER_IP"
        emit_prompt_expect
        printf 'send "setenv ipaddr %s"\n' "$BOARD_IP"
        emit_prompt_expect
        printf 'send "setenv netmask %s"\n' "$NETMASK"
        emit_prompt_expect
        printf '%s\n' 'send "ping ${serverip}"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "is alive" goto network_ready'
        printf '%s\n' '    "ARP Retry count exceeded" goto ping_retry_2'
        printf '%s\n' '    "is not alive" goto ping_retry_2'
        printf '%s\n' '    timeout 30 goto ping_retry_2'
        printf '%s\n' '}'
        printf '%s\n' 'ping_retry_2:'
        emit_prompt_expect
        printf '%s\n' 'sleep 3'
        printf '%s\n' 'send "ping ${serverip}"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "is alive" goto network_ready'
        printf '%s\n' '    "ARP Retry count exceeded" goto ping_retry_3'
        printf '%s\n' '    "is not alive" goto ping_retry_3'
        printf '%s\n' '    timeout 30 goto ping_retry_3'
        printf '%s\n' '}'
        printf '%s\n' 'ping_retry_3:'
        emit_prompt_expect
        printf '%s\n' 'sleep 5'
        printf '%s\n' 'send "ping ${serverip}"'
        printf '%s\n' 'expect {'
        printf '%s\n' '    "is alive" goto network_ready'
        printf '%s\n' '    "ARP Retry count exceeded" exit 30'
        printf '%s\n' '    "is not alive" exit 30'
        printf '%s\n' '    timeout 30 exit 31'
        printf '%s\n' '}'
        printf '%s\n' 'network_ready:'
        emit_prompt_expect

        printf 'send "setenv kernel_addr_r %s"\n' "$KERNEL_ADDR"
        emit_prompt_expect
        printf 'send "tftpboot ${kernel_addr_r} %s"\n' "$KERNEL_TFTP_NAME"
        printf '%s\n' 'expect {'
        printf '%s\n' '    "Bytes transferred ="'
        printf '%s\n' '    "File not found" exit 40'
        printf '%s\n' '    "TFTP error" exit 41'
        printf '%s\n' '    timeout 120 exit 42'
        printf '%s\n' '}'
        emit_prompt_expect

        printf '%s\n' 'send "booti ${kernel_addr_r} - ${fdt_addr_r}"'
        # 看到内核入口后 runscript 结束，minicom 继续留在交互界面。
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
    runscript_file=$(mktemp /tmp/f7ly-vf2-tftp.XXXXXX.runscript)
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
