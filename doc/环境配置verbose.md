# 环境配置verbose

## 配置qemu
你现在的版本是：
QEMU emulator version 8.2.50 (v6.0.0-23512-gffd454c67e)
这个版本号看似“高”，但它其实来自某个开发快照（v6.0.0 分支上 23,512 个提交之后），并不是正式的 8.2 或 9.x 稳定版，可能缺功能（比如你遇到的 user 网络缺失）。
✅ 目标：安装正式版 QEMU 9.2.1（官方最新稳定版）
官方源码地址：
https://gitlab.com/qemu-project/qemu
✅ 步骤：从源码构建 QEMU 9.2.1（含 user 网络）
我会给你最干净、稳妥的方式：
✅ 1. 安装依赖（Ubuntu/Debian）
sudo apt update
sudo apt install -y git ninja-build libglib2.0-dev libpixman-1-dev \
  libslirp-dev libfdt-dev zlib1g-dev libffi-dev libcap-ng-dev \
  libattr1-dev libgnutls28-dev libusb-1.0-0-dev \
  libzstd-dev libcurl4-openssl-dev python3
❗特别注意 libslirp-dev 一定要装上，解决 user 网络支持问题的关键！
✅ 2. 下载 QEMU 9.2.1 源码
cd ~
wget https://download.qemu.org/qemu-9.2.1.tar.xz
tar -xf qemu-9.2.1.tar.xz
cd qemu-9.2.1
✅ 3. 配置构建（仅 loongarch64）
mkdir build && cd build
../configure --target-list=loongarch64-softmmu --enable-slirp
⚠ 若你也想支持 x86/x86_64 可加上：
--target-list=loongarch64-softmmu,x86_64-softmmu
✅ 4. 编译并安装
make -j$(nproc)
sudo make install
✅ 5. 验证版本和网络支持
qemu-system-loongarch64 --version
### 应显示 QEMU emulator version 9.2.1

qemu-system-loongarch64 -netdev help
### 应包含 user
✅ 后续建议（可选）
你可以用下面这个命令，快速检查当前使用的是哪个 qemu：
which qemu-system-loongarch64
如果不是 /usr/local/bin/qemu-system-loongarch64（源码安装默认路径），你可能还得：
sudo ln -sf /usr/local/bin/qemu-system-loongarch64 /usr/bin/qemu-system-loongarch64
✅ 总结
步骤说明
安装依赖 特别是 libslirp-dev 
下载源码 官方 QEMU 9.2.1 
编译安装 使用 --enable-slirp 启用 user 网络 
验证 看 --version 和 -netdev help 输出是否符合预期 
是否需要我打包一个脚本 install_qemu_9.2.1.sh？你只要运行一次就能完成所有步骤。
你现在的版本是：
QEMU emulator version 8.2.50 (v6.0.0-23512-gffd454c67e)
这个版本号看似“高”，但它其实来自某个开发快照（v6.0.0 分支上 23,512 个提交之后），并不是正式的 8.2 或 9.x 稳定版，可能缺功能（比如你遇到的 user 网络缺失）。
✅ 目标：安装正式版 QEMU 9.2.1（官方最新稳定版）
官方源码地址：
https://gitlab.com/qemu-project/qemu
✅ 步骤：从源码构建 QEMU 9.2.1（含 user 网络）
我会给你最干净、稳妥的方式：
✅ 1. 安装依赖（Ubuntu/Debian）
sudo apt update
sudo apt install -y git ninja-build libglib2.0-dev libpixman-1-dev \
  libslirp-dev libfdt-dev zlib1g-dev libffi-dev libcap-ng-dev \
  libattr1-dev libgnutls28-dev libusb-1.0-0-dev \
  libzstd-dev libcurl4-openssl-dev python3
❗特别注意 libslirp-dev 一定要装上，解决 u