#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/../../../../.." && pwd)
CASE_LIST_FILE="$ROOT_DIR/tools/ltp/judge/ltp-testcase-list.txt"

COMBO="${1:-riscv-musl}"
RAW_LOG="${2:-$ROOT_DIR/tools/ltp/judge/v1/outputs/$COMBO/raw.log}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-15}"

case "$COMBO" in
    riscv-musl)
        TEST_ROOT="/mnt/sdcard-rv/musl/ltp/testcases/bin"
        QEMU_BIN="qemu-riscv64-static"
        QEMU_EXTRA_ENV=""
        ;;
    riscv-glibc)
        TEST_ROOT="/mnt/sdcard-rv/glibc/ltp/testcases/bin"
        QEMU_BIN="qemu-riscv64-static"
        QEMU_EXTRA_ENV=""
        ;;
    loongarch-musl)
        TEST_ROOT="/mnt/sdcard-la/musl/ltp/testcases/bin"
        QEMU_BIN="qemu-loongarch64-static"
        QEMU_EXTRA_ENV=""
        ;;
    loongarch-glibc)
        TEST_ROOT="/mnt/sdcard-la/glibc/ltp/testcases/bin"
        QEMU_BIN="qemu-loongarch64-static"
        QEMU_EXTRA_ENV="LD_LIBRARY_PATH=/mnt/sdcard-la/glibc/lib"
        ;;
    *)
        echo "Error: unknown combo '$COMBO'" >&2
        echo "Supported combos: riscv-musl, riscv-glibc, loongarch-musl, loongarch-glibc" >&2
        exit 1
        ;;
esac

if [ ! -f "$CASE_LIST_FILE" ]; then
    echo "Error: case list file not found: $CASE_LIST_FILE" >&2
    exit 1
fi

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "Error: required qemu binary not found in PATH: $QEMU_BIN" >&2
    exit 1
fi

if [ ! -d "$TEST_ROOT" ]; then
    echo "Error: test root not found: $TEST_ROOT" >&2
    exit 1
fi

mkdir -p "$(dirname "$RAW_LOG")"

mapfile -t files_array < <(grep -E '^[[:space:]]*"' "$CASE_LIST_FILE" | tr -d '\r' | sed -E 's/^[[:space:]]*"([^"]+)".*$/\1/')

{
    echo "# combo=$COMBO"
    echo "# generated_at=$(date '+%F %T %Z')"
    echo "# test_root=$TEST_ROOT"
    echo "# qemu=$QEMU_BIN"
    echo "# timeout_seconds=$TIMEOUT_SECONDS"
    echo
} > "$RAW_LOG"

run_case() {
    local program_name="$1"
    local program_path="$TEST_ROOT/$program_name"
    local tmpdir
    tmpdir=$(mktemp -d "/tmp/ltp-${COMBO}-XXXX")

    {
        echo "RUN LTP CASE $program_name"

        if [ ! -f "$program_path" ]; then
            echo "MISSING CASE BINARY: $program_path"
            echo "FAIL LTP CASE $program_name: 127"
            rm -rf "$tmpdir"
            return 0
        fi

        set +e
        (cd "$tmpdir" && env $QEMU_EXTRA_ENV timeout "${TIMEOUT_SECONDS}s" "$QEMU_BIN" "$program_path" 2>&1)
        local exit_code=$?
        set -e

        rm -rf "$tmpdir"

        if [ "$exit_code" -eq 124 ]; then
            echo "Test case timed out after ${TIMEOUT_SECONDS} seconds"
            echo "FAIL LTP CASE $program_name: 124"
        else
            echo "END LTP CASE $program_name: $exit_code"
        fi
    } >> "$RAW_LOG"
}

for program_name in "${files_array[@]}"; do
    run_case "$program_name"
done

echo "raw_log=$RAW_LOG"
