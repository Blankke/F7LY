#!/bin/sh
set -eu

# 为本地 QEMU 准备磁盘镜像。
#
# 固定镜像优先使用 images/ 工作副本，再从 images/bak/ 的只读基线恢复。
# 只有配置了 IMAGE_URL 的镜像才允许继续走网络下载；当前决赛镜像来源尚未
# 确认，因此 URL 留空并要求人工放置，避免自动拉取错误赛季的磁盘。

usage() {
    echo "用法: $0 {riscv-preliminary|loongarch-preliminary|riscv-final|loongarch-final} [镜像路径]" >&2
    exit 2
}

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    usage
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
IMAGE_DIR="$REPO_ROOT/images"
BACKUP_DIR="$IMAGE_DIR/bak"
IMAGE_KIND=$1

case "$IMAGE_KIND" in
    riscv-preliminary)
        IMAGE_NAME="oscomp-preliminary-riscv64.img"
        IMAGE_URL="https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-rv.img.xz"
        ;;
    loongarch-preliminary)
        IMAGE_NAME="oscomp-preliminary-loongarch64.img"
        IMAGE_URL="https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-la.img.xz"
        ;;
    riscv-final)
        IMAGE_NAME="oscomp-final-riscv64.img"
        IMAGE_URL=""
        ;;
    loongarch-final)
        IMAGE_NAME="oscomp-final-loongarch64.img"
        IMAGE_URL=""
        ;;
    *)
        echo "错误：未知镜像类型: $IMAGE_KIND" >&2
        usage
        ;;
esac

DEFAULT_IMAGE="$IMAGE_DIR/$IMAGE_NAME"
REQUESTED_IMAGE=${2:-$DEFAULT_IMAGE}
case "$REQUESTED_IMAGE" in
    /*) ;;
    *) REQUESTED_IMAGE="$REPO_ROOT/$REQUESTED_IMAGE" ;;
esac

# realpath -m 不要求目标已经存在，便于可靠地区分默认镜像和命令行覆盖路径。
if ! command -v realpath >/dev/null 2>&1; then
    echo "错误：缺少 realpath，无法安全规范化镜像路径" >&2
    exit 1
fi
REQUESTED_IMAGE=$(realpath -m -- "$REQUESTED_IMAGE")
DEFAULT_IMAGE=$(realpath -m -- "$DEFAULT_IMAGE")

# 显式覆盖的镜像属于调用者，不允许用官方镜像偷偷填充或覆盖它。
if [ "$REQUESTED_IMAGE" != "$DEFAULT_IMAGE" ]; then
    if [ ! -s "$REQUESTED_IMAGE" ]; then
        echo "错误：自定义 QEMU 镜像不存在或为空: $REQUESTED_IMAGE" >&2
        echo "自动下载只负责默认镜像；请先准备该自定义文件。" >&2
        exit 1
    fi
    echo "[image] 使用自定义镜像: $REQUESTED_IMAGE"
    exit 0
fi

if [ -s "$DEFAULT_IMAGE" ]; then
    echo "[image] 使用工作镜像: $DEFAULT_IMAGE"
    exit 0
fi

mkdir -p "$BACKUP_DIR"
BACKUP_IMAGE="$BACKUP_DIR/$IMAGE_NAME"

# 同一镜像只允许一个准备进程，避免并行 run/debug 同时下载或覆盖文件。
LOCK_DIR="$BACKUP_DIR/.prepare-$IMAGE_NAME.lock"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    echo "错误：另一个进程正在准备 $IMAGE_NAME" >&2
    echo "若确认没有相关进程，请删除残留锁目录: $LOCK_DIR" >&2
    exit 1
fi

WORK_IMAGE="$DEFAULT_IMAGE.tmp.$$"
BACKUP_TMP="$BACKUP_IMAGE.tmp.$$"
DOWNLOAD_PART="$BACKUP_IMAGE.xz.part"
cleanup() {
    rm -f -- "$WORK_IMAGE" "$BACKUP_TMP"
    rmdir "$LOCK_DIR" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

if [ ! -s "$BACKUP_IMAGE" ]; then
    if [ -z "$IMAGE_URL" ]; then
        echo "错误：$IMAGE_KIND 尚未配置可信下载地址。" >&2
        echo "请手动把正确镜像放到以下任一位置：" >&2
        echo "  工作副本: $DEFAULT_IMAGE" >&2
        echo "  bak 基线: $BACKUP_IMAGE" >&2
        exit 1
    fi

    for tool in curl xz; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "错误：缺少下载镜像所需工具: $tool" >&2
            exit 1
        fi
    done

    # .part 保留未完成下载，下一次可用 HTTP Range 继续；只有 xz 完整校验通过
    # 后才会生成权威 bak 镜像。
    if [ -s "$DOWNLOAD_PART" ] && xz --test "$DOWNLOAD_PART" 2>/dev/null; then
        echo "[image] 复用已完整下载的压缩包: $DOWNLOAD_PART"
    else
        echo "[image] bak 中没有 $IMAGE_NAME，开始下载官方压缩包"
        echo "[image] $IMAGE_URL"
        # 网络失败时保留 .part，不自动删除已经下载的数据。
        curl --fail --location --retry 3 --retry-delay 2 --progress-bar \
            --continue-at - --output "$DOWNLOAD_PART" "$IMAGE_URL"
    fi
    xz --test "$DOWNLOAD_PART"
    xz --decompress --stdout "$DOWNLOAD_PART" > "$BACKUP_TMP"
    if [ ! -s "$BACKUP_TMP" ]; then
        echo "错误：解压后的镜像为空: $BACKUP_TMP" >&2
        exit 1
    fi
    mv -- "$BACKUP_TMP" "$BACKUP_IMAGE"
    rm -f -- "$DOWNLOAD_PART"
    echo "[image] 已保存 bak 基线: $BACKUP_IMAGE"
else
    echo "[image] 从 bak 恢复: $BACKUP_IMAGE"
fi

# 先复制到同目录临时文件，再原子替换，QEMU 永远不会看到半份镜像。
# reflink 可用时几乎不占额外空间；不支持时退回普通稀疏复制。
cp --reflink=auto --sparse=always -- "$BACKUP_IMAGE" "$WORK_IMAGE"
mv -- "$WORK_IMAGE" "$DEFAULT_IMAGE"
echo "[image] 工作镜像已准备: $DEFAULT_IMAGE"
