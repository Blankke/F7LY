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

## 5. 后续 BuildStorm selfhost

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
