#!/usr/bin/env bash
#
# 在 F7LY guest 内执行 Cargo/ext4 分级压力并从空 target 编译 StarryOS。
#
# 使用示例（通常由宿主 self_compile.sh 自动调用）：
#   SELFBUILD_LEVEL=4 CARGO_BUILD_JOBS=8 /usr/bin/f7ly-self-compile
#   SELFBUILD_LEVEL=3 SELFBUILD_L3_JOBS=1 /usr/bin/f7ly-self-compile
#
# 分级：L0=CPU/内存/磁盘，L1=ext4 小文件，L2=最小 Rust crate，
#       L3=ax-hal 单 package，L4=完整 starryos package，
#       L5=空 target Cargo 与 ext4/进程并发长压力。

set -euo pipefail

LEVEL="${SELFBUILD_LEVEL:-4}"
JOBS="${CARGO_BUILD_JOBS:-8}"
L3_JOBS="${SELFBUILD_L3_JOBS:-1}"
EXPECTED_CPUS="${SELFBUILD_EXPECTED_CPUS:-8}"
MIN_MEMORY_KB="${SELFBUILD_MIN_MEMORY_KB:-7340032}"
L1_FILES="${SELFBUILD_L1_FILES:-1024}"
L5_SECONDS="${SELFBUILD_L5_SECONDS:-1800}"
SOURCE_ROOT="${SELFBUILD_SOURCE_ROOT:-/opt/starryos}"
TARGET="riscv64gc-unknown-none-elf"
ARTIFACT_DIR="/opt/f7ly-selfbuild-artifacts"
MINIMAL_ROOT="/opt/f7ly-selfbuild-minimal"
MINIMAL_TARGET="/opt/f7ly-selfbuild-target-l2"
L3_TARGET="/opt/f7ly-selfbuild-target-l3"
L4_TARGET="/opt/f7ly-selfbuild-target-l4"
L5_TARGET="/opt/f7ly-selfbuild-target-l5"

CURRENT_STAGE="bootstrap"
HEARTBEAT_PID=""
SOURCE_BACKED_UP=0
AXALLOC_BACKED_UP=0
SUCCEEDED=0

info() { printf '[f7ly-selfbuild] %s\n' "$*"; }
die() { printf '[f7ly-selfbuild] 错误：%s\n' "$*" >&2; exit 1; }
is_positive_integer() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }
is_level() { [[ "$1" =~ ^[0-5]$ ]]; }

stop_heartbeat() {
    if [[ -n "${HEARTBEAT_PID}" ]]; then
        kill "${HEARTBEAT_PID}" >/dev/null 2>&1 || true
        wait "${HEARTBEAT_PID}" >/dev/null 2>&1 || true
        HEARTBEAT_PID=""
    fi
}

start_heartbeat() {
    stop_heartbeat
    (
        while sleep 30; do
            printf '[f7ly-selfbuild] HEARTBEAT stage=%s uptime=%s free_kb=%s\n' \
                "${CURRENT_STAGE}" \
                "$(cut -d' ' -f1 /proc/uptime 2>/dev/null || printf unknown)" \
                "$(awk '/MemAvailable:/{print $2; exit}' /proc/meminfo 2>/dev/null || printf unknown)"
        done
    ) &
    HEARTBEAT_PID=$!
}

restore_source() {
    set +e
    if ((AXALLOC_BACKED_UP == 1)); then
        mv -f "${SOURCE_ROOT}/os/arceos/modules/axalloc/Cargo.toml.f7ly-original" \
            "${SOURCE_ROOT}/os/arceos/modules/axalloc/Cargo.toml"
    fi
    if ((SOURCE_BACKED_UP == 1)); then
        mv -f "${SOURCE_ROOT}/Cargo.toml.f7ly-original" "${SOURCE_ROOT}/Cargo.toml"
        rm -f "${SOURCE_ROOT}/Cargo.toml.bak"
    fi
}

on_exit() {
    local rc=$?
    trap - EXIT
    stop_heartbeat
    restore_source
    sync >/dev/null 2>&1 || true
    if ((rc != 0 && SUCCEEDED == 0)); then
        printf 'SELF_COMPILE_FAILED stage=%s rc=%d\n' "${CURRENT_STAGE}" "${rc}"
    fi
    exit "${rc}"
}
trap on_exit EXIT
trap 'exit 130' INT TERM

is_level "${LEVEL}" || die "SELFBUILD_LEVEL 必须是 0..5"
is_positive_integer "${JOBS}" || die "CARGO_BUILD_JOBS 必须是正整数"
is_positive_integer "${L3_JOBS}" || die "SELFBUILD_L3_JOBS 必须是正整数"
is_positive_integer "${EXPECTED_CPUS}" || die "SELFBUILD_EXPECTED_CPUS 必须是正整数"
is_positive_integer "${MIN_MEMORY_KB}" || die "SELFBUILD_MIN_MEMORY_KB 必须是正整数"
is_positive_integer "${L1_FILES}" || die "SELFBUILD_L1_FILES 必须是正整数"
is_positive_integer "${L5_SECONDS}" || die "SELFBUILD_L5_SECONDS 必须是正整数"
[[ -f "${SOURCE_ROOT}/Cargo.toml" ]] || die "缺少源码：${SOURCE_ROOT}/Cargo.toml"

export PATH="/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export RUSTUP_HOME="/root/.rustup"
export CARGO_HOME="/root/.cargo"
export CARGO_NET_OFFLINE=true
export CARGO_TERM_PROGRESS_WHEN=always
export CARGO_TERM_PROGRESS_WIDTH=120

CURRENT_STAGE="L0"
info "BEGIN stage=L0"
source_commit="$(tr -d '[:space:]' <"${SOURCE_ROOT}/.source-commit" 2>/dev/null || printf unknown)"
online_cpus="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || printf 0)"
memory_kb="$(awk '/MemTotal:/{print $2; exit}' /proc/meminfo 2>/dev/null || printf 0)"
printf 'SELFBUILD_SOURCE_COMMIT=%s\n' "${source_commit}"
printf 'SELFBUILD_CPUS_ONLINE=%s\n' "${online_cpus}"
printf 'SELFBUILD_MEMORY_KB=%s\n' "${memory_kb}"
printf 'SELFBUILD_CARGO_JOBS=%s\n' "${JOBS}"
cat /sys/devices/system/cpu/online 2>/dev/null || true
head -n 24 /proc/meminfo 2>/dev/null || true
df -h / "${SOURCE_ROOT}" 2>/dev/null || true
cargo --version || die "cargo 无法运行"
rustc --version --verbose || die "rustc 无法运行"
[[ "${online_cpus}" =~ ^[0-9]+$ ]] || die "无法读取在线 CPU 数：${online_cpus}"
[[ "${memory_kb}" =~ ^[0-9]+$ ]] || die "无法读取 MemTotal：${memory_kb}"
((online_cpus >= EXPECTED_CPUS)) || die "在线 CPU 不足：${online_cpus} < ${EXPECTED_CPUS}"
((memory_kb >= MIN_MEMORY_KB)) || die "可见内存不足：${memory_kb} KiB < ${MIN_MEMORY_KB} KiB"
info "PASS stage=L0"

if ((LEVEL >= 1)); then
    CURRENT_STAGE="L1"
    info "BEGIN stage=L1 files=${L1_FILES}"
    ext4_probe="/opt/f7ly-ext4-probe"
    rm -rf "${ext4_probe}"
    mkdir -p "${ext4_probe}"
    for ((i = 1; i <= L1_FILES; ++i)); do
        bucket=$((i % 64))
        mkdir -p "${ext4_probe}/${bucket}"
        printf 'f7ly-ext4-record-%08d-%08x\n' "${i}" "$((i * 2654435761 & 0xffffffff))" \
            >"${ext4_probe}/${bucket}/file-${i}"
    done
    created="$(find "${ext4_probe}" -type f | wc -l)"
    [[ "${created}" -eq "${L1_FILES}" ]] || die "小文件数量不一致：${created} != ${L1_FILES}"
    sync
    for ((i = 1; i <= L1_FILES; i += 97)); do
        bucket=$((i % 64))
        expected="$(printf 'f7ly-ext4-record-%08d-%08x' "${i}" "$((i * 2654435761 & 0xffffffff))")"
        actual="$(tr -d '\r\n' <"${ext4_probe}/${bucket}/file-${i}")"
        [[ "${actual}" == "${expected}" ]] || die "小文件校验失败：file-${i}"
    done
    rm -rf "${ext4_probe}"
    sync
    printf 'EXT4_SMALL_FILE_PASS files=%s\n' "${L1_FILES}"
    info "PASS stage=L1"
fi

if ((LEVEL >= 2)); then
    CURRENT_STAGE="L2"
    info "BEGIN stage=L2"
    rm -rf "${MINIMAL_ROOT}" "${MINIMAL_TARGET}"
    mkdir -p "${MINIMAL_ROOT}/src"
    cat >"${MINIMAL_ROOT}/Cargo.toml" <<'EOF'
[package]
name = "f7ly-selfbuild-probe"
version = "0.1.0"
edition = "2021"

[dependencies]
EOF
    cat >"${MINIMAL_ROOT}/src/main.rs" <<'EOF'
fn main() {
    let sum: u64 = (0..1_000_000_u64).map(|value| value ^ 0x5a5a).sum();
    println!("F7LY_MINIMAL_RUST_PASS sum={sum}");
}
EOF
    rust_host="$(rustc --version --verbose | awk '/^host:/{print $2; exit}')"
    [[ -n "${rust_host}" ]] || die "无法读取 rustc host triple"
    CARGO_TARGET_DIR="${MINIMAL_TARGET}" RUSTFLAGS='' \
        cargo build --manifest-path "${MINIMAL_ROOT}/Cargo.toml" \
            --target "${rust_host}" --offline --jobs "${JOBS}"
    minimal_binary="${MINIMAL_TARGET}/${rust_host}/debug/f7ly-selfbuild-probe"
    [[ -s "${minimal_binary}" ]] || die "最小 Rust 产物不存在"
    "${minimal_binary}" | grep -q 'F7LY_MINIMAL_RUST_PASS' || die "最小 Rust 产物运行失败"
    info "PASS stage=L2"
fi

prepare_starry_source() {
    cd "${SOURCE_ROOT}"
    rm -f Cargo.toml.bak Cargo.toml.f7ly-original
    cp Cargo.toml Cargo.toml.f7ly-original
    SOURCE_BACKED_UP=1
    /usr/bin/filter-workspace.sh riscv64 Cargo.toml

    axalloc_manifest="${SOURCE_ROOT}/os/arceos/modules/axalloc/Cargo.toml"
    [[ -f "${axalloc_manifest}" ]] || die "缺少 axalloc Cargo.toml"
    rm -f "${axalloc_manifest}.f7ly-original"
    cp "${axalloc_manifest}" "${axalloc_manifest}.f7ly-original"
    AXALLOC_BACKED_UP=1
    sed -i '/^default = /s/page-alloc-4g/page-alloc-64g/g' "${axalloc_manifest}"
    [[ -f "${SOURCE_ROOT}/linker.x" ]] || die "缺少宿主注入的 ${SOURCE_ROOT}/linker.x"
}

if ((LEVEL >= 3)); then
    prepare_starry_source
    export RUSTFLAGS='-Ccodegen-units=16 -Copt-level=0 -Cincremental=false -Clink-arg=-Tlinker.x -Clink-arg=-no-pie -Clink-arg=-znostart-stop-gc'

    CURRENT_STAGE="L3"
    info "BEGIN stage=L3 package=ax-hal jobs=${L3_JOBS}"
    rm -rf "${L3_TARGET}"
    start_heartbeat
    CARGO_TARGET_DIR="${L3_TARGET}" CARGO_BUILD_JOBS="${L3_JOBS}" \
        cargo check --ignore-rust-version -p ax-hal \
            --target "${TARGET}" --features smp,irq,uspace \
            --offline --jobs "${L3_JOBS}"
    stop_heartbeat
    info "PASS stage=L3"
fi

if ((LEVEL >= 4)); then
    CURRENT_STAGE="L4"
    info "BEGIN stage=L4 package=starryos jobs=${JOBS}"
    rm -rf "${L4_TARGET}"
    start_heartbeat
    CARGO_TARGET_DIR="${L4_TARGET}" CARGO_BUILD_JOBS="${JOBS}" \
        cargo build --ignore-rust-version -p starryos \
            --target "${TARGET}" \
            --features qemu,ax-driver/virtio-blk,ax-driver/virtio-net,ax-driver/virtio-gpu,ax-driver/virtio-input,ax-driver/virtio-socket \
            --offline --jobs "${JOBS}"
    stop_heartbeat

    starry_binary="${L4_TARGET}/${TARGET}/debug/starryos"
    [[ -s "${starry_binary}" ]] || die "完整 StarryOS 产物不存在：${starry_binary}"
    mkdir -p "${ARTIFACT_DIR}"
    cp "${starry_binary}" "${ARTIFACT_DIR}/starryos-riscv64"
    printf '%s\n' "${source_commit}" >"${ARTIFACT_DIR}/source-commit"
    stat -c 'SELFBUILD_ARTIFACT_SIZE=%s' "${ARTIFACT_DIR}/starryos-riscv64"
    sync
    info "PASS stage=L4"
fi

if ((LEVEL >= 5)); then
    CURRENT_STAGE="L5"
    info "BEGIN stage=L5 seconds=${L5_SECONDS} jobs=${JOBS}"
    start_heartbeat
    l5_started="$(date +%s)"
    l5_deadline=$((l5_started + L5_SECONDS))
    l5_iteration=0
    l5_churn_root="/opt/f7ly-buildstorm-churn"

    while (( $(date +%s) < l5_deadline )); do
        l5_iteration=$((l5_iteration + 1))
        rm -rf "${L5_TARGET}" "${l5_churn_root}"
        mkdir -p "${l5_churn_root}"

        # 文件 churn 与 rustc/cargo 子进程并行，覆盖 BuildStorm 最容易触发的
        # ext4 目录项、并发进程、pipe/futex、mmap 和页回收组合。
        (
            for ((worker = 0; worker < JOBS; ++worker)); do
                (
                    worker_dir="${l5_churn_root}/worker-${worker}"
                    mkdir -p "${worker_dir}"
                    for ((file_index = worker + 1; file_index <= L1_FILES; file_index += JOBS)); do
                        printf 'iteration=%d worker=%d file=%d value=%08x\n' \
                            "${l5_iteration}" "${worker}" "${file_index}" \
                            "$((file_index * 2654435761 & 0xffffffff))" \
                            >"${worker_dir}/file-${file_index}"
                    done
                ) &
            done
            wait
            churn_count="$(find "${l5_churn_root}" -type f | wc -l)"
            [[ "${churn_count}" -eq "${L1_FILES}" ]]
            sync
        ) &
        churn_pid=$!

        cargo_rc=0
        CARGO_TARGET_DIR="${L5_TARGET}" CARGO_BUILD_JOBS="${JOBS}" \
            cargo check --ignore-rust-version -p ax-hal \
                --target "${TARGET}" --features smp,irq,uspace \
                --offline --jobs "${JOBS}" || cargo_rc=$?
        churn_rc=0
        wait "${churn_pid}" || churn_rc=$?
        ((cargo_rc == 0)) || die "L5 Cargo 失败：iteration=${l5_iteration} rc=${cargo_rc}"
        ((churn_rc == 0)) || die "L5 ext4 churn 失败：iteration=${l5_iteration} rc=${churn_rc}"

        rm -rf "${l5_churn_root}"
        sync
        printf 'BUILDSTORM_LONG_ITERATION_PASS iteration=%d elapsed=%d\n' \
            "${l5_iteration}" "$(( $(date +%s) - l5_started ))"
    done

    stop_heartbeat
    l5_elapsed=$(( $(date +%s) - l5_started ))
    ((l5_elapsed >= L5_SECONDS)) || die "L5 实际压力时长不足：${l5_elapsed} < ${L5_SECONDS}"
    printf 'BUILDSTORM_LONG_STRESS_PASS seconds=%d iterations=%d\n' \
        "${l5_elapsed}" "${l5_iteration}"
    info "PASS stage=L5"
fi

SUCCEEDED=1
printf 'SELF_COMPILE_SUCCESS level=%s jobs=%s source=%s\n' "${LEVEL}" "${JOBS}" "${source_commit}"
