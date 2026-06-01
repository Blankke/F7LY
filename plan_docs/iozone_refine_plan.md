# IOzone 测试解析
它不只是测“磁盘速度”，而是在测整个文件 I/O 栈。不同子项含义大概是：
| 项目                  | 主要测什么        | 对内核的压力                      |
| ------------------- | ------------ | --------------------------- |
| `write`             | 第一次顺序写文件     | 块分配、写路径、缓存、脏块回写             |
| `rewrite`           | 覆盖已有文件       | 写缓存、块映射、元数据较少               |
| `read`              | 第一次顺序读       | buffer cache miss、块设备读      |
| `reread`            | 再读一遍         | buffer cache/page cache 命中率 |
| `random read/write` | 随机访问         | 缓存、寻址、块映射、I/O 合并能力          |
| `stride read`       | 跨步读          | 预读策略是否误判                    |
| `fwrite/fread`      | stdio 缓冲 I/O | libc 缓冲 + 内核 write/read     |
| 小 `reclen`          | 小块记录 I/O     | 系统调用开销、锁、缓存元数据开销            |

# 结果分析
我们当前的iozone_test分数效能很差，主要是因为我们在文件系统和块设备层的性能都不太好，导致iozone的测试结果很差。我们需要分析iozone的测试结果，找出性能瓶颈，并进行优化。
据一般情况分析，如果你的 iozone 很差，通常不是一个点的问题，而是可能出在：文件系统元数据太重、没有 buffer cache、每次写都同步落盘、没有 readahead、块设备请求没有合并、路径里 copy 太多、锁太粗、fsync/close 语义过重等。
- 首先，我们需要分析iozone的测试结果，找出性能瓶颈。我们可以通过iozone的输出结果，分析每个测试项的性能表现.

| 测试点 | rv result | rv baseline | rv score | la result | la baseline | la score | 总分 |
|---|---:|---:|---:|---:|---:|---:|---:|
| iozone fwrite/fread 4 freaders (kb/sec) | 205.27 | 7123.18 | 1.0 | 344.26 | 7123.18 | 1.0 | 2.0 |
| iozone fwrite/fread 4 fwriters (kb/sec) | 866.31 | 3204.25 | 1.0 | 1361.03 | 3204.25 | 1.0 | 2.0 |
| iozone pwrite/pread 4 pread readers (kb/sec) | 441.98 | 14183.61 | 1.0 | 555.41 | 14183.61 | 1.0 | 2.0 |
| iozone pwrite/pread 4 pwrite writers (kb/sec) | 979.62 | 3724.0 | 1.0 | 1743.58 | 3724.0 | 1.0 | 2.0 |
| iozone pwritev/preadv 4 initial writers (kb/sec) | 1101.05 | 3609.25 | 1.0 | 1512.59 | 3609.25 | 1.0 | 2.0 |
| iozone pwritev/preadv 4 rewriters (kb/sec) | 1074.32 | 8305.9 | 1.0 | 1670.78 | 8305.9 | 1.0 | 2.0 |
| iozone random-read 4 initial writers (kb/sec) | 1190.88 | 3401.72 | 1.0 | 2140.57 | 3401.72 | 1.0 | 2.0 |
| iozone random-read 4 random readers (kb/sec) | 391.84 | 11082.03 | 1.0 | 536.36 | 11082.03 | 1.0 | 2.0 |
| iozone random-read 4 random writers (kb/sec) | 232.55 | 7140.81 | 1.0 | 336.72 | 7140.81 | 1.0 | 2.0 |
| iozone random-read 4 rewriters (kb/sec) | 1168.89 | 7196.32 | 1.0 | 2663.53 | 7196.32 | 1.0 | 2.0 |
| iozone read-backwards 4 initial writers (kb/sec) | 1180.77 | 3331.39 | 1.0 | 2173.46 | 3331.39 | 1.0 | 2.0 |
| iozone read-backwards 4 reverse readers (kb/sec) | 328.13 | 10743.42 | 1.0 | 523.3 | 10743.42 | 1.0 | 2.0 |
| iozone read-backwards 4 rewriters (kb/sec) | 1464.64 | 4906.05 | 1.0 | 1900.94 | 4906.05 | 1.0 | 2.0 |
| iozone stride-read 4 initial writers (kb/sec) | 932.03 | 3468.09 | 1.0 | 1623.78 | 3468.09 | 1.0 | 2.0 |
| iozone stride-read 4 rewriters (kb/sec) | 1008.07 | 7004.64 | 1.0 | 1565.01 | 7004.64 | 1.0 | 2.0 |
| iozone stride-read 4 stride readers (kb/sec) | 311.45 | 11848.11 | 1.0 | 417.73 | 11848.11 | 1.0 | 2.0 |
| iozone write/read 4 initial writers (kb/sec) | 1086.85 | 3524.04 | 1.0 | 2078.42 | 3524.04 | 1.0 | 2.0 |
| iozone write/read 4 re-readers (kb/sec) | 503.55 | 13064.75 | 1.0 | 791.94 | 13064.75 | 1.0 | 2.0 |
| iozone write/read 4 readers (kb/sec) | 536.06 | 13135.64 | 1.0 | 873.05 | 13135.64 | 1.0 | 2.0 |
| iozone write/read 4 rewriters (kb/sec) | 1257.46 | 7471.07 | 1.0 | 2059.13 | 7471.07 | 1.0 | 2.0 |
| 总分 |  |  | 20.0 |  |  | 20.0 | 40.0 |

# 优化方向
针对 iozone_test：文件系统与块 I/O 优化

第一类是文件系统布局与局部性。经典论文是 McKusick 等人的 [Fast File System](https://docs-archive.freebsd.org/44doc/smm/05.fastfs/paper.html)，也就是 FFS。它的核心思想包括更大的块、fragment、cylinder group、把相关 inode 和数据放近一些，用布局局部性提升顺序和小文件性能。

第二类是日志结构文件系统。Rosenblum 和 Ousterhout 的 [LFS](https://dl.acm.org/doi/10.1145/146941.146943)提出把修改顺序写入 log，从而把大量小随机写转化为顺序写，对小文件写、元数据写很有启发。ACM 页面摘要也说明 LFS 的核心是把所有修改以类似日志的结构顺序写入。

第三类是元数据一致性优化。[Soft Updates](https://www.usenix.org/conference/1999-usenix-annual-technical-conference/soft-updates-technique-eliminating-most) 这篇很值得看，它的目标是消除大多数同步元数据写，通过依赖关系维护一致性，同时把原来很多同步写变成可延迟、可聚合的写；USENIX 页面提到它可以在文件密集环境下降低 40% 到 70% 的磁盘写。

第四类是面向 flash/SSD/eMMC/SD 卡的文件系统。[F2FS](https://www.usenix.org/conference/fast15/technical-sessions/presentation/lee) 是 FAST 2015 的经典论文，它针对现代 flash 存储设计，建立在 append-only logging 思路上，并根据 flash 特性做数据结构和算法设计.

具体落地可以往下面的方向优化：
1. buffer cache：读过的块必须缓存，reread 应明显变快。
2. write-back cache：不要每个 write 都同步写盘，允许脏块延迟回写。
3. readahead：顺序 read 时提前读后续块。
4. I/O 合并：连续 block 合并成大请求。
5. 元数据延迟更新：减少 inode/bitmap/目录项反复同步写。
6. 块分配局部性：文件连续块尽量连续分配。
7. 减少 syscall/copy 开销：小 reclen 下这个很明显。

## 测评逻辑
- 官方的baseline评测机制在这个文件中：https://github.com/oscomp/autotest-for-oskernel/blob/main/kernel/judge/judge_iozone-glibc.py
    - https://github.com/oscomp/autotest-for-oskernel/blob/main/kernel/judge/judge_iozone-musl.py

## 可参考的开源内核
- https://gitlab.eduxiji.net/T202610346999652/oskernel2026.git 这个内核的iozone分数在排行榜上是比较的，至少不是保底的20分（参考测评文件机制），我们可以借鉴他们做过哪些优化
- https://gitlab.eduxiji.net/T2026103369910203/T2026103369910203DDUHDU 去年霸榜的内核今年再次参赛，他们的优化是比较全面的，我们可以参考他们做过哪些优化。
- https://github.com/Starry-OS/StarryOS.git 优秀的RUST内核，去年的冠军，一定在iozone方面也有不错的优化，我们可以参考他们的实现

# 优化步骤
针对 iozone_test 的优化需要按照以下步骤进行：
iozone测试包括很多小测试，每一次优化都需要针对单独的iozone测试，选择一个具体的方向进行优化，优化完成后需要验证iozone测试的结果是否有提升，如果有提升则继续优化下一个测试项，如果没有提升则需要分析原因并进行修复，直到iozone测试的结果达到官方的baseline的水平为止。

优化的来源可能是排行榜上分数较高的内核，或者是相关的论文和资料。优化的内容可能涉及文件系统设计、块设备调度、缓存管理、系统调用接口等多个方面，需要根据具体的测试项来选择优化的方向。最好先参考已有的内核实现，看看他们是如何优化iozone测试的，然后再结合相关的论文和资料来进行优化。

优化不能牺牲正确性，任何优化都必须在正确性的前提上完成，优化完成后需要进行完整的iozone测试，确保所有测试项都能通过，不会因为原子操作等卡死，并且性能有提升。

任何操作不能修改磁盘中的测试文件，只能修改内核的行为。

任务验收标准为达到baseline的分数水平，或者在某些测试项上超过baseline的分数水平。

任务完成后，请在此文档末添加“已完成，待验收”字样，并简要描述成果，不要详细解释你的修改具体内容，也不要总结优化的细节和原理，只需要简单说明优化了哪些测试项，性能提升了多少分。

## 已完成，待验收（2026-06-01）

iozone 四组合均已完整跑完，官方 judge 脚本可解析子项 80/80，缺项 0。当前 RISC-V 得分 40.618，LoongArch 得分 49.085，四组合合计 89.703，较官方 baseline floor 80 高 9.703 分；主要提升了 LoongArch iozone 读写组合，并确保 RISC-V 不再出现缺项。
