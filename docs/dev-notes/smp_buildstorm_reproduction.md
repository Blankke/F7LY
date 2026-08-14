# SMP 与 BuildStorm 内核能力复现方法

本文只记录代码层已完成的复现入口和已经实际跑过的短验收。命令默认在仓库根目录 `/home/czc/F7LY` 执行，不提交 `images/` 下的原始镜像改动。

## 1. 双架构编译

```bash
make build PROFILE=riscv-qemu MODE=evaluation
make build PROFILE=loongarch-qemu MODE=evaluation
make build PROFILE=riscv-qemu MODE=shell
make build PROFILE=loongarch-qemu MODE=shell
```

## 2. SMP CPU 吞吐与 affinity

```bash
scripts/run/smp_cpu_bench.sh \
  --arch all --worker-list 1,2,4,8 --seconds 3 --max-prime 1200
```

该脚本在同一台 8-vCPU QEMU 中比较 1/2/4/8 个 worker，worker 使用 `sched_setaffinity` 固定到不同 CPU，并持续通过 `getcpu()` 验证没有迁移。RV 和 LA 都使用对应的 `images/oscomp-final-*.img` 决赛完整 rootfs。

本轮实际结果：

- RV：1/2/4/8 worker 均 PASS，events/s 为 11524.665、24957.165、50653.834、99504.986，8 worker 加速比 8.634。
- LA：1/2/4/8 worker 均 PASS，events/s 为 16101.520、33044.438、66386.127、123023.106，8 worker 加速比 7.640、效率 95.51%。

## 3. RV 真实 stress-ng

```bash
scripts/run/smp_stress_ng.sh \
  --seconds 3 --runs 1 --warmup-seconds 1
```

脚本确认 guest 中存在真实 `/usr/bin/stress-ng`、在线 CPU 为 8，并解析 `metrics-brief` 的 CPU bogo ops/s。实际结果为 1/2/4/8 worker 吞吐 35.270、69.190、142.120、271.240 bogo ops/s，8 worker 加速比 7.690、效率 96.13%。

## 4. 8 GiB PMM 与跨 CPU TLB shootdown

```bash
scripts/run/tlb_shootdown_test.sh --arch all --rounds 20
```

专项程序把两个线程固定到 CPU0/CPU1，覆盖 mprotect 降权、预期 SIGSEGV、munmap、MAP_FIXED 重映射和重新访问。`sysinfo` 与 `/proc/meminfo` 均读取动态 PMM 容量。

本轮实际结果：

- RV：`managed_pages=1751684`，20 轮、40 次 fault、20 次 mprotect、20 次 munmap 全部 PASS。
- LA：`managed_pages=1947687`，同样全部 PASS。

## 5. `22f5813` 以来的性能优化方法

这一阶段的提交按“先证明瓶颈、再改变不变量、最后用完整冷构建验收”收口，不能把某个测例名或工具名写成生产代码的设计理由。可复用的方法如下：

1. **测量边界固定。** 以官方计分命令的 guest 单调时钟为准，正式轮次删除旧产物并记录 CPU 数、产物大小和日志；宿主采样、GDB 栈、性能计数器只用于定位，不改变 guest 时间或工作量。
2. **并发状态先定义所有权。** 调度、wait channel、futex、进程退出和地址空间释放统一锁序与最终引用所有权；唤醒只扫描活跃候选并只唤醒能取得执行权的等待者，避免全表扫描、惊群和跨核 ABBA。
3. **地址空间按共享对象而不是线程维护。** `CLONE_VM` 线程共享 mm/ASID，TLB shootdown 只发往当前运行该 mm 的 CPU；批量 teardown、干净文件页回收和分配失败重试共同控制内存峰值。架构相关粒度（例如 LoongArch 双页 TLB、页表叶覆盖范围）必须从页表契约推导，不能散落 magic number。
4. **文件系统把连续性贯穿到底。** 路径组件、inode/bcache 热点必须由权威增删路径失效；连续 extent 的查询、分配、I/O 和 cache 一致性处理均按 run 批量执行。FIFO 写者精确唤醒，完整块数据 I/O 移出全局元数据排他区，缩短锁持有时间而不放宽一致性。
5. **平台差异集中在画像。** 最大 CPU 数、次核启动时限和架构能力由 platform profile/CPU 探测给出；共享内核只消费契约。这样 QEMU 画像可以使用评测机并行度，物理板画像仍保持其真实容量。
6. **每次优化保留三层门禁。** 先做双架构编译与无浮点检查，再做 VM/ext4/SMP 定向回归，最后做不复用 `target` 的完整构建。定向测试不能替代完整构建，单次完整构建也不能替代外部评测机复验。

对应历史阶段：`22f5813` 建立 SMP/容量基础，`7d30c3c` 收口进程与 VFS 容量，`d092673` 增加缓存、退出/TLB 批处理和诊断，`28ea121` 建立性能采样，`6fc72fc`/`6fb3a7d` 统一平台画像，`8706e3b` 与 `69dc930` 收口原子库、freestanding 标志和构建 jobserver。本轮继续把这些做法落实为通用内存回收、画像容量和连续 extent 分配/I/O，不在生产注释中保留针对单一负载的特判叙述。

## 6. 2026-08-14 完整冷构建结果

正式探针按 final-2026 的计时边界删除旧 `target/debug`，仅计 `cargo xtask arceos build`：

- RV：8 vCPU，`ok=true`，1693.23 秒，产物 1681000 字节；相对评测机旧结果 3223.75 秒缩短 47.5%。日志：`logs/run/output_r_20260814-191332_buildstorm-perf.txt`。
- LA：12 vCPU，`ok=true`，1367.35 秒，产物 1714568 字节；相对评测机旧结果 2564.93 秒缩短 46.7%。日志：`logs/run/output_l_20260814-194725_buildstorm-perf.txt`。
- LA 内层 QEMU 内存路径：复刻 `PROT_NONE` 预留、2 MiB 对齐、`MAP_FIXED` 激活、裁边和 128 MiB 全量触页，16 个子进程轮次全部通过。日志：`logs/run/output_la_qemu_mmap_stress_reclaim_smp12_20260814.txt`。
- RV/LA ext4 并发 `write/fsync/rename/read/unlink` 均通过，临时镜像只读 `e2fsck` 通过。日志：`logs/run/output_rv_ext4_concurrency_smp8_mem8G_20260814-191146.txt`、`logs/run/output_la_ext4_concurrency_smp8_mem8G_20260814-191221.txt`。

本机决赛 rootfs 没有内层 `qemu-system-loongarch64`，因此不能在 guest 内启动编译产物；上述等价 mmap 压力用于验证旧日志中的 ENOMEM 根因，最终得分仍以官方 harness 为准。

## 7. 后续 BuildStorm selfhost

以下入口已经保留，实际执行需要下载 RISC-V Debian 基础镜像并准备 Rust/Cargo 离线依赖，属于后续长时间验收：

```bash
scripts/selfbuild/prepare_rootfs.sh --download-base --build-prepared
scripts/selfbuild/self_compile.sh --level 0
scripts/selfbuild/self_compile.sh --level 1
scripts/selfbuild/self_compile.sh --level 2
scripts/selfbuild/self_compile.sh --level 3 --l3-jobs 1
scripts/selfbuild/self_compile.sh --level 5 --l5-seconds 1800
```

selfhost 使用 `build/selfbuild/` 工作副本和 QEMU `-snapshot`/可写副本，不回写原始评测镜像；每一级失败都应保留该级日志后再修复，不应把后续级别的结果当作前置级别的 PASS。
