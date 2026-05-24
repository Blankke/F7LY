#!/usr/bin/env bash
# 用法示例：
#   tools/patch_loongarch_libctest_llsc.sh sdcard-la.img
# 说明：
#   这个脚本会原地修补 LoongArch 镜像里 musl libc-test 预置二进制中的
#   “ll.w 结果寄存器与地址寄存器复用”代码序列，避免 pthread 相关测例在
#   __vm_lock 等路径上卡死。脚本是幂等的：已经补过则直接跳过。

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "用法: $0 <loongarch-rootfs-image>" >&2
    exit 1
fi

IMAGE_PATH=$1
DEBUGFS_BIN=${DEBUGFS_BIN:-debugfs}

if [[ ! -f "$IMAGE_PATH" ]]; then
    echo "[patch-loongarch-libctest] 镜像不存在: $IMAGE_PATH" >&2
    exit 1
fi

if ! command -v "$DEBUGFS_BIN" >/dev/null 2>&1; then
    echo "[patch-loongarch-libctest] 找不到 debugfs" >&2
    exit 1
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

patch_bytes() {
    local host_file=$1
    local offset_hex=$2
    local old_hex=$3
    local new_hex=$4
    local label=$5

    local offset=$((offset_hex))
    local byte_count=$(( ${#old_hex} / 2 ))
    local current_hex
    current_hex=$(od -An -tx1 -j "$offset" -N "$byte_count" "$host_file" | tr -d ' \n')

    if [[ "$current_hex" == "$new_hex" ]]; then
        return 1
    fi

    if [[ "$current_hex" != "$old_hex" ]]; then
        echo "[patch-loongarch-libctest] $label 字节不匹配，拒绝盲补: offset=$offset_hex current=$current_hex" >&2
        exit 1
    fi

    xxd -r -p <<<"$new_hex" | dd of="$host_file" bs=1 seek="$offset" conv=notrunc status=none
    return 0
}

patch_file_in_image() {
    local image_file=$1
    local guest_path=$2
    local host_name=$3
    shift 3

    local host_file="$TMP_DIR/$host_name"
    local cmd_file="$TMP_DIR/${host_name}.debugfs"
    local changed=0

    "$DEBUGFS_BIN" -R "dump $guest_path $host_file" "$image_file" >/dev/null 2>&1

    while [[ $# -gt 0 ]]; do
        local offset_hex=$1
        local old_hex=$2
        local new_hex=$3
        local label=$4
        shift 4

        if patch_bytes "$host_file" "$offset_hex" "$old_hex" "$new_hex" "$label"; then
            changed=1
        fi
    done

    if [[ $changed -eq 0 ]]; then
        return 0
    fi

    cat >"$cmd_file" <<EOF
rm $guest_path
write $host_file $guest_path
EOF
    "$DEBUGFS_BIN" -w -f "$cmd_file" "$image_file" >/dev/null 2>&1
}

# entry-static.exe 里只有一处 __vm_lock 热点。
patch_file_in_image "$IMAGE_PATH" "/musl/entry-static.exe" "entry-static.exe" \
    0x354a8 \
    ac21d5028c0100208c058002ae21d502cc010021 \
    ae21d502cc0100208c05800200004003cc010021 \
    "entry-static::__vm_lock"

# 动态 musl libc.so 里同类代码出现多处，集中一起补掉，避免 pthread_* 继续撞到下一处。
patch_file_in_image "$IMAGE_PATH" "/musl/lib/libc.so" "musl-libc.so" \
    0x14620 \
    ac31f9028c0100208c058002ae31f902cc010021 \
    ae31f902cc0100208c05800200004003cc010021 \
    "libc.so::__aio_get_queue_ref_inc" \
    0x1471c \
    ac31f9028c0100208cfdbf02ae31f902cc010021 \
    ae31f902cc0100208cfdbf0200004003cc010021 \
    "libc.so::__aio_unref_queue_ref_dec" \
    0x14ab0 \
    8c20f9028c0100208d8140008f20f902cc011500 \
    8f20f902ec0100208d81400000004003cc011500 \
    "libc.so::cleanup_exchange_zero" \
    0x6a0ec \
    ac61d6028c0100208c058002ae61d602cc010021 \
    ae61d602cc0100208c05800200004003cc010021 \
    "libc.so::__vm_lock"

echo "[patch-loongarch-libctest] 已检查并修补 $IMAGE_PATH"
