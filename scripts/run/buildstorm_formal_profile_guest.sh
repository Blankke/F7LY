#!/bin/sh
#
# 与决赛 BuildStorm 相同的冷 target 正式构建，另加每 30 秒诊断快照。
# 本脚本只会被 buildstorm_perf_probe.sh 写入 /tmp 镜像副本；
# Cargo 参数、/proc/uptime 计时和产物判定均与公开脚本一致。

echo "#### OS COMP TEST GROUP START buildstorm-glibc ####"

mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null

export PATH=/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin
export HOME=/root RUSTUP_HOME=/root/.rustup CARGO_HOME=/root/.cargo
export RUSTUP_TOOLCHAIN=nightly-2026-05-28
export CARGO_NET_OFFLINE=true
FINAL_RC=0

case "$(uname -m 2>/dev/null)" in
    loongarch64) AXARCH=loongarch64; AXTGT=loongarch64-unknown-linux-musl ;;
    riscv64) AXARCH=riscv64; AXTGT=riscv64gc-unknown-linux-musl ;;
    *) AXARCH=riscv64; AXTGT=riscv64gc-unknown-linux-musl ;;
esac

if rustc --version && cargo --version; then
    echo "BUILDSTORM_TOOLCHAIN ok"
else
    echo "BUILDSTORM_TOOLCHAIN fail"
    FINAL_RC=1
fi

rm -rf /tmp/minibuild
if cargo new --vcs none /tmp/minibuild >/dev/null 2>&1 \
    && (cd /tmp/minibuild && cargo build >/dev/null 2>&1) \
    && [ "$(/tmp/minibuild/target/debug/minibuild)" = "Hello, world!" ]; then
    echo "BUILDSTORM_MINIBUILD ok"
else
    echo "BUILDSTORM_MINIBUILD fail"
    FINAL_RC=1
fi

cd /work/tgoskits 2>/dev/null || {
    echo "BUILDSTORM_COMPILE mode=multi ok=false elapsed_s=0 cores=$(nproc) bytes=0 arch=$AXARCH"
    echo "BUILDSTORM_FORMAL_RESULT ok=false reason=missing-workdir"
    echo "#### OS COMP TEST GROUP END buildstorm-glibc ####"
    exit 1
}

# 与官方一致：只清理正式目标架构，tg-xtask 预构建不计分但计入 1h QEMU 生命周期。
rm -rf "target/$AXTGT"
echo "----- pre-build tg-xtask (untimed) -----"
cargo build -p tg-xtask 2>&1 || true

echo "----- build arceos-helloworld (timed, arch=$AXARCH) -----"
echo "BUILDSTORM_BEGIN mode=multi"

# 诊断计数仅在 PERF_DIAG=1 内核中存在。写入只是重置快照，
# 不修改 Cargo 负载或计时源。
if command -v f7ly-perf >/dev/null 2>&1; then
    f7ly-perf reset all 2>/dev/null || true
fi

PROFILE_DONE=/tmp/buildstorm_formal_profile.done
rm -f "$PROFILE_DONE"
(
    SAMPLE=0
    while [ ! -e "$PROFILE_DONE" ]; do
        UPTIME=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
        ARTIFACTS=$(find "target/$AXTGT" -type f 2>/dev/null | wc -l)
        echo "BUILDSTORM_PROFILE sample=$SAMPLE uptime=$UPTIME artifacts=$ARTIFACTS"
        ps -eo pid,ppid,stat,psr,pcpu,time,comm 2>/dev/null |
            awk 'NR == 1 || $7 == "cargo" || $7 == "rustc" || $7 == "cc" || $7 == "ld"'
        if command -v f7ly-perf >/dev/null 2>&1; then
            echo "BUILDSTORM_PERF_SNAPSHOT_BEGIN sample=$SAMPLE"
            f7ly-perf status --json 2>/dev/null
            f7ly-perf stat --interval-ms 10 --count 1 --json 2>/dev/null
            echo "BUILDSTORM_PERF_SNAPSHOT_END sample=$SAMPLE"
        fi
        SAMPLE=$((SAMPLE + 1))
        sleep 30
    done
) &
PROFILE_PID=$!

T0_EPOCH=$(date +%s)
T0=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
rm -f /work/.build.rc
BUILD_COMMAND=/tmp/f7ly-buildstorm-profile-command.sh
cat >"$BUILD_COMMAND" <<'F7LY_BUILD_COMMAND_EOF'
#!/bin/sh
{
    timeout 14400 cargo xtask arceos build -p arceos-helloworld --arch "$AXARCH" 2>&1
    echo $? >/work/.build.rc
} | awk -v start="$T0_EPOCH" '
    {
        print
        if ($0 ~ /^[[:space:]]*(Compiling|Checking|Building|Finished)/) {
            printf "BUILDSTORM_CRATE T+%ds %s\n", systime() - start, $0
        }
        fflush()
    }
' | tee /work/buildstorm.build.out
F7LY_BUILD_COMMAND_EOF
chmod 0700 "$BUILD_COMMAND"
export AXARCH T0_EPOCH
if command -v f7ly-perf >/dev/null 2>&1; then
    f7ly-perf top --backend auto --event cycles --frequency 100 --period 1000000 \
        --limit 20 -- /bin/sh "$BUILD_COMMAND"
else
    /bin/sh "$BUILD_COMMAND"
fi

touch "$PROFILE_DONE"
kill "$PROFILE_PID" 2>/dev/null || true
wait "$PROFILE_PID" 2>/dev/null || true

RC=$(cat /work/.build.rc 2>/dev/null || echo 1)
rm -f /work/.build.rc
T1=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
ELAPSED=$(awk "BEGIN{printf \"%.2f\", (\"$T1\"+0)-(\"$T0\"+0)}" 2>/dev/null)
[ -n "$ELAPSED" ] || ELAPSED=0

ART=$(find target -type f \( -name 'arceos-helloworld' -o -name 'helloworld' \) 2>/dev/null | head -1)
BYTES=0
[ -n "$ART" ] && BYTES=$(wc -c <"$ART")

if [ "$RC" -eq 0 ] && [ -n "$ART" ] && [ "$BYTES" -ge 500000 ]; then
    echo "BUILDSTORM_COMPILE mode=multi ok=true elapsed_s=$ELAPSED cores=$(nproc) bytes=$BYTES arch=$AXARCH"
else
    echo "BUILDSTORM_COMPILE mode=multi ok=false rc=$RC elapsed_s=$ELAPSED cores=$(nproc) bytes=$BYTES arch=$AXARCH"
    echo "----- buildstorm.build.out tail -----"
    tail -25 /work/buildstorm.build.out 2>/dev/null
    FINAL_RC=1
fi

if command -v f7ly-perf >/dev/null 2>&1; then
    echo "BUILDSTORM_PERF_FINAL_BEGIN"
    f7ly-perf status --json 2>/dev/null
    cat /proc/f7ly/perf/metrics 2>/dev/null
    cat /proc/f7ly/perf/syscalls 2>/dev/null
    echo "BUILDSTORM_PERF_FINAL_END"
fi
if [ "$FINAL_RC" -eq 0 ]; then
    echo "BUILDSTORM_FORMAL_RESULT ok=true"
else
    echo "BUILDSTORM_FORMAL_RESULT ok=false"
fi
echo "#### OS COMP TEST GROUP END buildstorm-glibc ####"
sync
exit "$FINAL_RC"
