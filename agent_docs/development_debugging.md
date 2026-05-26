# 开发调试指令

## 文档概况

本文档给 agent 快速执行构建、运行、日志保存、镜像挂载和单测调试。需要架构背景时读 `agent_docs/project_architecture.md`；需要评测状态协作时读 `agent_docs/scoreboard.md`。

## 开始任务前

先执行：

```bash
git status --short
git log -5 --pretty=fuller --stat
```

如果用户或其他 agent 已经有未提交改动，不要覆盖；如果同一文件需要继续修改，先读 diff 理解现状。

## 构建命令

常用命令：

```bash
make all
make build ARCH=riscv
make build ARCH=loongarch
make run r
make run l
make debug ARCH=riscv
make debug ARCH=loongarch
make clean
```

架构别名：

```bash
make r
make l
make run r
make run l
```

Makefile 事实：

- 默认 `ARCH ?= riscv`。
- `make r` 或目标名包含 `riscv` 时切到 RISC-V。
- `make l` 或目标名包含 `loongarch` 时切到 LoongArch。
- 输出目录：`build/riscv/`、`build/loongarch/`。
- RISC-V 内核 ELF：`build/riscv/kernel-qemu`。
- LoongArch 内核 ELF：`build/loongarch/kernel-la`。
- RISC-V initcode：`user/app/initcode-rv.cc` -> `user/initcode-rv`。
- LoongArch initcode：`user/app/initcode-la.cc` -> `user/initcode-la`。
- EASTL 单独编成 `build/<arch>/thirdparty/EASTL/libeastl.a`。
- `make clean` 会删除 `build/`、`.o/.d`、EASTL 产物和 `user/initcode-*`，不是轻量清理。

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

不要把 QEMU 长输出直接刷进聊天。所有运行输出写入 `logs/output_*.txt`，不要直接写到项目根目录；模板会先创建 `logs/`，这些输出文件已被 `.gitignore` 覆盖。

完整 RISC-V 回归日志：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs
log="logs/output_r_${ts}_make-run-r_QEMU_MEM-1G_timeout-40m.txt"
{
  echo "run_at=${ts}"
  echo "arch=riscv"
  echo "cmd=timeout 40m make run r QEMU_MEM=1G"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 40m make run r QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

完整 LoongArch 回归日志：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs
log="logs/output_l_${ts}_make-run-l_QEMU_MEM-1G_timeout-40m.txt"
{
  echo "run_at=${ts}"
  echo "arch=loongarch"
  echo "cmd=timeout 40m make run l QEMU_MEM=1G"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 40m make run l QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

完整测试 timeout 统一 40 分钟；单条测例 timeout 最多 5 分钟。

## 单条测例调试

调试时先缩小到目标测例，不要一上来跑完整回归。

常用入口：

- 回归总入口：`user/user_lib/user_test.cc` 的 `regression_suite_4d1444()`
- basic 测例列表：`basic_testcases[]`
- LTP 列表：`ltp_testcases[]`
- subset 工具：`basic_subset_test()`、`ltp_subset_test()`
- 用户 init：`user/app/initcode-rv.cc`、`user/app/initcode-la.cc`

RISC-V 单测日志模板：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs
log="logs/output_r_${ts}_single-target_QEMU_MEM-1G_timeout-5m.txt"
{
  echo "run_at=${ts}"
  echo "arch=riscv"
  echo "scope=single-target"
  echo "cmd=timeout 5m make run r QEMU_MEM=1G"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 5m make run r QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

LoongArch 单测日志模板：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs
log="logs/output_l_${ts}_single-target_QEMU_MEM-1G_timeout-5m.txt"
{
  echo "run_at=${ts}"
  echo "arch=loongarch"
  echo "scope=single-target"
  echo "cmd=timeout 5m make run l QEMU_MEM=1G"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 5m make run l QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

汇报时只说日志路径、退出码、关键 PASS/FAIL/panic 现象，不要贴完整日志。

## QEMU 与 GDB

QEMU 运行参数由 Makefile 管理：

- RISC-V：`qemu-system-riscv64 -machine virt -kernel build/riscv/kernel-qemu -drive file=images/sdcard-rv.img ... -initrd images/initrd.img`
- LoongArch：`qemu-system-loongarch64 -machine virt -cpu la464-loongarch-cpu -kernel build/loongarch/kernel-la -drive file=images/sdcard-la.img ... -initrd images/initrd.img`
- 默认内存：`QEMU_MEM ?= 1G`
- 调试内存：`QEMU_DEBUG_MEM ?= 1G`

GDB：

```bash
gdb-multiarch -x debug/gdb/riscv.gdb
loongarch64-linux-gnu-gdb -x debug/gdb/loongarch.gdb
```

## 镜像挂载与恢复

挂载脚本：

- `scripts/mount/mount-rv.sh`：挂载 `images/sdcard-rv.img` 到 `/mnt/sdcard-rv`，并链接 RISC-V musl loader。
- `scripts/mount/mount-la.sh`：挂载 `images/sdcard-la.img` 到 `/mnt/sdcard-la`。
- `scripts/mount/mount-all.sh`：批量挂载多个镜像。

脚本会自动创建 `/mnt/...` 挂载点。

恢复镜像：

- `scripts/images/restore-sdcards.sh` 会从 `images/sdcard-bak/` 覆盖恢复当前 `images/sdcard-la.img` 和 `images/sdcard-rv.img`。
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

- 修改 C/C++ 内核或用户态代码后，至少运行对应架构 `make build ARCH=...`。
- 修改通用代码且影响双架构时，运行 `make build ARCH=riscv` 和 `make build ARCH=loongarch`。
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
