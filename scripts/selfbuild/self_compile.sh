#!/usr/bin/env bash
#
# 用 F7LY RV64（8 CPU、8 GiB）启动 selfhost rootfs，并在 guest 内编译 StarryOS。
#
# 使用示例：
#   scripts/selfbuild/self_compile.sh
#   scripts/selfbuild/self_compile.sh --level 3 --jobs 1 --timeout-seconds 3600
#   scripts/selfbuild/self_compile.sh --level 5 --l5-seconds 1800
#   scripts/selfbuild/self_compile.sh --skip-f7ly-build --skip-starry-seed-build
#
# 每次运行都在 build/selfbuild/runs 下创建可写 rootfs 副本；原始准备镜像不变。
# 串口日志、参数和 e2fsck 结果保存在 logs/run/selfbuild-rv-* 下。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

ROOTFS="${PROJECT_ROOT}/build/selfbuild/rootfs-riscv64-debian-selfhost-f7ly.img"
TGOSKITS="${HOME}/tgoskits"
LINKER_X=""
SMP=8
JOBS=8
L3_JOBS=1
MEMORY="8G"
LEVEL=4
L1_FILES=1024
L5_SECONDS=1800
MIN_MEMORY_KB=7340032
BOOT_WAIT_SECONDS=20
TIMEOUT_SECONDS=10800
SKIP_F7LY_BUILD=0
SKIP_STARRY_SEED_BUILD=0

info() { printf '[self-compile] %s\n' "$*"; }
die() { printf '[self-compile] 错误：%s\n' "$*" >&2; exit 1; }
is_positive_integer() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }

usage() {
    cat <<'EOF'
用法：scripts/selfbuild/self_compile.sh [选项]

选项：
  --rootfs PATH              prepare_rootfs.sh 生成的工作镜像
  --tgoskits PATH            tgoskits 仓库路径，默认 ~/tgoskits
  --linker-x PATH            已生成的 RISC-V StarryOS linker.x
  --smp N                    QEMU vCPU，默认 8，最大 8
  --jobs N                   L2/L4 Cargo 并行度，默认 8
  --l3-jobs N                L3 ax-hal 并行度，默认 1
  --mem SIZE                 QEMU 内存，默认 8G
  --level N                  运行到 L0..L5，默认 L4
  --l1-files N               L1 ext4 小文件数，默认 1024
  --l5-seconds N             L5 Cargo/ext4 长压力秒数，默认 1800
  --boot-wait-seconds N      发送命令前等待时间，默认 20
  --timeout-seconds N        QEMU 总超时，默认 10800（3 小时）
  --skip-f7ly-build          复用现有 kernel-rv-shell
  --skip-starry-seed-build   不运行宿主 Starry seed build，仅复用现有 linker.x
  -h, --help                 显示帮助
EOF
}

while (($# > 0)); do
    case "$1" in
        --rootfs)
            (($# >= 2)) || die "--rootfs 缺少参数"
            ROOTFS="$2"
            shift 2
            ;;
        --tgoskits)
            (($# >= 2)) || die "--tgoskits 缺少参数"
            TGOSKITS="$2"
            shift 2
            ;;
        --linker-x)
            (($# >= 2)) || die "--linker-x 缺少参数"
            LINKER_X="$2"
            shift 2
            ;;
        --smp)
            (($# >= 2)) || die "--smp 缺少参数"
            SMP="$2"
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || die "--jobs 缺少参数"
            JOBS="$2"
            shift 2
            ;;
        --l3-jobs)
            (($# >= 2)) || die "--l3-jobs 缺少参数"
            L3_JOBS="$2"
            shift 2
            ;;
        --mem)
            (($# >= 2)) || die "--mem 缺少参数"
            MEMORY="$2"
            shift 2
            ;;
        --level)
            (($# >= 2)) || die "--level 缺少参数"
            LEVEL="$2"
            shift 2
            ;;
        --l1-files)
            (($# >= 2)) || die "--l1-files 缺少参数"
            L1_FILES="$2"
            shift 2
            ;;
        --l5-seconds)
            (($# >= 2)) || die "--l5-seconds 缺少参数"
            L5_SECONDS="$2"
            shift 2
            ;;
        --boot-wait-seconds)
            (($# >= 2)) || die "--boot-wait-seconds 缺少参数"
            BOOT_WAIT_SECONDS="$2"
            shift 2
            ;;
        --timeout-seconds)
            (($# >= 2)) || die "--timeout-seconds 缺少参数"
            TIMEOUT_SECONDS="$2"
            shift 2
            ;;
        --skip-f7ly-build)
            SKIP_F7LY_BUILD=1
            shift
            ;;
        --skip-starry-seed-build)
            SKIP_STARRY_SEED_BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "未知参数：$1"
            ;;
    esac
done

is_positive_integer "${SMP}" || die "--smp 必须是正整数"
((SMP <= 8)) || die "--smp 不能超过 F7LY NCPU=8"
is_positive_integer "${JOBS}" || die "--jobs 必须是正整数"
is_positive_integer "${L3_JOBS}" || die "--l3-jobs 必须是正整数"
[[ "${LEVEL}" =~ ^[0-5]$ ]] || die "--level 必须是 0..5"
is_positive_integer "${L1_FILES}" || die "--l1-files 必须是正整数"
is_positive_integer "${L5_SECONDS}" || die "--l5-seconds 必须是正整数"
[[ "${BOOT_WAIT_SECONDS}" =~ ^[0-9]+$ ]] || die "--boot-wait-seconds 必须是非负整数"
is_positive_integer "${TIMEOUT_SECONDS}" || die "--timeout-seconds 必须是正整数"
[[ -f "${ROOTFS}" ]] || die "缺少 selfhost rootfs：${ROOTFS}；请先运行 prepare_rootfs.sh"
[[ -d "${TGOSKITS}/.git" ]] || die "不是可用的 tgoskits 仓库：${TGOSKITS}"

for command_name in qemu-system-riscv64 timeout rg cp e2fsck debugfs sudo losetup mount umount; do
    command -v "${command_name}" >/dev/null || die "缺少命令：${command_name}"
done

if ((SKIP_F7LY_BUILD == 0)); then
    info "构建 F7LY RISC-V shell 内核"
    make -C "${PROJECT_ROOT}" build ARCH=riscv INITCODE_MODE=shell
fi
KERNEL="${PROJECT_ROOT}/kernel-rv-shell"
[[ -x "${KERNEL}" ]] || die "缺少 F7LY shell 内核：${KERNEL}"

if ((LEVEL >= 3)); then
    if ((SKIP_STARRY_SEED_BUILD == 0)); then
        command -v cargo >/dev/null || die "生成 Starry linker.x 需要宿主 cargo"
        [[ -z "$(git -C "${TGOSKITS}" status --short)" ]] || die \
            "tgoskits 有未提交改动；rootfs 只包含 HEAD，不能用脏工作区生成 linker.x"
        info "宿主构建 Starry seed，仅用于生成与当前 HEAD 匹配的 linker.x"
        (
            cd "${TGOSKITS}"
            cargo xtask starry build --arch riscv64
        )
    fi
    if [[ -z "${LINKER_X}" ]]; then
        for candidate in \
            "${TGOSKITS}/target/riscv64gc-unknown-none-elf/release/linker.x" \
            "${TGOSKITS}/target/riscv64gc-unknown-none-elf/debug/linker.x"; do
            if [[ -f "${candidate}" ]]; then
                LINKER_X="${candidate}"
                break
            fi
        done
    fi
    [[ -f "${LINKER_X}" ]] || die \
        "找不到 Starry linker.x；请取消 --skip-starry-seed-build 或通过 --linker-x 指定"
fi

timestamp="$(date +%Y%m%d-%H%M%S)"
run_id="selfbuild-rv-smp${SMP}-j${JOBS}-l${LEVEL}-${timestamp}"
work_dir="${PROJECT_ROOT}/build/selfbuild/runs/${run_id}"
result_dir="${PROJECT_ROOT}/logs/run/${run_id}"
work_rootfs="${work_dir}/rootfs.img"
console_log="${result_dir}/console.log"
fsck_log="${result_dir}/e2fsck.log"
run_env="${result_dir}/run.env"
guest_commands="${result_dir}/guest_commands.txt"
mkdir -p "${work_dir}" "${result_dir}"

info "创建本次可写 rootfs 副本：${work_rootfs}"
cp --reflink=auto --sparse=always "${ROOTFS}" "${work_rootfs}.new"
mv "${work_rootfs}.new" "${work_rootfs}"

source_commit="$(git -C "${TGOSKITS}" rev-parse HEAD)"
image_commit="$(debugfs -R 'cat /opt/starryos/.source-commit' "${work_rootfs}" 2>/dev/null | tr -d '[:space:]')"
[[ "${image_commit}" == "${source_commit}" ]] || die \
    "rootfs 源码版本 ${image_commit:-missing} 与 tgoskits HEAD ${source_commit} 不一致；请重新运行 prepare_rootfs.sh"

run_e2fsck_repair() {
    local image="$1" rc
    set +e
    e2fsck -pf "${image}" >>"${fsck_log}" 2>&1
    rc=$?
    set -e
    ((rc <= 1)) || die "QEMU 前 e2fsck 失败，退出码=${rc}，详见 ${fsck_log}"
}
run_e2fsck_repair "${work_rootfs}"

mount_dir="$(mktemp -d /tmp/f7ly-selfbuild-run-mount-XXXXXX)"
loop_device=""
mounted=0
cleanup_mount() {
    set +e
    if ((mounted == 1)); then
        sudo umount "${mount_dir}" >/dev/null 2>&1
    fi
    if [[ -n "${loop_device}" ]]; then
        sudo losetup -d "${loop_device}" >/dev/null 2>&1
    fi
    rmdir "${mount_dir}" >/dev/null 2>&1
}
trap cleanup_mount EXIT

sudo -v
loop_device="$(sudo losetup --find --show "${work_rootfs}")"
sudo mount "${loop_device}" "${mount_dir}"
mounted=1
sudo install -m 0755 "${SCRIPT_DIR}/guest_self_compile.sh" \
    "${mount_dir}/usr/bin/f7ly-self-compile"
if ((LEVEL >= 3)); then
    sudo install -m 0644 "${LINKER_X}" "${mount_dir}/opt/starryos/linker.x"
fi
sudo sync
sudo umount "${mount_dir}"
mounted=0
sudo losetup -d "${loop_device}"
loop_device=""

cat >"${guest_commands}" <<EOF
echo __F7LY_SELFCOMPILE_LAUNCH__
SELFBUILD_LEVEL=${LEVEL} CARGO_BUILD_JOBS=${JOBS} SELFBUILD_L3_JOBS=${L3_JOBS} SELFBUILD_EXPECTED_CPUS=${SMP} SELFBUILD_L1_FILES=${L1_FILES} SELFBUILD_L5_SECONDS=${L5_SECONDS} /bin/bash /usr/bin/f7ly-self-compile
rc=\$?
echo __F7LY_SELFCOMPILE_GUEST_RC__=\${rc}
sync
exit \${rc}
EOF

cat >"${run_env}" <<EOF
run_id=${run_id}
arch=riscv64
smp=${SMP}
cargo_jobs=${JOBS}
l3_jobs=${L3_JOBS}
memory=${MEMORY}
level=${LEVEL}
l1_files=${L1_FILES}
l5_seconds=${L5_SECONDS}
boot_wait_seconds=${BOOT_WAIT_SECONDS}
timeout_seconds=${TIMEOUT_SECONDS}
source_rootfs=${ROOTFS}
work_rootfs=${work_rootfs}
kernel=${KERNEL}
tgoskits=${TGOSKITS}
tgoskits_commit=${source_commit}
linker_x=${LINKER_X}
EOF

info "启动 F7LY：RV64 ${SMP} CPU、${MEMORY}；串口日志：${console_log}"
set +e
{ sleep "${BOOT_WAIT_SECONDS}"; sed -n '1,$p' "${guest_commands}"; } |
    timeout "${TIMEOUT_SECONDS}s" qemu-system-riscv64 \
        -machine virt -kernel "${KERNEL}" -m "${MEMORY}" \
        -display none -chardev stdio,id=shell_stdio,signal=off \
        -serial chardev:shell_stdio -monitor none -smp "${SMP}" -bios default \
        -drive file="${work_rootfs}",if=none,format=raw,id=x0,file.locking=off \
        -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
        -device virtio-net-device,netdev=net -netdev user,id=net \
        -no-reboot -rtc base=utc -initrd "${PROJECT_ROOT}/images/initrd.img" \
        >"${console_log}" 2>&1
qemu_exit_code=${PIPESTATUS[1]}
set -e
printf 'qemu_exit_code=%s\n' "${qemu_exit_code}" >>"${run_env}"

set +e
e2fsck -fn "${work_rootfs}" >>"${fsck_log}" 2>&1
fsck_exit_code=$?
set -e
printf 'fsck_exit_code=%s\n' "${fsck_exit_code}" >>"${run_env}"

online_cpus="$(sed -n 's/.*SELFBUILD_CPUS_ONLINE=\([0-9][0-9]*\).*/\1/p' "${console_log}" | tail -n 1)"
memory_kb="$(sed -n 's/.*SELFBUILD_MEMORY_KB=\([0-9][0-9]*\).*/\1/p' "${console_log}" | tail -n 1)"
artifact_stat="$(debugfs -R 'stat /opt/f7ly-selfbuild-artifacts/starryos-riscv64' "${work_rootfs}" 2>/dev/null || true)"
artifact_size="$(printf '%s\n' "${artifact_stat}" | sed -n 's/.*Size: *\([0-9][0-9]*\).*/\1/p' | head -n 1)"
: "${artifact_size:=0}"
printf 'guest_online_cpus=%s\n' "${online_cpus:-missing}" >>"${run_env}"
printf 'guest_memory_kb=%s\n' "${memory_kb:-missing}" >>"${run_env}"
printf 'artifact_size=%s\n' "${artifact_size}" >>"${run_env}"

failure=0
if ((qemu_exit_code != 0)); then
    printf '[self-compile] QEMU 退出码异常：%d\n' "${qemu_exit_code}" >&2
    failure=1
fi
if ((fsck_exit_code != 0)); then
    printf '[self-compile] e2fsck 只读校验失败：%d\n' "${fsck_exit_code}" >&2
    failure=1
fi
if [[ -z "${memory_kb}" ]] || ((memory_kb < MIN_MEMORY_KB)); then
    printf '[self-compile] guest 可见内存不足或未解析：%s KiB\n' "${memory_kb:-missing}" >&2
    failure=1
fi
if [[ -z "${online_cpus}" ]] || ((online_cpus < SMP)); then
    printf '[self-compile] guest 在线 CPU 不足或未解析：%s\n' "${online_cpus:-missing}" >&2
    failure=1
fi
if ! rg -q 'SELF_COMPILE_SUCCESS' "${console_log}"; then
    printf '[self-compile] 串口日志中没有 SELF_COMPILE_SUCCESS\n' >&2
    failure=1
fi
if ((LEVEL >= 5)) && ! rg -q '^BUILDSTORM_LONG_STRESS_PASS ' "${console_log}"; then
    printf '[self-compile] 缺少 L5 长压力成功标记\n' >&2
    failure=1
fi
if ((LEVEL >= 4 && artifact_size <= 1000000)); then
    printf '[self-compile] StarryOS 产物无效：%s bytes\n' "${artifact_size}" >&2
    failure=1
fi
if rg -n 'SELF_COMPILE_FAILED|TLB shootdown timeout|alloc_page failed|unexpected scause|panic:' \
    "${console_log}" >"${result_dir}/fatal_markers.txt"; then
    printf '[self-compile] 检测到失败/内核异常标记，详见 %s\n' \
        "${result_dir}/fatal_markers.txt" >&2
    failure=1
else
    rm -f "${result_dir}/fatal_markers.txt"
fi

((failure == 0)) || die "自编译验收失败；日志：${console_log}"
info "自编译验收通过：artifact=${artifact_size} bytes"
info "结果目录：${result_dir}"
info "持久化工作镜像：${work_rootfs}"
