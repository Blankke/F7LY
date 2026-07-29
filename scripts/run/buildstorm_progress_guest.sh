#!/bin/sh
#
# BuildStorm 冷构建进度诊断（在 guest 内运行）。
#
# 使用示例：
#   GUEST_SCRIPT_OVERRIDE=scripts/run/buildstorm_progress_guest.sh \
#     HOST_TIMEOUT=8m bash scripts/run/buildstorm_perf_probe.sh riscv
#
# 每 30 秒记录 uptime、Cargo/rustc 进程和 target/debug 产物数，用来区分
# 依赖解析的有效计算、等待唤醒卡住和多核编译。该脚本只用于诊断，不用于
# 最终计分或性能结论。

echo "#### OS COMP TEST GROUP START buildstorm-progress ####"

mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null

export PATH=/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin
export HOME=/root
export RUSTUP_HOME=/root/.rustup
export CARGO_HOME=/root/.cargo
export RUSTUP_TOOLCHAIN=nightly-2026-05-28
export CARGO_NET_OFFLINE=true

cd /work/tgoskits || exit 1
rm -rf target/debug
sync

PROGRESS_LOG=/tmp/buildstorm_progress.cargo.log
rm -f "${PROGRESS_LOG}"
cargo build -p tg-xtask >"${PROGRESS_LOG}" 2>&1 &
CARGO_PID=$!

SAMPLE=0
while [ "${SAMPLE}" -le 16 ]; do
    UPTIME=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
    ARTIFACTS=$(find target/debug/deps -type f 2>/dev/null | wc -l)
    LOG_LINES=$(wc -l <"${PROGRESS_LOG}" 2>/dev/null)
    echo "BUILDSTORM_PROGRESS sample=${SAMPLE} uptime=${UPTIME} artifacts=${ARTIFACTS} log_lines=${LOG_LINES}"
    ps -eo pid,ppid,stat,psr,pcpu,time,comm 2>/dev/null |
        awk 'NR == 1 || $7 == "cargo" || $7 == "rustc"'
    tail -n 8 "${PROGRESS_LOG}" 2>/dev/null

    if ! kill -0 "${CARGO_PID}" 2>/dev/null; then
        break
    fi
    sleep 30
    SAMPLE=$((SAMPLE + 1))
done

if kill -0 "${CARGO_PID}" 2>/dev/null; then
    kill "${CARGO_PID}" 2>/dev/null
fi
wait "${CARGO_PID}"
RC=$?
UPTIME=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
ARTIFACTS=$(find target/debug/deps -type f 2>/dev/null | wc -l)
echo "BUILDSTORM_PROGRESS_END rc=${RC} uptime=${UPTIME} artifacts=${ARTIFACTS}"
echo "#### OS COMP TEST GROUP END buildstorm-progress ####"
