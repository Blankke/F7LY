#!/usr/bin/env bash
#
# 准备供 F7LY 启动的 RISC-V StarryOS 自托管 rootfs。
#
# 使用示例：
#   # 已有 tgoskits 生成的 selfhost 镜像时，只制作 F7LY 工作副本并刷新源码：
#   scripts/selfbuild/prepare_rootfs.sh
#
#   # 首次准备：下载固定版本基础镜像，再调用 tgoskits 的官方制备脚本：
#   scripts/selfbuild/prepare_rootfs.sh --download-base --build-prepared
#
# 说明：
#   - tgoskits 官方制备步骤会安装 Rust nightly 和预取离线 crates，需要网络且耗时较长；
#   - 本脚本只修改 build/selfbuild 下的输出副本，不写回已经制备好的 selfhost 镜像；
#   - 注入源码使用 git archive HEAD，不包含 .git、target、tmp 和未提交改动。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

TGOSKITS="${HOME}/tgoskits"
BASE_IMAGE=""
PREPARED_IMAGE=""
OUTPUT_IMAGE="${PROJECT_ROOT}/build/selfbuild/rootfs-riscv64-debian-selfhost-f7ly.img"
DOWNLOAD_BASE=0
BUILD_PREPARED=0
FORCE=0

readonly BASE_URL="https://github.com/rcore-os/tgosimages/releases/download/v0.0.8/rootfs-riscv64-debian.img.tar.xz"
readonly BASE_SHA256="21510bff9b2a0f843f7dfe4998ba7bce049deddd0a3ca0c3dfa0e7d296753281"

info() { printf '[prepare-selfbuild-rootfs] %s\n' "$*"; }
warn() { printf '[prepare-selfbuild-rootfs] 警告：%s\n' "$*" >&2; }
die() { printf '[prepare-selfbuild-rootfs] 错误：%s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法：scripts/selfbuild/prepare_rootfs.sh [选项]

选项：
  --tgoskits PATH       tgoskits 仓库路径，默认 ~/tgoskits
  --base-image PATH     RISC-V Debian 基础镜像路径
  --prepared-image PATH tgoskits 已制备 selfhost 镜像路径
  --output PATH         F7LY 工作镜像输出路径
  --download-base       基础镜像不存在时下载并校验 tgosimages v0.0.8
  --build-prepared      selfhost 镜像不存在时调用 tgoskits 官方制备脚本
  --force               覆盖已经存在的 F7LY 输出副本
  -h, --help            显示帮助
EOF
}

while (($# > 0)); do
    case "$1" in
        --tgoskits)
            (($# >= 2)) || die "--tgoskits 缺少参数"
            TGOSKITS="$2"
            shift 2
            ;;
        --base-image)
            (($# >= 2)) || die "--base-image 缺少参数"
            BASE_IMAGE="$2"
            shift 2
            ;;
        --prepared-image)
            (($# >= 2)) || die "--prepared-image 缺少参数"
            PREPARED_IMAGE="$2"
            shift 2
            ;;
        --output)
            (($# >= 2)) || die "--output 缺少参数"
            OUTPUT_IMAGE="$2"
            shift 2
            ;;
        --download-base)
            DOWNLOAD_BASE=1
            shift
            ;;
        --build-prepared)
            BUILD_PREPARED=1
            shift
            ;;
        --force)
            FORCE=1
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

[[ -d "${TGOSKITS}/.git" ]] || die "不是可用的 tgoskits Git 仓库：${TGOSKITS}"
[[ -f "${TGOSKITS}/scripts/prepare-selfhost-rootfs.sh" ]] || \
    die "缺少 tgoskits selfhost 制备脚本"
[[ -f "${TGOSKITS}/scripts/filter-workspace.sh" ]] || \
    die "缺少 tgoskits workspace 过滤脚本"

: "${BASE_IMAGE:=${TGOSKITS}/tmp/axbuild/rootfs/rootfs-riscv64-debian.img}"
: "${PREPARED_IMAGE:=${TGOSKITS}/tmp/axbuild/rootfs/rootfs-riscv64-debian-selfhost-v2.img}"

for command_name in git tar sha256sum cp e2fsck debugfs find sudo losetup mount umount; do
    command -v "${command_name}" >/dev/null || die "缺少命令：${command_name}"
done

download_base_image() {
    command -v curl >/dev/null || die "下载基础镜像需要 curl"
    local download_dir archive extract_dir found_image
    download_dir="${PROJECT_ROOT}/build/selfbuild/downloads"
    archive="${download_dir}/rootfs-riscv64-debian.img.tar.xz"
    extract_dir="$(mktemp -d /tmp/f7ly-rv-rootfs-extract-XXXXXX)"
    mkdir -p "${download_dir}" "$(dirname "${BASE_IMAGE}")"

    if [[ ! -f "${archive}" ]]; then
        info "下载 RISC-V Debian 基础镜像：${BASE_URL}"
        curl -L --fail --retry 3 --output "${archive}.part" "${BASE_URL}"
        mv "${archive}.part" "${archive}"
    fi
    printf '%s  %s\n' "${BASE_SHA256}" "${archive}" | sha256sum -c -

    tar -xJf "${archive}" -C "${extract_dir}"
    found_image="$(find "${extract_dir}" -type f -name 'rootfs-riscv64-debian.img' -print -quit)"
    if [[ -z "${found_image}" ]]; then
        found_image="$(find "${extract_dir}" -type f -name '*.img' -print -quit)"
    fi
    [[ -n "${found_image}" ]] || die "下载包中没有找到 .img 文件"
    cp --reflink=auto --sparse=always "${found_image}" "${BASE_IMAGE}"
    rm -rf "${extract_dir}"
    info "基础镜像已准备：${BASE_IMAGE}"
}

if [[ ! -f "${PREPARED_IMAGE}" ]]; then
    if [[ ! -f "${BASE_IMAGE}" && ${DOWNLOAD_BASE} -eq 1 ]]; then
        download_base_image
    fi
    [[ -f "${BASE_IMAGE}" ]] || die \
        "缺少基础镜像 ${BASE_IMAGE}；可增加 --download-base"
    ((BUILD_PREPARED == 1)) || die \
        "缺少 selfhost 镜像 ${PREPARED_IMAGE}；可增加 --build-prepared"

    default_base="${TGOSKITS}/tmp/axbuild/rootfs/rootfs-riscv64-debian.img"
    default_prepared="${TGOSKITS}/tmp/axbuild/rootfs/rootfs-riscv64-debian-selfhost-v2.img"
    [[ "${BASE_IMAGE}" == "${default_base}" ]] || die \
        "tgoskits 官方脚本使用固定基础镜像路径；--build-prepared 时请使用 ${default_base}"
    [[ "${PREPARED_IMAGE}" == "${default_prepared}" ]] || die \
        "tgoskits 官方脚本使用固定输出路径；--build-prepared 时请使用 ${default_prepared}"

    info "调用 tgoskits 官方制备流程；该步骤会安装工具链并预取 crates"
    sudo "${TGOSKITS}/scripts/prepare-selfhost-rootfs.sh" --arch riscv64
fi

[[ -f "${PREPARED_IMAGE}" ]] || die "selfhost 制备结束后仍找不到：${PREPARED_IMAGE}"
if [[ -e "${OUTPUT_IMAGE}" && ${FORCE} -ne 1 ]]; then
    die "输出已存在：${OUTPUT_IMAGE}；如需重做请使用 --force"
fi

mkdir -p "$(dirname "${OUTPUT_IMAGE}")"
info "创建稀疏工作副本：${OUTPUT_IMAGE}"
cp --reflink=auto --sparse=always "${PREPARED_IMAGE}" "${OUTPUT_IMAGE}.new"
mv "${OUTPUT_IMAGE}.new" "${OUTPUT_IMAGE}"

run_e2fsck_repair() {
    local image="$1" rc
    set +e
    e2fsck -pf "${image}"
    rc=$?
    set -e
    ((rc <= 1)) || die "e2fsck 无法修复工作镜像，退出码=${rc}"
}

run_e2fsck_repair "${OUTPUT_IMAGE}"

source_stage="$(mktemp -d /tmp/f7ly-starryos-source-XXXXXX)"
mount_dir="$(mktemp -d /tmp/f7ly-selfbuild-mount-XXXXXX)"
loop_device=""
mounted=0
cleanup() {
    set +e
    if ((mounted == 1)); then
        sudo umount "${mount_dir}" >/dev/null 2>&1
    fi
    if [[ -n "${loop_device}" ]]; then
        sudo losetup -d "${loop_device}" >/dev/null 2>&1
    fi
    rm -rf "${source_stage}"
    rmdir "${mount_dir}" >/dev/null 2>&1
}
trap cleanup EXIT

source_commit="$(git -C "${TGOSKITS}" rev-parse HEAD)"
if [[ -n "$(git -C "${TGOSKITS}" status --short)" ]]; then
    warn "tgoskits 工作区有未提交改动；本次只打包 HEAD=${source_commit}"
fi
info "打包 tgoskits HEAD=${source_commit}"
git -C "${TGOSKITS}" archive HEAD | tar -x -C "${source_stage}"
printf '%s\n' "${source_commit}" >"${source_stage}/.source-commit"
[[ -f "${source_stage}/Cargo.toml" ]] || die "git archive 中缺少 Cargo.toml"
[[ -f "${source_stage}/os/StarryOS/starryos/Cargo.toml" ]] || \
    die "git archive 中缺少 StarryOS package"
chmod -R a+rX "${source_stage}"

sudo -v
loop_device="$(sudo losetup --find --show "${OUTPUT_IMAGE}")"
sudo mount "${loop_device}" "${mount_dir}"
mounted=1

# 这里只清理刚创建的 F7LY 工作副本中的旧源码；原始 selfhost 镜像保持不变。
sudo rm -rf "${mount_dir}/opt/starryos"
sudo mkdir -p "${mount_dir}/opt/starryos" "${mount_dir}/usr/bin" \
    "${mount_dir}/opt/f7ly-selfbuild-artifacts"
sudo cp -a "${source_stage}/." "${mount_dir}/opt/starryos/"
sudo install -m 0755 "${TGOSKITS}/scripts/filter-workspace.sh" \
    "${mount_dir}/usr/bin/filter-workspace.sh"
sudo install -m 0755 "${SCRIPT_DIR}/guest_self_compile.sh" \
    "${mount_dir}/usr/bin/f7ly-self-compile"
sudo sync
sudo umount "${mount_dir}"
mounted=0
sudo losetup -d "${loop_device}"
loop_device=""

run_e2fsck_repair "${OUTPUT_IMAGE}"

verify_path() {
    local path="$1" output
    output="$(debugfs -R "stat ${path}" "${OUTPUT_IMAGE}" 2>&1 || true)"
    [[ "${output}" == *"Inode:"* ]] || die "rootfs 验证失败，缺少 ${path}"
}

verify_path /bin/bash
verify_path /root/.cargo/bin/cargo
verify_path /root/.cargo/bin/rustc
verify_path /root/.cargo/registry/src
verify_path /opt/starryos/Cargo.toml
verify_path /opt/starryos/.source-commit
verify_path /usr/bin/filter-workspace.sh
verify_path /usr/bin/f7ly-self-compile

info "rootfs 准备完成：${OUTPUT_IMAGE}"
info "下一步：scripts/selfbuild/self_compile.sh --rootfs ${OUTPUT_IMAGE}"
