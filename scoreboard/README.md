# F7LY Test Scoreboard

这个目录是从已挂载的大赛磁盘抽取出来的 Markdown scoreboard。
它只记录测例结构和协作状态，不负责执行测例。

顶层直接汇总四个组合；每个小分链接到对应 Markdown 文件。
Pass测例按小分文件中“是否通过”列的 `PASS` 数统计。没有真实运行结果时不会自动标 PASS。

## riscv musl

| 小分 | 总测例 | Pass测例 | 记录文件 |
| --- | ---: | ---: | --- |
| 当前未挂载或未抽取到测例 | 0 | 0 |  |

## riscv glibc

| 小分 | 总测例 | Pass测例 | 记录文件 |
| --- | ---: | ---: | --- |
| 当前未挂载或未抽取到测例 | 0 | 0 |  |

## loongarch musl

| 小分 | 总测例 | Pass测例 | 记录文件 |
| --- | ---: | ---: | --- |
| 当前未挂载或未抽取到测例 | 0 | 0 |  |

## loongarch glibc

| 小分 | 总测例 | Pass测例 | 记录文件 |
| --- | ---: | ---: | --- |
| 当前未挂载或未抽取到测例 | 0 | 0 |  |


## 挂载状态

| 目标 | 路径 | 状态 |
| --- | --- | --- |
| `riscv` | `/mnt/sdcard-rv` | 存在 |
| `riscv/musl` | `/mnt/sdcard-rv/musl` | 缺失 |
| `riscv/glibc` | `/mnt/sdcard-rv/glibc` | 缺失 |
| `loongarch` | `/mnt/sdcard-la` | 存在 |
| `loongarch/musl` | `/mnt/sdcard-la/musl` | 缺失 |
| `loongarch/glibc` | `/mnt/sdcard-la/glibc` | 缺失 |

## 当前项目默认回归

当前 `regression_suite_4d1444()` 默认开启 `musl/basic` 和 `musl/libctest`。
小分文件只记录协作状态；测例命令和来源由生成器从挂载磁盘重新扫描。
