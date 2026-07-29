#!/bin/sh
#
# BuildStorm tg-xtask 性能探针（在 guest 内运行）。
#
# 使用示例：
#   由 scripts/run/buildstorm_perf_probe.sh 自动写入临时评测镜像并执行。
#
# 探针只测从干净 target/debug 开始的 `cargo build -p tg-xtask`，固定运行
# 900 秒；不会复用上轮产物，也不会修改计时接口或伪造 BuildStorm 结果。

echo "#### OS COMP TEST GROUP START buildstorm-perf ####"

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

# 清理不计入计时；每次 QEMU 又使用全新的临时镜像副本，确保冷启动口径一致。
rm -rf target/debug
sync

START_EPOCH=$(date +%s)
START_UPTIME=$(cut -d' ' -f1 /proc/uptime)
echo "BUILDSTORM_PERF_BEGIN epoch=${START_EPOCH} uptime=${START_UPTIME} cores=$(nproc)"

rm -f /tmp/buildstorm_perf.rc
{
    timeout 900 cargo build -p tg-xtask
    echo $? > /tmp/buildstorm_perf.rc
} 2>&1 | awk -v start="${START_EPOCH}" '
{
    gsub(/\r/, "\n")
    printf "BUILDSTORM_PERF_T+%ds %s\n", systime() - start, $0
    fflush()
}'

RC=$(cat /tmp/buildstorm_perf.rc 2>/dev/null)
[ -n "${RC}" ] || RC=255
END_UPTIME=$(cut -d' ' -f1 /proc/uptime)
ARTIFACTS=$(find target/debug/deps -type f 2>/dev/null | wc -l)
echo "BUILDSTORM_PERF_END rc=${RC} uptime=${END_UPTIME} artifacts=${ARTIFACTS}"
echo "#### OS COMP TEST GROUP END buildstorm-perf ####"

