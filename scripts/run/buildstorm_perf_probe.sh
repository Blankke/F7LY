#!/bin/bash
#
# 在不改写原始评测镜像的前提下运行 BuildStorm tg-xtask 固定时长性能探针。
#
# 使用示例：
#   bash scripts/run/buildstorm_perf_probe.sh riscv
#   bash scripts/run/buildstorm_perf_probe.sh loongarch
#
# 环境变量：
#   SKIP_BUILD=1                     复用已经构建的 kernel-rv/kernel-la。
#   KERNEL_ELF_OVERRIDE=/abs/kernel  运行指定内核，便于和基线 worktree 对比。
#   SOURCE_IMAGE_OVERRIDE=/abs/img   使用指定评测镜像。
#   GUEST_SCRIPT_OVERRIDE=/abs/file  写入其它 guest 诊断入口。
#   HOST_TIMEOUT=15m                 覆盖默认 40 分钟宿主超时。
#
# 输出：
#   logs/run/output_<arch>_<timestamp>_buildstorm-perf.txt
#   logs/run/output_<arch>_<timestamp>_buildstorm-perf-host-cpu.txt

set -euo pipefail

ARCH_NAME="${1:-riscv}"
case "${ARCH_NAME}" in
    riscv)
        MAKE_PROFILE="riscv-qemu"
        MAKE_ALIAS="r"
        IMAGE_KIND="riscv-evaluation"
        SOURCE_IMAGE="images/oscomp-preliminary-riscv64.img"
        KERNEL_FILENAME="kernel-rv"
        ;;
    loongarch)
        MAKE_PROFILE="loongarch-qemu"
        MAKE_ALIAS="l"
        IMAGE_KIND="loongarch-evaluation"
        SOURCE_IMAGE="images/oscomp-preliminary-loongarch64.img"
        KERNEL_FILENAME="kernel-la"
        ;;
    *)
        echo "用法: $0 {riscv|loongarch}" >&2
        exit 2
        ;;
esac

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "${REPO_ROOT}"
SOURCE_IMAGE="${SOURCE_IMAGE_OVERRIDE:-${SOURCE_IMAGE}}"
GUEST_SCRIPT="${GUEST_SCRIPT_OVERRIDE:-scripts/run/buildstorm_perf_guest.sh}"
HOST_TIMEOUT="${HOST_TIMEOUT:-40m}"

for tool in debugfs timeout make; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "缺少必需工具: ${tool}" >&2
        exit 1
    fi
done

if pgrep -f 'qemu-system-(riscv64|loongarch64)' >/dev/null 2>&1; then
    echo "检测到其它 QEMU 正在运行；为避免污染性能结果，本次探针拒绝启动。" >&2
    exit 3
fi

# 与 make run 共用同一套“工作副本 -> bak -> 官方下载”规则；显式覆盖的
# SOURCE_IMAGE 只会被校验，不会被默认镜像替换。
scripts/images/prepare-qemu-image.sh "${IMAGE_KIND}" "${SOURCE_IMAGE}"
if [[ ! -f "${GUEST_SCRIPT}" ]]; then
    echo "缺少 guest 探针脚本: ${GUEST_SCRIPT}" >&2
    exit 1
fi

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    make build "PROFILE=${MAKE_PROFILE}"
fi
KERNEL_PATH="${KERNEL_ELF_OVERRIDE:-${REPO_ROOT}/${KERNEL_FILENAME}}"
if [[ ! -f "${KERNEL_PATH}" ]]; then
    echo "缺少待测内核: ${KERNEL_PATH}" >&2
    exit 1
fi

TEMP_DIR=$(mktemp -d "/tmp/f7ly-buildstorm-perf-${ARCH_NAME}.XXXXXX")
TEMP_IMAGE="${TEMP_DIR}/sdcard.img"
SAMPLER_PID=""
cleanup() {
    if [[ -n "${SAMPLER_PID}" ]]; then
        kill "${SAMPLER_PID}" >/dev/null 2>&1 || true
        wait "${SAMPLER_PID}" 2>/dev/null || true
        SAMPLER_PID=""
    fi
    # TEMP_DIR 由本脚本刚刚用 mktemp 创建，只包含这一份临时镜像。
    # 使用精确 unlink+rmdir，避免任何递归删除误伤工作区或原始镜像。
    if [[ -f "${TEMP_IMAGE}" ]]; then
        unlink "${TEMP_IMAGE}"
    fi
    rmdir "${TEMP_DIR}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[1/4] 复制临时评测镜像（原镜像保持只读）"
cp --reflink=auto "${SOURCE_IMAGE}" "${TEMP_IMAGE}"

echo "[2/4] 写入 guest 构建探针: ${GUEST_SCRIPT}"
debugfs -w -R 'unlink /glibc/buildstorm_testcode.sh' "${TEMP_IMAGE}" >/dev/null 2>&1
debugfs -w -R \
    "write ${GUEST_SCRIPT} /glibc/buildstorm_testcode.sh" \
    "${TEMP_IMAGE}" >/dev/null
debugfs -w -R \
    'set_inode_field /glibc/buildstorm_testcode.sh mode 0100755' \
    "${TEMP_IMAGE}" >/dev/null

if [[ "${ARCH_NAME}" == "loongarch" ]]; then
    debugfs -w -R 'unlink /glibc/cagent_testcode.sh' "${TEMP_IMAGE}" >/dev/null 2>&1
    debugfs -w -R \
        "write scripts/run/buildstorm_perf_skip_cagent.sh /glibc/cagent_testcode.sh" \
        "${TEMP_IMAGE}" >/dev/null
    debugfs -w -R \
        'set_inode_field /glibc/cagent_testcode.sh mode 0100755' \
        "${TEMP_IMAGE}" >/dev/null
fi

mkdir -p logs/run
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
LOG_PATH="logs/run/output_${MAKE_ALIAS}_${TIMESTAMP}_buildstorm-perf.txt"
HOST_CPU_LOG_PATH="logs/run/output_${MAKE_ALIAS}_${TIMESTAMP}_buildstorm-perf-host-cpu.txt"

echo "[3/4] 启动 8 vCPU/8 GiB QEMU"
# 每 10 秒记录一次 QEMU 线程的宿主 CPU/落核信息。采样只读 /proc，
# 开销远低于 guest 编译负载，用于区分“8 个空闲 vCPU 自旋”与 rustc 真并行。
(
    while true; do
        QEMU_PID=$(pgrep -n -f 'qemu-system-(riscv64|loongarch64)' || true)
        if [[ -n "${QEMU_PID}" ]]; then
            echo "HOST_CPU_SAMPLE epoch=$(date +%s) qemu_pid=${QEMU_PID}"
            ps -L -p "${QEMU_PID}" -o tid=,psr=,pcpu=,stat=,comm=
        fi
        sleep 10
    done
) >"${HOST_CPU_LOG_PATH}" 2>&1 &
SAMPLER_PID=$!

set +e
timeout "${HOST_TIMEOUT}" make qemu-run \
    "PROFILE=${MAKE_PROFILE}" \
    MODE=evaluation \
    "KERNEL_ELF=${KERNEL_PATH}" \
    QEMU_MEM=8G \
    QEMU_SMP=8 \
    QEMU_SNAPSHOT= \
    "QEMU_STORAGE_IMAGE=${TEMP_IMAGE}" >"${LOG_PATH}" 2>&1
RUN_STATUS=$?
set -e
kill "${SAMPLER_PID}" >/dev/null 2>&1 || true
wait "${SAMPLER_PID}" 2>/dev/null || true
SAMPLER_PID=""

echo "[4/4] 性能摘要"
tr '\r' '\n' <"${LOG_PATH}" |
    rg 'BUILDSTORM_(PERF_(BEGIN|T\\+|END)|PROGRESS)|panic|shootdown timeout' |
    tail -n 80 || true
echo "日志: ${LOG_PATH}"
echo "宿主 CPU 采样: ${HOST_CPU_LOG_PATH}"
exit "${RUN_STATUS}"
