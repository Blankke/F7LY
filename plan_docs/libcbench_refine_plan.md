# libcbench-test介绍
libc-bench 是 Rich Felker/Eta Labs 做的 libc 性能基准，目标是比较不同 C/POSIX 标准库实现的时间效率和内存效率。它覆盖 malloc、多线程 malloc 竞争、malloc 内存归还、字符串/正则搜索、线程创建与 join、UTF-8 解码、stdio 缓冲读写等。

| 类别       | 例子                                   | 主要瓶颈                          |
| -------- | ------------------------------------ | ----------------------------- |
| 内存分配     | `malloc/free/realloc`                | 分配器算法、碎片、锁竞争、`brk/mmap`       |
| 字符串/内存函数 | `memcpy/memset/strlen/strcmp/strstr` | CPU 指令、对齐、word-at-a-time/SIMD |
| stdio    | `fread/fwrite/printf`                | libc 缓冲、read/write 系统调用       |
| 线程       | `pthread_create/join/mutex`          | clone、futex、调度、TLS            |
| 正则/字符串搜索 | `regex/strstr`                       | 算法复杂度、分支预测、缓存局部性              |
| UTF-8    | 解码/验证                                | 分支、查表、向量化                     |

在性能方面，我们的系统用的是现成 musl libc，那么 memcpy/strlen/malloc 很多逻辑其实在用户态 libc 里，未必能靠改内核直接变快。内核能影响的部分主要是 mmap/brk、页错误、read/write、clone、futex、调度、文件描述符、时间系统调用等。

# 结果分析
我们目前的libcbench性能一般

| 测试点 | rv baseline | rv score | la baseline | la score | 总分 |
|---|---:|---:|---:|---:|---:|
| libc-bench b_malloc_big1 (0) (seconds) | 0.118578071 | 1.0 | 0.118578071 | 1.0 | 2.0 |
| libc-bench b_malloc_big2 (0) (seconds) | 0.107088632 | 1.0 | 0.107088632 | 1.0 | 2.0 |
| libc-bench b_malloc_bubble (0) (seconds) | 0.36015349 | 1.2010600813558685 | 0.36015349 | 1.0436463353444112 | 2.2447064167002795 |
| libc-bench b_malloc_sparse (0) (seconds) | 0.384919462 | 1.2255715040981743 | 0.384919462 | 1.1553370975043085 | 2.3809086016024827 |
| libc-bench b_malloc_thread_local (0) (seconds) | 0.096989785 | 0.0 | 0.096989785 | 1.0 | 1.0 |
| libc-bench b_malloc_thread_stress (0) (seconds) | 0.09632549 | 0.0 | 0.09632549 | 1.0 | 1.0 |
| libc-bench b_malloc_tiny1 (0) (seconds) | 0.014186324 | 1.0 | 0.014186324 | 1.4090287237201125 | 2.4090287237201125 |
| libc-bench b_malloc_tiny2 (0) (seconds) | 0.010810043 | 1.0 | 0.010810043 | 1.2432851562200076 | 2.243285156220008 |
| libc-bench b_pthread_create_serial1 (0) (seconds) | 0.892420718 | 1.4886839908618077 | 0.892420718 | 1.991447105780858 | 3.4801310966426935 |
| libc-bench b_pthread_createjoin_serial1 (0) (seconds) | 1.284791557 | 1.0 | 1.284791557 | 1.986163483171146 | 2.986163483171146 |
| libc-bench b_pthread_createjoin_serial2 (0) (seconds) | 1.006433722 | 1.0 | 1.006433722 | 1.0 | 2.0 |
| libc-bench b_pthread_uselesslock (0) (seconds) | 0.080551368 | 1.0 | 0.080551368 | 1.0 | 2.0 |
| libc-bench b_regex_compile ("(a\|b\|c)*d*b") (seconds) | 0.088259223 | 1.0064596421837977 | 0.088259223 | 1.45220365241602 | 2.458663294599818 |
| libc-bench b_regex_search ("(a\|b\|c)*d*b") (seconds) | 0.083251251 | 0.0 | 0.083251251 | 0.0 | 0.0 |
| libc-bench b_regex_search ("a{25}b") (seconds) | 0.286712251 | 0.0 | 0.286712251 | 0.0 | 0.0 |
| libc-bench b_stdio_putgetc (0) (seconds) | 0.767822967 | 1.0 | 0.767822967 | 1.0 | 2.0 |
| libc-bench b_stdio_putgetc_unlocked (0) (seconds) | 0.765147171 | 1.0 | 0.765147171 | 1.0 | 2.0 |
| libc-bench b_string_memset (0) (seconds) | 0.025313365 | 1.0752632058203244 | 0.025313365 | 1.2654971000497168 | 2.3407603058700412 |
| libc-bench b_string_strchr (0) (seconds) | 0.012811332 | 1.0 | 0.012811332 | 1.0 | 2.0 |
| libc-bench b_string_strlen (0) (seconds) | 0.013459429 | 1.0 | 0.013459429 | 1.0 | 2.0 |
| libc-bench b_string_strstr ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac") (seconds) | 0.013963025 | 1.0 | 0.013963025 | 1.0 | 2.0 |
| libc-bench b_string_strstr ("aaaaaaaaaaaaaaaaaaaaaaac") (seconds) | 0.011749838 | 1.0 | 0.011749838 | 1.0 | 2.0 |
| libc-bench b_string_strstr ("aaaaaaaaaaaaaaccccccccccccc") (seconds) | 0.011944437 | 1.0 | 0.011944437 | 1.0 | 2.0 |
| libc-bench b_string_strstr ("abcdefghijklmnopqrstuvwxyz") (seconds) | 0.011402239 | 1.0 | 0.011402239 | 1.0 | 2.0 |
| libc-bench b_string_strstr ("azbycxdwevfugthsirjqkplomn") (seconds) | 0.020868089 | 1.0 | 0.020868089 | 1.0 | 2.0 |
| libc-bench b_utf8_bigbuf (0) (seconds) | 0.036977802 | 1.0 | 0.036977802 | 1.0 | 2.0 |
| libc-bench b_utf8_onebyone (0) (seconds) | 0.109599811 | 1.2436656665402461 | 0.109599811 | 1.0 | 2.243665666540246 |
| 总分 |  | 24.24070409086022 |  | 28.546608654206608 | 52.78731274506683 |

# 优化方向
针对 libcbench-test：malloc、pthread、string、stdio 优化

libcbench-test 里最有论文价值的是 malloc 和线程同步。

内存分配器方向，[Hoard](https://dl.acm.org/doi/10.1145/378995.379232) 是非常经典的多线程 allocator 论文。它关注多线程分配器的可扩展性，主要解决锁竞争、false sharing、内存膨胀等问题；ACM 摘要说 Hoard 是一个快速、可扩展、内存高效的 allocator，并且很大程度避免 false sharing。

[jemalloc](https://engineering.fb.com/2011/01/03/core-infra/scalable-memory-allocation-using-jemalloc/?utm_source=chatgpt.com) 也是非常重要的工业级 allocator。Facebook 工程文章总结了 jemalloc 的核心算法、数据结构、heap profiling 以及生产环境优化，并比较了多个 allocator。 还有 Jason Evans 的 jemalloc 论文，主题就是 scalable concurrent allocation，也就是面向多处理器/多线程的可扩展分配。

[mimalloc](https://www.microsoft.com/en-us/research/publication/mimalloc-free-list-sharding-in-action/?utm_source=chatgpt.com) 是较新的 allocator 代表，Microsoft Research 页面描述它使用 page-local sharded free lists，提高 locality、降低 contention，并支持高效 allocate/free fast path。

线程同步方向，futex 论文非常相关。[Fuss, Futexes and Furwocks](https://www.kernel.org/doc/ols/2002/ols2002-pages-479-495.pdf?utm_source=chatgpt.com) 讲的是 Linux fast user-level locking：无竞争时尽量在用户态完成，只有竞争时陷入内核等待/唤醒。这个思想直接影响 pthread mutex、condition variable 等同步原语。

具体落地可以往下面的方向优化：
1. malloc/free：
   - size class
   - freelist
   - 小对象缓存
   - 大对象走 mmap/page allocator
   - 避免全局大锁
2. string/memory：
   - memcpy/memset/strlen 使用按 word 处理
   - 处理对齐
   - 不要逐字节慢循环
3. stdio：
   - fread/fwrite 要有较大缓冲
   - 避免每个字符一次 syscall
4. pthread：
   - clone/thread 创建路径要轻
   - mutex/condvar 最好有用户态 fast path + futex slow path
5. syscall：
   - read/write/brk/mmap/clock_gettime 路径减少无意义检查和锁
6. VM：
   - lazy allocation
   - page fault 路径要稳定
   - brk/mmap 不能太重

## 评测逻辑
- 官方的baseline评测机制在这个文件中：https://github.com/oscomp/autotest-for-oskernel/blob/main/kernel/judge/judge_libcbench-musl.py
- 官方的baseline评测机制在这个文件中：https://github.com/oscomp/autotest-for-oskernel/blob/main/kernel/judge/judge_libcbench-glibc.py
## 可参考的开源内核
- https://gitlab.eduxiji.net/T202610346999652/oskernel2026.git 这个内核的iozone分数在排行榜上是最高的，至少不是保底的20分（参考测评文件机制），我们可以借鉴他们做过哪些优化
- https://github.com/Starry-OS/StarryOS.git 优秀的RUST内核，去年的冠军，一定在libcbench方面也有不错的优化，我们可以参考他们的实现

# 优化步骤
针对 libcbench_test 的优化需要按照以下步骤进行：
libcbench测试包括很多小测试，每一次优化都需要针对单独的libcbench测试，选择一个具体的方向进行优化，优化完成后需要验证libcbench测试的结果是否有提升，如果有提升则继续优化下一个测试项，如果没有提升则需要分析原因并进行修复，直到libcbench测试的结果达到官方的baseline的水平为止。

优化的来源可能是排行榜上分数较高的内核，或者是相关的论文和资料。优化的内容它会暴露你的系统调用、线程、内存管理、文件描述符、stdio 支撑是否成熟。需要根据具体的测试项来选择优化的方向。最好先参考已有的内核实现，看看他们是如何优化libcbench测试的，然后再结合相关的论文和资料来进行优化。

优化不能牺牲正确性，任何优化都必须在正确性的前提上完成，优化完成后需要进行完整的libcbench测试，确保所有测试项都能通过，不会因为原子操作等卡死，并且性能有提升。

任何操作不能修改磁盘中的测试文件，只能修改内核的行为。

任务验收标准为达到baseline的分数水平，或者在某些测试项上超过baseline的分数水平。

任务完成后，请在此文档末添加“已完成，待验收”字样，并简要描述成果，不要详细解释你的修改具体内容，也不要总结优化的细节和原理，只需要简单说明优化了哪些测试项，性能提升了多少分。

