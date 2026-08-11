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
#   PROBE_MODE=xtask|formal|progress|teardown|filecache
#                                      选择预设工作负载，默认 xtask。
#   PERF_DIAG=1                     构建独立的 *-perf 诊断内核。
#   SKIP_CAGENT=1                   定向调试时跳过 CAgent，默认保留官方顺序。
#   HOST_TIMEOUT=15m                 覆盖默认 40 分钟宿主超时。
#   CHECK_E2FSCK=1                   QEMU 结束后对临时镜像执行只读 e2fsck -fn。
#
# 输出：
#   logs/run/output_<arch>_<timestamp>_buildstorm-perf.txt
#   logs/run/output_<arch>_<timestamp>_buildstorm-perf-host-cpu.txt

set -euo pipefail

ARCH_NAME="${1:-riscv}"
PROBE_MODE="${PROBE_MODE:-xtask}"
case "${ARCH_NAME}" in
    riscv)
        MAKE_ARCH="riscv"
        MAKE_ALIAS="r"
        SOURCE_IMAGE="images/sdcard-rv-pub.img"
        KERNEL_FILENAME="kernel-rv"
        RUN_TARGET="run-riscv"
        ;;
    loongarch)
        MAKE_ARCH="loongarch"
        MAKE_ALIAS="l"
        SOURCE_IMAGE="images/sdcard-la-pub.img"
        KERNEL_FILENAME="kernel-la"
        RUN_TARGET="run-loongarch"
        ;;
    *)
        echo "用法: $0 {riscv|loongarch}" >&2
        exit 2
        ;;
esac

case "${PROBE_MODE}" in
    xtask) DEFAULT_GUEST_SCRIPT="scripts/run/buildstorm_perf_guest.sh" ;;
    formal) DEFAULT_GUEST_SCRIPT="scripts/run/buildstorm_formal_profile_guest.sh" ;;
    progress) DEFAULT_GUEST_SCRIPT="scripts/run/buildstorm_progress_guest.sh" ;;
    teardown) DEFAULT_GUEST_SCRIPT="scripts/run/mm_teardown_bench_guest.sh" ;;
    filecache) DEFAULT_GUEST_SCRIPT="scripts/run/file_page_cache_bench_guest.sh" ;;
    *)
        echo "PROBE_MODE 必须是 xtask、formal、progress、teardown 或 filecache" >&2
        exit 2
        ;;
esac

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "${REPO_ROOT}"
SOURCE_IMAGE="${SOURCE_IMAGE_OVERRIDE:-${SOURCE_IMAGE}}"
GUEST_SCRIPT="${GUEST_SCRIPT_OVERRIDE:-${DEFAULT_GUEST_SCRIPT}}"
if [[ "${PROBE_MODE}" == "formal" ]]; then
    HOST_TIMEOUT="${HOST_TIMEOUT:-60m}"
elif [[ "${PROBE_MODE}" == "teardown" || "${PROBE_MODE}" == "filecache" ]]; then
    HOST_TIMEOUT="${HOST_TIMEOUT:-20m}"
else
    HOST_TIMEOUT="${HOST_TIMEOUT:-40m}"
fi
SAMPLE_INTERVAL="${SAMPLE_INTERVAL:-30}"

REQUIRED_TOOLS="awk debugfs grep rg timeout make tr"
if [[ "${CHECK_E2FSCK:-0}" == "1" ]]; then
    REQUIRED_TOOLS="${REQUIRED_TOOLS} e2fsck"
fi
for tool in ${REQUIRED_TOOLS}; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "缺少必需工具: ${tool}" >&2
        exit 1
    fi
done

if pgrep -f 'qemu-system-(riscv64|loongarch64)' >/dev/null 2>&1; then
    echo "检测到其它 QEMU 正在运行；为避免污染性能结果，本次探针拒绝启动。" >&2
    exit 3
fi

if [[ ! -f "${SOURCE_IMAGE}" ]]; then
    echo "缺少评测镜像: ${SOURCE_IMAGE}" >&2
    exit 1
fi
if [[ ! -f "${GUEST_SCRIPT}" ]]; then
    echo "缺少 guest 探针脚本: ${GUEST_SCRIPT}" >&2
    exit 1
fi

if [[ "${PROBE_MODE}" == "filecache" && "${PERF_DIAG:-0}" != "1" ]]; then
    echo "filecache 探针依赖 /proc/f7ly/perf；请显式设置 PERF_DIAG=1。" >&2
    exit 2
fi

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    make build "ARCH=${MAKE_ARCH}" "PERF_DIAG=${PERF_DIAG:-0}"
fi
if [[ "${PERF_DIAG:-0}" == "1" ]]; then
    KERNEL_FILENAME="${KERNEL_FILENAME}-perf"
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

if [[ "${SKIP_CAGENT:-0}" == "1" ]]; then
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
E2FSCK_LOG_PATH="logs/run/output_${MAKE_ALIAS}_${TIMESTAMP}_buildstorm-perf-e2fsck.txt"

echo "[3/4] 启动 8 vCPU/8 GiB QEMU"
# 默认每 30 秒记录一次 QEMU 线程的宿主 CPU/落核信息。采样只读 /proc，
# 开销远低于 guest 编译负载，用于区分“8 个空闲 vCPU 自旋”与 rustc 真并行。
(
    while true; do
        QEMU_PID=$(pgrep -n -f 'qemu-system-(riscv64|loongarch64)' || true)
        if [[ -n "${QEMU_PID}" ]]; then
            echo "HOST_CPU_SAMPLE epoch=$(date +%s) qemu_pid=${QEMU_PID}"
            ps -L -p "${QEMU_PID}" -o tid=,psr=,pcpu=,stat=,comm=
        fi
        sleep "${SAMPLE_INTERVAL}"
    done
) >"${HOST_CPU_LOG_PATH}" 2>&1 &
SAMPLER_PID=$!

set +e
timeout "${HOST_TIMEOUT}" make "${RUN_TARGET}" \
    "ARCH=${MAKE_ARCH}" \
    INITCODE_MODE=evaluation \
    "KERNEL_ELF=${KERNEL_PATH}" \
    QEMU_MEM=8G \
    QEMU_SMP=8 \
    QEMU_SNAPSHOT= \
    "QEMU_STORAGE_IMAGE=${TEMP_IMAGE}" \
    "INITRD_IMAGE=${REPO_ROOT}/images/initrd.img" >"${LOG_PATH}" 2>&1
RUN_STATUS=$?
set -e
kill "${SAMPLER_PID}" >/dev/null 2>&1 || true
wait "${SAMPLER_PID}" 2>/dev/null || true
SAMPLER_PID=""

E2FSCK_STATUS=0
if [[ "${CHECK_E2FSCK:-0}" == "1" ]]; then
    set +e
    e2fsck -fn "${TEMP_IMAGE}" >"${E2FSCK_LOG_PATH}" 2>&1
    E2FSCK_STATUS=$?
    set -e
fi

echo "[4/4] 性能摘要"
tr '\r' '\n' <"${LOG_PATH}" |
    rg 'BUILDSTORM_(PERF_(BEGIN|T\\+|END)|PROGRESS|PROFILE|COMPILE|FORMAL_RESULT)|MM_TEARDOWN_(SAMPLE|RESULT|PERF_(BEGIN|END))|FILE_CACHE_(ROUND|TRUNCATE|RESULT|PERF_(BEGIN|END))|panic: |shootdown timeout' |
    tail -n 80 || true
echo "日志: ${LOG_PATH}"
echo "宿主 CPU 采样: ${HOST_CPU_LOG_PATH}"
if [[ "${CHECK_E2FSCK:-0}" == "1" ]]; then
    echo "只读 e2fsck: status=${E2FSCK_STATUS} log=${E2FSCK_LOG_PATH}"
fi
PROBE_STATUS=0
if grep -aF 'panic: ' "${LOG_PATH}" >/dev/null; then
    echo "探针失败: 检测到内核 panic。" >&2
    PROBE_STATUS=4
fi
if grep -aF 'shootdown timeout' "${LOG_PATH}" >/dev/null; then
    echo "探针失败: 检测到 TLB shootdown timeout。" >&2
    PROBE_STATUS=4
fi

# 正式、teardown 与 filecache 都有唯一的结构化终态。QEMU 正常关机并不
# 表示 guest 测试成功，因此必须独立拒绝缺失、重复或 ok=false 的结果。
RESULT_PREFIX=""
case "${PROBE_MODE}" in
    formal) RESULT_PREFIX="BUILDSTORM_FORMAL_RESULT" ;;
    teardown) RESULT_PREFIX="MM_TEARDOWN_RESULT" ;;
    filecache) RESULT_PREFIX="FILE_CACHE_RESULT" ;;
esac
if [[ -n "${RESULT_PREFIX}" ]]; then
    RESULT_COUNT=$(tr '\r' '\n' <"${LOG_PATH}" |
        awk -v prefix="${RESULT_PREFIX}" '
            $0 ~ "^[[:space:]]*" prefix "[[:space:]]" { count++ }
            END { print count + 0 }
        ')
    RESULT_LINE=$(tr '\r' '\n' <"${LOG_PATH}" |
        awk -v prefix="${RESULT_PREFIX}" '
            $0 ~ "^[[:space:]]*" prefix "[[:space:]]" { result = $0 }
            END { print result }
        ')
    if [[ "${RESULT_COUNT}" -ne 1 ]]; then
        echo "探针失败: ${RESULT_PREFIX} 终态数量应为 1，实际为 ${RESULT_COUNT}。" >&2
        PROBE_STATUS=4
    elif [[ " ${RESULT_LINE} " != *" ok=true "* ]]; then
        echo "探针失败: ${RESULT_LINE}" >&2
        PROBE_STATUS=4
    elif [[ "${PROBE_MODE}" == "filecache" && " ${RESULT_LINE} " != *" diagnostic=true "* ]]; then
        echo "探针失败: filecache 结果缺少 diagnostic=true: ${RESULT_LINE}" >&2
        PROBE_STATUS=4
    fi
fi

if [[ "${RUN_STATUS}" -ne 0 ]]; then
    exit "${RUN_STATUS}"
fi
if [[ "${E2FSCK_STATUS}" -ne 0 ]]; then
    exit "${E2FSCK_STATUS}"
fi
if [[ "${PROBE_STATUS}" -ne 0 ]]; then
    exit "${PROBE_STATUS}"
fi
exit 0
