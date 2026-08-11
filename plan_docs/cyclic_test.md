# cyclictest 修复记录

状态：已完成，待人工验收。

## 目标

让 RISC-V / LoongArch 的 musl、glibc cyclictest 都能完整跑完：

- NO_STRESS_P1 / NO_STRESS_P8 / STRESS_P1 / STRESS_P8 均输出 success。
- stress 阶段的 hackbench 主进程在 `kill -2` 后能打印 SIGINT 清理链路：
  `Signal 2 caught`、`longjmp'ed out`、`signaling 400 worker threads to terminate`、`kill hackbench: success`。
- 不能靠修改测试脚本规避，应修内核 Linux ABI、调度和阻塞语义。

## 根因与修改

1. `sched_getaffinity()` 返回值语义错误。
   - 根因：libnuma 直接调用 raw syscall，用返回值推导 cpumask 大小；内核成功返回 0 会让 libnuma 得到 size=0。
   - 修改：`kernel/sys/syscall_handler.cc` 中 `sys_sched_getaffinity()` 成功返回 `sizeof(CpuMask)`，同时清空用户 cpuset 缓冲区后只置 CPU0。

2. `sched_*` 的 pid 参数只按进程 pid 查找。
   - 根因：`pthread_setaffinity_np()` / scheduler API 会传 tid，原实现找不到线程。
   - 修改：`sched_getaffinity`、`sched_setaffinity`、`sched_getparam`、`sched_getscheduler`、`sched_setscheduler` 统一使用 `find_live_task_by_pid_or_tid()`。

3. hackbench stress 阶段进程槽不够。
   - 根因：hackbench 默认创建 `10 * 20 * 2 = 400` 个 worker，原 `proc::num_process = 128` 会让 fork/clone 返回 EAGAIN，hackbench 主进程提前退出，脚本后续 `kill` 找不到 pid。
   - 修改：`kernel/libs/param.h` 将 `NPROC` 提升到 512，`kernel/proc/proc.hh` 的 `num_process` 改为引用 `NPROC`，避免两套容量常量分叉。

4. 大运行队列下实时线程和后台 worker 的调度延迟过大。
   - 修改：`kernel/trap/riscv/trap.cc`、`kernel/trap/loongarch/trap.cc` 的 timer 抢占阈值改为每 tick 抢占。
   - 修改：`kernel/proc/scheduler.cc` 增加 effective priority：
     - 有未屏蔽待处理信号的 RUNNABLE 任务临时视为最高优先级。
     - `_has_non_default_priority` 在没有存活非默认优先级任务时自动清回 false。
     - 保留原整表轮转形态，避免同组 cyclictest 线程启动时被第一个 SCHED_FIFO 线程饿住。

5. `ppoll()` 无事件时 yield 忙轮询。
   - 根因：hackbench worker 等 wakefd 时 400 个进程全部保持 RUNNABLE，会淹没单核调度器。
   - 修改：`kernel/sys/syscall_handler.cc` 的 `ppoll` 无事件路径改为 `sleep_n_ticks(1)` 后重检，并保持信号可中断。

6. 实时 cyclictest worker 的子 tick 睡眠仍保持 RUNNABLE。
   - 根因：短 `clock_nanosleep()` 走 busy-yield，SCHED_FIFO worker 会反复抢回 CPU，控制线程难以及时收尾。
   - 修改：`wait_short_timeout()` 中对非默认优先级任务的短超时改为阻塞睡眠 10 tick，收到未屏蔽信号时返回 `EINTR`。

7. AF_UNIX/TCP 小块 socket I/O 没有 handoff。
   - 根因：hackbench 默认使用 `socketpair(AF_UNIX, SOCK_STREAM)` 传 100B 小消息；单个 worker 在一个 tick 内可连续执行大量 send/recv syscall，shell 的下一条 `kill` 会被拖很久。
   - 修改：`kernel/fs/vfs/file/socket_file.cc` 增加小块 stream transfer handoff，成功 send/recv 4096B 以内数据后主动 `yield()` 一次。

8. `wait4()` 阻塞等待不可被普通信号打断。
   - 根因：hackbench parent 阻塞在 `wait()` 等 400 个 worker；收到 SIGINT 后虽然被唤醒，但 `wait4()` 不检查 pending signal，又重新睡回去，用户态 handler 无法执行 longjmp 清理。
   - 修改：`kernel/proc/proc_manager.cc` 的阻塞 `wait4()` 在重新睡眠前检查 `has_unmasked_signal_pending()`，返回 `-EINTR` 让用户态 SIGINT handler 执行。

9. 镜像修改约束。
   - 禁止补丁、挂载写入、原地改写或回写 `images/` 下的原始 sdcard 镜像。
   - 允许复制 sdcard 到 `/tmp` 作为临时运行副本，并在关闭 snapshot 时只改写这个临时副本。

## 验证

语法/构建：

```bash
make build PROFILE=riscv-qemu
make build PROFILE=loongarch-qemu
git diff --check
```

RISC-V 运行：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs/run
img="/tmp/f7ly-rv-cyclictest-wait-eintr-${ts}.img"
log="logs/run/output_r_cyclictest_wait_eintr_${ts}.txt"
cp images/oscomp-final-riscv64.img "$img"
{
  echo "run_at=${ts}"
  echo "arch=riscv"
  echo "scope=cyclictest-wait-eintr"
  echo "cmd=timeout 180s make run PROFILE=riscv-qemu QEMU_DISK=final QEMU_MEM=1G QEMU_RUN_SNAPSHOT= QEMU_STORAGE_IMAGE=$img"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 180s make run PROFILE=riscv-qemu QEMU_DISK=final QEMU_MEM=1G QEMU_RUN_SNAPSHOT= QEMU_STORAGE_IMAGE="$img"
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

结果：`logs/run/output_r_cyclictest_wait_eintr_20260620-002754.txt`

- cyclictest-musl：四段 success，hackbench SIGINT/longjmp/reap 输出完整，`kill hackbench: success`，Time 11.029。
- cyclictest-glibc：四段 success，hackbench SIGINT/longjmp/reap 输出完整，`kill hackbench: success`，Time 28.999。
- `exit_code=0`。

LoongArch 运行：

```bash
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs/run
img="/tmp/f7ly-la-cyclictest-wait-eintr-${ts}.img"
log="logs/run/output_l_cyclictest_wait_eintr_${ts}.txt"
cp images/oscomp-final-loongarch64.img "$img"
{
  echo "run_at=${ts}"
  echo "arch=loongarch"
  echo "scope=cyclictest-wait-eintr"
  echo "cmd=timeout 240s make run PROFILE=loongarch-qemu QEMU_DISK=final QEMU_MEM=1G QEMU_RUN_SNAPSHOT= QEMU_STORAGE_IMAGE=$img"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 240s make run PROFILE=loongarch-qemu QEMU_DISK=final QEMU_MEM=1G QEMU_RUN_SNAPSHOT= QEMU_STORAGE_IMAGE="$img"
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

结果：`logs/run/output_l_cyclictest_wait_eintr_20260620-002952.txt`

- cyclictest-musl：四段 success，hackbench SIGINT/longjmp/reap 输出完整，`kill hackbench: success`，Time 12.001。
- cyclictest-glibc：四段 success，hackbench SIGINT/longjmp/reap 输出完整，`kill hackbench: success`，Time 27.107。
- `exit_code=0`。
- 仍有 `WARN: High resolution timers not available`，但不阻断 cyclictest 完整流程。

## 注意

- stress 清理期间偶尔会出现 `SENDER: write (error: Broken pipe)`，这是 SIGTERM 清理 worker 时发送端看到对端关闭后的输出；关键判定链路已经具备 `Signal 2 caught`、`longjmp'ed out`、`signaling 400 worker threads to terminate`、`kill hackbench: success`。
- 后续运行允许复制 sdcard 到 `/tmp` 作为临时副本；禁止改写、补丁、挂载写入或回写 `images/` 下的原始 sdcard 镜像。
- 未提交 commit。
