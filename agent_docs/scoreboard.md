# Scoreboard 协作说明

## 文档概况

本文档说明 agent 如何查看、维护和更新 F7LY 的 Markdown scoreboard。scoreboard 同时给人类和 agent 使用：人类看当前开发进度，agent 在调试后更新测例状态并重新汇总。架构背景读 `agent_docs/project_architecture.md`；运行日志方法读 `agent_docs/development_debugging.md`。

## 当前 scoreboard 入口

- 总览：[scoreboard/README.md](../scoreboard/README.md)
- 生成器：[scoreboard/generate_scoreboard.py](../scoreboard/generate_scoreboard.py)

总览文件包含四张表：

- `riscv musl`
- `riscv glibc`
- `loongarch musl`
- `loongarch glibc`

每张表按小分统计：

```text
小分 | 总测例 | Pass测例 | 记录文件
```

小分文件继续列出具体测例：

```text
测例 | 状态 | 默认回归 | 命令 | 来源 | 备注
```

## 目录结构

```text
scoreboard/
├── README.md
├── generate_scoreboard.py
├── riscv/
│   ├── musl/
│   └── glibc/
└── loongarch/
    ├── musl/
    └── glibc/
```

每个 `<arch>/<libc>/` 下都有：

- `README.md`：该组合的小分汇总。
- `basic.md`、`ltp.md`、`libctest-static.md` 等小分文件。

小分文件中的测例名链接到挂载磁盘里的实际测例文件，例如：

```text
/mnt/sdcard-rv/musl/basic/brk
/mnt/sdcard-la/glibc/ltp/testcases/bin/accept01
```

## 生成与刷新

生成器会扫描默认挂载点：

- RISC-V：`/mnt/sdcard-rv`
- LoongArch：`/mnt/sdcard-la`

刷新 scoreboard：

```bash
source .venv/bin/activate
which python
python scoreboard/generate_scoreboard.py
```

如果没有 venv：

```bash
UV_CACHE_DIR=/tmp/uv-cache uv venv
source .venv/bin/activate
which python
python scoreboard/generate_scoreboard.py
```

生成器会：

- 扫描四组合磁盘测例。
- 生成或刷新小分 Markdown 文件。
- 保留已有小分文件中 `状态` 列的非空值。
- 重新计算顶层和组合 README 中的 `Pass测例`。
- 清理旧版 `scoreboard/out/` HTML/JSON 目录。

## 状态列约定

`状态` 列建议只使用这些值：

- 空：未确认或未运行。
- `PASS`：该测例在对应架构/libc 组合下已确认通过。
- `FAIL`：已运行但失败。
- `SKIP`：明确跳过。
- `BLOCKED`：被其他问题阻塞，不能独立判定。

顶层 `Pass测例` 只统计状态严格等于 `PASS` 的行。大小写由生成器按大写判断，但建议统一写 `PASS`。

## Agent 更新流程

当 agent 修复或验证测例后：

1. 找到对应小分文件，例如 `scoreboard/riscv/musl/ltp.md`。
2. 找到具体测例行。
3. 将 `状态` 改为 `PASS`、`FAIL`、`SKIP` 或 `BLOCKED`。
4. 在 `备注` 写短信息：日期、日志文件名、关键原因或依赖。
5. 重新运行生成器刷新顶层汇总。
6. 汇报时说明更新了哪些组合、小分、测例和依据日志。

示例：

```markdown
| [accept01](/mnt/sdcard-rv/musl/ltp/testcases/bin/accept01) | PASS |  | `accept01` | disk-ltp-bin | 2026-05-25 output_r_xxx.txt |
```

不要在没有运行证据时把状态改成 `PASS`。

## 从日志更新 scoreboard 的原则

可接受依据：

- QEMU 日志中明确出现对应测例 `[PASS]`。
- LTP Summary 显示 `failed 0`、`broken 0` 且返回值为 0。
- 用户明确说明某组合某测例已经通过。

不可接受依据：

- “构建通过”推断测例通过。
- 其他架构或其他 libc 组合通过，推断当前组合通过。
- 旧日志无法对应当前代码状态。
- 只看到没有 panic，但没有测例通过标记。

## 小分与运行入口的对应关系

- `basic`：磁盘 `basic/` 下的单测；项目内对应 `basic_testcases[]` 和 `basic_test()`。
- `libctest-static`：磁盘 `run-static.sh` 中的 `runtest.exe -w entry-static.exe <case>`。
- `libctest-dynamic`：磁盘 `run-dynamic.sh` 中的 `runtest.exe -w entry-dynamic.exe <case>`。
- `ltp`：磁盘 `ltp/testcases/bin/` 下的 LTP 可执行文件；项目内对应 `ltp_testcases[]`、`ltp_test()`、`ltp_subset_test()`。
- `lua`：磁盘 `lua_testcode.sh` 调用的 Lua 脚本。
- `busybox`：磁盘 `busybox_cmd.txt` 里的 BusyBox 命令。
- `iozone`、`libcbench`、`lmbench`、`unixbench`、`cyclictest`、`iperf`、`netperf`：由各自 `*_testcode.sh` 抽取。

## 调试协作建议

- 修一个小分时，优先只打开该小分或该小分中的目标测例。
- 对 LTP，优先用 `ltp_subset_test()` 跑目标列表。
- 对 basic，优先用 `basic_subset_test()` 或临时缩小 `basic_testcases[]`。
- 每次从 FAIL 变 PASS 后，更新 scoreboard 对应行。
- 如果发现 scoreboard 里测例路径失效，先确认镜像是否挂载；不要直接删行。

## 验证 scoreboard 工具

修改生成器后运行：

```bash
source .venv/bin/activate
which python
python -c 'import py_compile; py_compile.compile("scoreboard/generate_scoreboard.py", cfile="/tmp/f7ly_generate_scoreboard.pyc", doraise=True); print("py_compile ok")'
python scoreboard/generate_scoreboard.py
find scoreboard -maxdepth 4 -type f \( -name "*.html" -o -name "*.json" -o -name "*.pyc" \) | sort
```

最后一个命令应无输出。
