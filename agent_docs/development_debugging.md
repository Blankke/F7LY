# 开发调试指令

## 文档概况

本文档给 agent 快速执行构建、运行、日志保存、镜像挂载和单测调试。第一次把 F7LY 放到 VisionFive2 或 2K1000LA 实机时，按 [board_bringup_beginner.md](board_bringup_beginner.md) 操作；需要架构背景时读 `agent_docs/project_architecture.md`；需要评测状态协作时读 `agent_docs/scoreboard.md`。

## 开始任务前

先执行：

```bash
git status --short
git log -5 --pretty=fuller --stat
```

如果用户或其他 agent 已经有未提交改动，不要覆盖；如果同一文件需要继续修改，先读 diff 理解现状。

## 调试输出按钮
普通调试输出默认关闭，全局开关在 `printer.cc` 中：
```
bool disable_printf_flag = true;
```
2K1000 画像还会通过 `boardPrintf*` 输出少量启动阶段、资源绑定和硬件错误；QEMU 画像默认关闭这类上板日志，避免污染评测输出。不要在逐 IRQ、逐包或逐扇区路径加入默认日志。

## 构建命令

常用命令：

```bash
make all
make build PROFILE=riscv-qemu
make build PROFILE=riscv-visionfive2
make build PROFILE=loongarch-qemu
make build PROFILE=loongarch-2k1000
make visionfive2
make visionfive2-shell
make 2k1000
make 2k1000-shell
make run PROFILE=riscv-qemu
make run PROFILE=loongarch-qemu
make debug PROFILE=riscv-qemu
make debug PROFILE=loongarch-qemu
make clean
```

查看和检查画像：

```bash
make profiles
make print-config PROFILE=loongarch-2k1000 MODE=shell
```

Makefile 事实：

- 裸 `make` 与 `make all` 等价，固定完整构建 RISC-V QEMU evaluation 和 LoongArch QEMU evaluation 两套内核；构建过程不会覆盖或恢复任何磁盘镜像。
- 默认 `PROFILE ?= riscv-qemu`，每个画像同时确定架构、开发板、驱动集合和链接脚本；不再允许分别传入 `ARCH` 与 `BOARD`。
- 默认 `MODE ?= evaluation`；`run` 固定运行 evaluation，`shell` 固定运行
  shell。直接构建或调试时才按需传 `MODE=shell`。旧变量 `INITCODE_MODE`
  会直接报错。
- 可用画像为 `riscv-qemu`、`loongarch-qemu`、`riscv-visionfive2`、`loongarch-2k1000`。
- `make visionfive2` / `make visionfive2-shell` 与 `make 2k1000` /
  `make 2k1000-shell` 是两块实机的对称快捷构建入口；它们仍使用上述画像，
  不建立第二套构建规则。
- `run`、`shell`、`debug` 只接受 QEMU 画像；实机画像只使用 `make build PROFILE=...` 生成产物。
- `run` 默认使用初赛评测盘；显式传 `QEMU_DISK=final` 可运行决赛完整
  rootfs，`shell` 固定使用决赛盘。只有真正启动本地 QEMU 的入口才检查
  或下载镜像，`build/all` 不依赖磁盘。
- 输出目录以完整画像命名：`build/riscv-qemu/`、
  `build/riscv-visionfive2/`、`build/loongarch-qemu/`、
  `build/loongarch-2k1000/`；shell 模式在目录名后追加 `-shell`。
- 最终内核产物位于仓库根目录：`kernel-rv(.bin)`、`kernel-rv-visionfive2(.bin)`、`kernel-la(.bin)`、`kernel-la-2k1000(.bin)`；`build/` 保存中间目标和 initcode。
- RISC-V QEMU initcode：`user/app/initcode-rv.cc` ->
  `build/riscv-qemu/user/initcode.bin`。
- LoongArch QEMU initcode：`user/app/initcode-la.cc` ->
  `build/loongarch-qemu/user/initcode.bin`。
- EASTL 单独编成当前画像输出目录下的 `thirdparty/EASTL/libeastl.a`。
- 每个画像目录中的 `build-config.stamp` 记录工具链、编译/链接参数和构建规则摘要；`kernel-sources.list` 记录实际链接的内核源码。配置变化会重编对象，源码新增、删除或移动会更新链接结果，同配置的重复构建保持无操作。
- `make clean` 从当前画像清单自动推导 evaluation/shell 产物，只删除这些
  画像的构建目录和根目录内核产物；会保留 `build/` 下的 Docker 评测包、
  selfbuild 镜像与专项日志，也不会修改 `images/`。

工具链：

- RISC-V：`riscv64-linux-gnu-`
- LoongArch：`loongarch64-linux-gnu-`

## Python 命令

所有 Python 命令必须在 venv 中执行：

```bash
uv venv
source .venv/bin/activate
which python
python -c 'import sys; print(sys.executable)'
```

如果 `uv venv` 因 cache 不可写失败：

```bash
UV_CACHE_DIR=/tmp/uv-cache uv venv
source .venv/bin/activate
which python
```

不要为了缺少 Python 环境而改项目代码降级；直接报告环境缺失。

## QEMU 运行与日志保存

`run`、`shell`、`debug` 只接受名字以 `-qemu` 对应的 QEMU 画像。物理板画像会明确拒绝进入 QEMU，防止拿 2K1000 的链接地址和驱动组合误启动 `virt` 机器。

不要把 QEMU 长输出直接刷进聊天。所有运行输出写入 `logs/run/output_*.txt`，不要直接写到项目根目录；模板会先创建 `logs/run/`，这些输出文件已被 `.gitignore` 覆盖。

## VisionFive2 的 U-Boot 启动契约

`kernel-rv-visionfive2.bin` 的装载地址是 `0x40200000`，文件开头包含 U-Boot `booti` 可识别的标准 RISC-V Linux Image v0.2 头。启动时必须同时传入真实板级 DTB：

```bash
setenv kernel_addr_r 0x40200000
fatload mmc 1:1 ${kernel_addr_r} kernel-rv-visionfive2.bin
fatload mmc 1:1 ${fdt_addr_r} <实际的-VisionFive2-DTB-路径>
booti ${kernel_addr_r} - ${fdt_addr_r}
```

VisionFive 2 v1.3B 当前实机使用的等价 U-Boot 环境命令为：

```console
setenv f7boot 'fatload mmc 1:1 0x40200000 kernel-rv-visionfive2-shell.bin; fatload mmc 1:1 0x46000000 jh7110-starfive-visionfive-2-v1.3b.dtb; booti 0x40200000 - 0x46000000'
run f7boot
```

日常构建、复制、自动执行上述启动序列并保留交互串口可使用：

```bash
BOOT_DIR=/path/to/mounted-fat \
  ./scripts/board/visionfive2-dev.sh build
```

其中 `fdt_addr_r` 必须是 U-Boot 环境中一段不与内核镜像重叠的 RAM；DTB 文件名和 FAT 分区路径以实际启动介质为准。不要使用旧分支的 `go 0x40200000`：`go` 传入的是 `argc/argv`，而当前内核明确要求标准 RISC-V 启动 ABI `a0=hartid、a1=DTB 物理地址`。

当前 VF2 存储驱动读取整张 SD 卡，公共块层再识别裸 ext4、MBR 或 GPT；不应把 ext4 分区偏移写回 DWMMC 驱动。

完整 RISC-V 回归日志：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs/run
log="logs/run/output_r_${ts}_final-2026_QEMU_MEM-8G_QEMU_SMP-8_timeout-40m.txt"
{
  echo "run_at=${ts}"
  echo "arch=riscv"
  echo "cmd=timeout 40m make run PROFILE=riscv-qemu QEMU_DISK=final QEMU_MEM=8G QEMU_SMP=8"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 40m make run PROFILE=riscv-qemu QEMU_DISK=final QEMU_MEM=8G QEMU_SMP=8
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

完整 LoongArch 回归日志：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs/run
log="logs/run/output_l_${ts}_final-2026_QEMU_MEM-8G_QEMU_SMP-8_timeout-40m.txt"
{
  echo "run_at=${ts}"
  echo "arch=loongarch"
  echo "cmd=timeout 40m make run PROFILE=loongarch-qemu QEMU_DISK=final QEMU_MEM=8G QEMU_SMP=8"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 40m make run PROFILE=loongarch-qemu QEMU_DISK=final QEMU_MEM=8G QEMU_SMP=8
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

完整测试 timeout 统一 40 分钟；单条测例 timeout 最多 5 分钟。

## 单条测例调试

调试时先缩小到目标测例，不要一上来跑完整回归。

常用入口：

- basic 测例列表：`basic_testcases[]`
- LTP 列表：`ltp_testcases[]`
- subset 工具：`basic_subset_test()`、`ltp_subset_test()`
- 用户 init：`user/app/initcode-rv.cc`、`user/app/initcode-la.cc`

RISC-V 单测日志模板：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs/run
log="logs/run/output_r_${ts}_single-target_QEMU_MEM-8G_QEMU_SMP-8_timeout-5m.txt"
{
  echo "run_at=${ts}"
  echo "arch=riscv"
  echo "scope=single-target"
  echo "cmd=timeout 5m make run PROFILE=riscv-qemu QEMU_MEM=8G QEMU_SMP=8"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 5m make run PROFILE=riscv-qemu QEMU_MEM=8G QEMU_SMP=8
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

LoongArch 单测日志模板：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs/run
log="logs/run/output_l_${ts}_single-target_QEMU_MEM-8G_QEMU_SMP-8_timeout-5m.txt"
{
  echo "run_at=${ts}"
  echo "arch=loongarch"
  echo "scope=single-target"
  echo "cmd=timeout 5m make run PROFILE=loongarch-qemu QEMU_MEM=8G QEMU_SMP=8"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 5m make run PROFILE=loongarch-qemu QEMU_MEM=8G QEMU_SMP=8
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

汇报时只说日志路径、退出码、关键 PASS/FAIL/panic 现象，不要贴完整日志。

## QEMU 与 GDB

QEMU 运行参数由 Makefile 管理：

- `make run PROFILE=riscv-qemu`：初赛盘
  `images/oscomp-preliminary-riscv64.img`。
- `make run PROFILE=loongarch-qemu`：初赛盘
  `images/oscomp-preliminary-loongarch64.img`。
- `make run PROFILE=riscv-qemu QEMU_DISK=final`：RISC-V 决赛盘
  `images/oscomp-final-riscv64.img`。
- `make run PROFILE=loongarch-qemu QEMU_DISK=final`：LoongArch 决赛盘
  `images/oscomp-final-loongarch64.img`。
- `make shell PROFILE=riscv-qemu`：决赛盘
  `images/oscomp-final-riscv64.img`。
- `make shell PROFILE=loongarch-qemu`：决赛盘
  `images/oscomp-final-loongarch64.img`。
- 默认 QEMU 运行直接使用 ext4 主盘，不再加载本地 `initrd.img`。固件若在 DTB `/chosen` 中明确声明 ext4 initrd，内核仍保留回退能力。
- 默认内存：`QEMU_MEM ?= 8G`
- 调试内存：`QEMU_DEBUG_MEM ?= 8G`
- 默认 CPU：`QEMU_SMP ?= 8`
- 默认磁盘套件：`QEMU_DISK ?= preliminary`，定义在 `mk/qemu.mk`。若希望
  项目长期默认跑决赛盘，只修改这里为 `QEMU_DISK ?= final`；不要替换
  `images/` 中的初赛文件。
- `make run PROFILE=<qemu画像>` 默认传入 `QEMU_RUN_SNAPSHOT ?= -snapshot`，防止自动回归污染评测 sdcard 镜像；如需写回可显式传 `QEMU_RUN_SNAPSHOT=`。
- `make shell PROFILE=<qemu画像>` 默认传入空的 `QEMU_SHELL_SNAPSHOT`，会写回独立 shell rootfs 镜像；如需临时 shell 可显式传 `QEMU_SHELL_SNAPSHOT=-snapshot`。
- `make debug PROFILE=<qemu画像>` 默认传入 `QEMU_DEBUG_SNAPSHOT ?= -snapshot`，调试过程中不会写回磁盘镜像；确实需要观察持久化写入时再显式传 `QEMU_DEBUG_SNAPSHOT=`。

镜像准备规则：

1. `images/` 中工作镜像存在时直接使用。
2. 工作镜像缺失时，从 `images/bak/` 的同名基线复制。
3. 两者都不存在时，仅当该镜像配置了可信 URL 才下载官方 `.xz`。当前初赛
   镜像允许下载，决赛 URL 刻意留空并明确报错，避免使用错误赛季的 rootfs。
4. 下载中断时保留 `.xz.part`，下次自动断点续传；下载成功后删除压缩包，
   避免同时长期保存压缩与解压两份备份。
5. 显式传入自定义 `QEMU_STORAGE_IMAGE` 时只校验该文件，不会用官方镜像
   覆盖它。

只准备镜像、不启动 QEMU：

```bash
make prepare-image PROFILE=riscv-qemu QEMU_DISK=preliminary
make prepare-image PROFILE=loongarch-qemu QEMU_DISK=final
```

GDB：

```bash
gdb-multiarch -x debug/gdb/riscv.gdb
loongarch64-linux-gnu-gdb -x debug/gdb/loongarch.gdb
```

## 镜像挂载与恢复

挂载脚本：

- `scripts/mount/mount-rv.sh`：挂载 RISC-V 初赛盘到 `/mnt/sdcard-rv`，并链接 RISC-V musl loader。
- `scripts/mount/mount-la.sh`：挂载 LoongArch 初赛盘到 `/mnt/sdcard-la`。
- `scripts/mount/mount-rootfs-rv.sh`：挂载 RISC-V 决赛完整 rootfs。
- `scripts/mount/mount-rootfs-la.sh`：挂载 LoongArch 决赛完整 rootfs。
- `scripts/mount/mount-all.sh`：批量挂载多个镜像。

脚本会自动创建 `/mnt/...` 挂载点。

恢复镜像：

- `scripts/images/restore-sdcards.sh` 会从 `images/bak/` 覆盖恢复两份初赛盘
  和两份决赛 rootfs。
- 这是破坏性操作，运行前必须确认。

## 回归输出标记

常见输出：

- 分组开始：`#### OS COMP TEST GROUP START <group> ####`
- 分组结束：`#### OS COMP TEST GROUP END <group> ####`
- 单测开始：`[RUN ] <path>`
- 单测通过：`[PASS] <path> (exit=0)`
- 单测失败：`[FAIL] <path> (...)`
- LTP 开始：`RUN LTP CASE <name>`
- LTP 失败/返回：`FAIL LTP CASE <name>: <ret>`

解析日志或更新 scoreboard 时优先使用这些标记。

## 验证策略

- 修改 C/C++ 内核或用户态代码后，至少运行对应完整画像的 `make build PROFILE=...`。
- 修改启动、内存、IRQ、console、clock、block、net 等公共平台边界时，至少静态构建 `riscv-qemu`、`riscv-visionfive2`、`loongarch-qemu`、`loongarch-2k1000` 四个画像。
- 只要求静态修改时不得自行运行 QEMU 或上板；四画像链接通过、`git diff --check` 通过，并确认源集合没有混入其他板驱动即可。
- 修改 Python 工具后，在 venv 中运行语法检查，例如：

```bash
source .venv/bin/activate
which python
python -c 'import py_compile; py_compile.compile("scoreboard/generate_scoreboard.py", cfile="/tmp/f7ly_generate_scoreboard.pyc", doraise=True); print("py_compile ok")'
```

- 修改 Markdown 文档后，至少检查关键链接路径存在：

```bash
find agent_docs scoreboard -maxdepth 3 -type f | sort
```

## 常见调试定位

- `init exiting`：检查回归主进程是否被测例误杀；确认 `run_test()` 是否仍对子进程 `setpgid(0,0)`。
- basic 大面积失败：优先检查 fork/clone、execve、open/read/write、wait status。
- LTP 某族失败：先读 `ref/ltp/` 对应源码，再看 syscall 语义和错误码。
- pthread/futex 问题：同时看 clone flags、futex wait/wake、robust list、signal、LoongArch LL/SC/TLB。
- mmap/munmap 问题：同时看 VMA、trap 缺页、copy_in/copy_out 懒分配、退出释放。
- 文件系统并发/路径问题：先区分虚拟文件、设备文件、普通 ext4、pipe/socket，再找对应 file 派生类。
