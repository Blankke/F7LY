= 修复与优化设计实现

== 调度：从全表扫描到活跃候选

调度器为每个 home CPU 维护可运行任务位图和原子压力计数。任务进入或离开 RUNNABLE 时更新索引；调度器先读取候选位图，再获取对应 PCB 锁确认状态和唯一运行权。初始选核只比较各 online CPU 的活跃压力，复杂度从反复扫描全 PCB 表收敛为 O(`NUMCPU`) 原子读取。

跨核唤醒通过语义化 `kick_cpu`/IPI 通知目标 CPU，不依赖周期性扫描碰巧发现任务。运行期不启用激进迁移：home CPU 和执行权规则保持稳定，避免为了某一轮吞吐破坏 affinity、sleep 或 guest 时间行为。

== futex：统一锁序与精确候选

futex key 根据 private/shared 属性转换为 mm 私有键或物理页键，并登记到固定 bucket。WAIT 和 WAKE 统一遵循“futex bucket/全局锁 → PCB 锁”的顺序：WAIT 在锁内重新读取用户值，值不匹配立即返回 `-EAGAIN`；匹配后原子发布 key 和等待状态。WAKE 只扫描相同 bucket 和 key 的候选，并只唤醒能取得执行权的任务。

robust list、超时、信号中断、`FUTEX_REQUEUE` 和 `CLONE_CHILD_CLEARTID` 复用同一状态模型，防止退出清理重新引入反向锁序。该设计同时解决 ABBA、丢唤醒和惊群扫描，而不是为 BuildStorm 返回固定成功。

== 地址空间：共享 mm 拥有 ASID 与最终清理权

`ProcessMemoryManager` 是页表、VMA、VmObject、ASID、TLB generation、活跃 CPU mask 和引用计数的权威所有者。`CLONE_VM` 线程共享同一 mm/ASID；fork 创建独立 mm；exec 成功后原子替换 mm。PCB 在归还前先摘除 mm 指针，本次原子引用递减结果唯一决定谁执行最终 `free_all_memory()`。

页表变化只向当前运行该 mm 的 CPU 发送定向 range/ASID shootdown。本地和远端完成失效、generation 得到确认后，才允许释放旧 PTE、COW 页或整个地址空间。teardown 先批量撤销 VMA/PTE，再集中释放页表与后端引用，减少逐页同步。

RISC-V 后端使用 SBI remote fence；LoongArch 使用 IOCSR IPI 和 request/ack generation，并按双页 TLB 粒度覆盖末端区间。架构差异留在 HAL，通用 VMA 代码只消费统一的同步契约。

== 文件系统：权威失效、连续 extent 与缩短排他区

路径组件缓存同时保存正向和负向结果，但 create、rename、link、unlink 和目录 rename 必须从权威目录项操作位置使相关缓存失效。root 打开路径合并前缀 resolve/stat，symlink 使用单遍扫描；普通读采用 relatime 条件更新，减少无意义的 atime 写放大。

ext4 挂载锁采用可重入 FIFO 睡眠锁，同 owner 嵌套只增加深度，其他线程按顺序睡眠。bcache 使用独立锁和 O(1) dirty 链，完整块数据 I/O 移出全局元数据排他区；连续 extent 查询、分配、I/O 和 cache 一致性失效按 run 批量执行。这样缩短锁持有时间，同时保留 fsync、close、rename 和读后可见性。

文件页缓存按文件对象和页偏移复用 clean 页面。分配失败时可以有界淘汰 clean 文件页再重试；dirty 页不能被当作普通可回收页丢弃。exec、mmap 和重复动态库读取因此可以共享底层页，但 truncate 和文件修改仍必须使对应页失效并保持 SIGBUS/EOF 语义。

== 大范围映射：统一 64 位契约

VMA 长度、起止地址、页偏移和文件偏移统一使用可覆盖用户地址空间的 64 位类型。拆分、合并、扩展、`mremap`、fork 克隆、后备缺页和“文件基准偏移 + VMA 页偏移”都显式检查加法溢出和底层接口边界。

上层 mmap/fork 无条件调用统一的用户范围准备入口；只有 LoongArch 在入口内部按 TLB refill 契约预建必要页表骨架。架构行为不再通过上层重复分支实现，避免 RV 和 LA 的普通映射路径逐渐分叉。

== 可选性能观测框架

`PERF_DIAG=1` 生成独立诊断内核，普通内核完全不暴露相关 proc ABI。诊断框架采用静态指标注册和 per-CPU 计数，避免热点路径争用一个全局统计锁；syscall 记录 count/time ticks，profile 支持 timer/PMU、符号化热点和可选调用链。

符号表通过首次链接 ELF 生成，二次链接后校验 text 地址稳定。`f7ly-perf` 以 `/proc/f7ly/perf` v1 TSV 为数据源，将内核 ABI 与人类可读/JSON 输出分开。frame pointer、采样和统计开销只存在于 `-perf` 产物，最终正式计时必须回到普通内核。

== 三层验证门禁

每次优化按以下顺序验收：

1. 双架构或四画像构建、`git diff --check`、内核无浮点/向量门禁；
2. 与改动对应的 futex、TLB、VMA、ext4、SMP 或性能 ABI 窄测；
3. 空 target 的完整 BuildStorm、CAgent 连续回归及需要时的 Docker harness。

某层失败时不继续扩大工作量。窄测通过但完整构建失败时，结论应写成“对应不变量已验证，综合负载仍开放”，而不是把计划项直接标为全部完成。
