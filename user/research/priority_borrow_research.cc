/**
 * @file priority_borrow_research.cc
 * @brief priority-borrow 调度实验入口：使用手写长时块设备 worker 验证优先级压制与带宽借用
 *
 * 使用示例：
 * 1. 在 `user/app/initcode-rv.cc` 中启用 `priority_borrow_research();`
 * 2. 构建：`make build ARCH=riscv`
 * 3. 运行：`make run r QEMU_MEM=1G`
 *
 * 说明：
 * - 这里不依赖 iozone/FIO 产生 IO，而是直接在用户态持续发起对齐 read/write 请求。
 * - 默认使用原始块设备 read 作为实验负载，既能压到 virtio 块层队列，又不会破坏 ext4 镜像。
 * - 每个 worker 按固定时长持续访问自己的块设备区域，更贴近“长时间并发争用同一设备”的题意。
 * - A/B 的 CPU nice 被固定为同级，只通过独立的 IO nice 让块层看到优先级差异。
 */

#include "user.hh"

namespace
{
    constexpr int k_direct_chunk_bytes = 64 * 1024;
    constexpr unsigned long long k_mib_bytes = 1024ULL * 1024ULL;
    constexpr unsigned long long k_usec_per_sec = 1000000ULL;
    constexpr int k_max_group_workers = 24;

    /**
     * @brief initcode 以扁平二进制方式装载，超大 .bss 不可靠，所以改成运行时 mmap。
     */
    static char *g_write_buffer = nullptr;

    struct io_job
    {
        const char *tag;                     ///< worker 标签，仅用于日志
        const char *file_path;               ///< 目标文件或块设备路径
        bool use_direct_device;              ///< true 表示直接访问块设备
        bool write_request;                  ///< true 表示写请求，false 表示读请求
        unsigned long long start_offset_bytes; ///< 本 worker 的起始偏移
        unsigned long long region_bytes;     ///< 本 worker 可循环覆盖的独占区域大小
        unsigned long long runtime_usec;     ///< 持续发起 IO 的总时长
        int nice_value;                      ///< 块层优先级（IO nice）
        int pause_usec_after_chunk;          ///< 每个 chunk 之后的额外停顿，用于模拟“未吃满配额”
        char fill_byte;                      ///< 写请求用于区分不同 worker 内容，读请求不关心
    };

    struct io_report
    {
        int status_code;                     ///< 0 表示成功，负值表示本地错误
        int sys_errno;                       ///< 最近一次失败时的 errno
        int pid;                             ///< worker pid
        int requested_nice;                  ///< 请求设置的 IO nice
        int observed_nice;                   ///< 实际应用的 IO nice
        int observed_cpu_nice;               ///< 实际 CPU nice，用于确认 CPU 调度不再干扰实验
        unsigned long long bytes_completed;  ///< 该 worker 在持续时间窗口内完成的总 IO 字节数
        unsigned long long start_us;         ///< 真正开始发起 IO 的时刻
        unsigned long long finish_us;        ///< 停止发起 IO 的时刻
    };

    struct child_ctx
    {
        io_job job;
        int pid;
        int ready_r;
        int ready_w;
        int go_r;
        int go_w;
        int report_r;
        int report_w;
        int wait_status;
        bool has_report;
        io_report report;
    };

    struct io_group_job
    {
        const char *tag;                       ///< A/B 组标签
        const char *file_prefix;               ///< 直接块设备模式下即目标设备路径
        int workers;                           ///< 组内 worker 数，必须 <= k_max_group_workers
        bool use_direct_device;                ///< true 表示直接对块设备发起对齐请求
        bool write_request;                    ///< true 表示写请求，false 表示读请求
        unsigned long long base_offset_bytes;  ///< 第 0 个 worker 的起始偏移
        unsigned long long worker_stride_bytes; ///< 相邻 worker 的偏移步长
        unsigned long long worker_region_bytes; ///< 每个 worker 独占区域大小
        unsigned long long runtime_usec;       ///< 每个 worker 的持续时间
        int nice_value;                        ///< 该组 worker 的 IO nice
        int pause_usec_after_chunk;            ///< 每个 chunk 之后的额外停顿
        char fill_byte_base;                   ///< 用于区分 worker 的填充值
    };

    struct io_group_report
    {
        int status_code;                        ///< 0 表示整组成功
        int worker_count;                       ///< 组内 worker 总数
        int successful_workers;                 ///< 成功 worker 数
        int requested_nice;                     ///< 请求设置的 IO nice
        int observed_nice_min;                  ///< 实际最小 IO nice
        int observed_nice_max;                  ///< 实际最大 IO nice
        int observed_cpu_nice_min;              ///< 实际最小 CPU nice
        int observed_cpu_nice_max;              ///< 实际最大 CPU nice
        unsigned long long total_bytes;         ///< 整组在持续时间窗口内完成的总字节数
        unsigned long long earliest_start_us;   ///< 最早开始发起 IO 的时刻
        unsigned long long latest_finish_us;    ///< 最晚停止发起 IO 的时刻
    };

    struct group_ctx
    {
        io_group_job job;
        child_ctx workers[k_max_group_workers];
        char tag_storage[k_max_group_workers][32];
        char path_storage[k_max_group_workers][64];
    };

    void zero_bytes(void *buffer, size_t bytes)
    {
        char *cursor = reinterpret_cast<char *>(buffer);
        for (size_t i = 0; i < bytes; ++i)
        {
            cursor[i] = 0;
        }
    }

    size_t append_cstr(char *buffer, size_t cap, size_t pos, const char *text)
    {
        if (buffer == nullptr || cap == 0)
        {
            return 0;
        }
        if (text == nullptr)
        {
            text = "";
        }

        while (text[0] != '\0' && pos + 1 < cap)
        {
            buffer[pos++] = *text++;
        }
        buffer[pos] = '\0';
        return pos;
    }

    size_t append_uint_decimal(char *buffer, size_t cap, size_t pos, unsigned int value)
    {
        char digits[16];
        int len = 0;
        do
        {
            digits[len++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0 && len < static_cast<int>(sizeof(digits)));

        while (len > 0 && pos + 1 < cap)
        {
            buffer[pos++] = digits[--len];
        }
        buffer[pos] = '\0';
        return pos;
    }

    void build_worker_text(char *buffer, size_t cap, const char *prefix, unsigned int worker_index, const char *suffix)
    {
        size_t pos = 0;
        if (cap == 0)
        {
            return;
        }

        buffer[0] = '\0';
        pos = append_cstr(buffer, cap, pos, prefix);
        pos = append_cstr(buffer, cap, pos, "-");
        pos = append_uint_decimal(buffer, cap, pos, worker_index);
        (void)append_cstr(buffer, cap, pos, suffix);
    }

    unsigned long long now_usec()
    {
        user_timeval tv;
        zero_bytes(&tv, sizeof(tv));
        if (gettimeofday(&tv, 0) != 0)
        {
            return 0;
        }
        return static_cast<unsigned long long>(tv.tv_sec) * k_usec_per_sec +
               static_cast<unsigned long long>(tv.tv_usec);
    }

    int ensure_write_buffer()
    {
        if (g_write_buffer != nullptr)
        {
            return 0;
        }

        void *buffer = mmap(nullptr,
                            k_direct_chunk_bytes,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            -1,
                            0);
        if (buffer == MAP_FAILED)
        {
            return -1;
        }

        g_write_buffer = reinterpret_cast<char *>(buffer);
        return 0;
    }

    void fill_write_buffer(char fill_byte)
    {
        if (g_write_buffer == nullptr)
        {
            return;
        }
        for (int i = 0; i < k_direct_chunk_bytes; ++i)
        {
            g_write_buffer[i] = fill_byte;
        }
    }

    int write_exact(int fd, const void *buffer, int bytes)
    {
        const char *cursor = reinterpret_cast<const char *>(buffer);
        int done = 0;
        while (done < bytes)
        {
            int rc = write(fd, cursor + done, static_cast<size_t>(bytes - done));
            if (rc <= 0)
            {
                return -1;
            }
            done += rc;
        }
        return done;
    }

    int read_exact(int fd, void *buffer, int bytes)
    {
        char *cursor = reinterpret_cast<char *>(buffer);
        int done = 0;
        while (done < bytes)
        {
            int rc = read(fd, cursor + done, static_cast<size_t>(bytes - done));
            if (rc <= 0)
            {
                return -1;
            }
            done += rc;
        }
        return done;
    }

    void sleep_usec_approx(int pause_usec)
    {
        if (pause_usec <= 0)
        {
            return;
        }

        const unsigned long long start = now_usec();
        while (now_usec() - start < static_cast<unsigned long long>(pause_usec))
        {
            sched_yield();
        }
    }

    void close_if_open(int &fd)
    {
        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }
    }

    int decode_wait_status(int raw_status, bool &exited_normally)
    {
        exited_normally = (raw_status & 0x7f) == 0;
        if (exited_normally)
        {
            return (raw_status >> 8) & 0xff;
        }
        return -(raw_status & 0x7f);
    }

    unsigned long long group_throughput_x100(const io_group_report &report)
    {
        if (report.total_bytes == 0 || report.latest_finish_us <= report.earliest_start_us)
        {
            return 0;
        }
        const unsigned long long duration_us = report.latest_finish_us - report.earliest_start_us;
        return report.total_bytes * 100ULL * k_usec_per_sec /
               (duration_us * k_mib_bytes);
    }

    unsigned long long ratio_x100(unsigned long long numerator, unsigned long long denominator)
    {
        if (denominator == 0)
        {
            return 0;
        }
        return numerator * 100ULL / denominator;
    }

    void print_x100(unsigned long long value_x100)
    {
        const int integer = static_cast<int>(value_x100 / 100ULL);
        const int fraction = static_cast<int>(value_x100 % 100ULL);
        printf("%d.", integer);
        if (fraction < 10)
        {
            printf("0");
        }
        printf("%d", fraction);
    }

    void init_child_ctx(child_ctx &ctx, const io_job &job)
    {
        ctx.job = job;
        ctx.pid = -1;
        ctx.ready_r = -1;
        ctx.ready_w = -1;
        ctx.go_r = -1;
        ctx.go_w = -1;
        ctx.report_r = -1;
        ctx.report_w = -1;
        ctx.wait_status = -1;
        ctx.has_report = false;
        zero_bytes(&ctx.report, sizeof(ctx.report));
    }

    void close_child_ctx_parent_fds(child_ctx &ctx)
    {
        close_if_open(ctx.ready_w);
        close_if_open(ctx.go_r);
        close_if_open(ctx.report_w);
    }

    void close_child_ctx_child_fds(child_ctx &ctx)
    {
        close_if_open(ctx.ready_r);
        close_if_open(ctx.go_w);
        close_if_open(ctx.report_r);
    }

    int write_report_and_exit(child_ctx &ctx, io_report &report, int exit_code)
    {
        report.finish_us = now_usec();
        if (ctx.report_w >= 0)
        {
            (void)write_exact(ctx.report_w, &report, sizeof(report));
        }
        close_if_open(ctx.report_w);
        close_if_open(ctx.ready_w);
        close_if_open(ctx.go_r);
        exit(exit_code);
        return exit_code;
    }

    int rewind_worker_region(int fd, child_ctx &ctx, io_report &report)
    {
        if (lseek(fd, static_cast<off_t>(ctx.job.start_offset_bytes), SEEK_SET) < 0)
        {
            report.status_code = -23;
            report.sys_errno = errno;
            return -1;
        }
        return 0;
    }

    int child_run_job(child_ctx &ctx)
    {
        io_report report;
        zero_bytes(&report, sizeof(report));
        report.status_code = -1;
        report.sys_errno = 0;
        report.pid = getpid();
        report.requested_nice = ctx.job.nice_value;
        report.observed_nice = 0;
        report.observed_cpu_nice = getpriority(PRIO_PROCESS, 0);
        report.bytes_completed = 0;
        report.start_us = 0;
        report.finish_us = 0;

        if (ctx.job.runtime_usec == 0 || ctx.job.region_bytes < static_cast<unsigned long long>(k_direct_chunk_bytes))
        {
            report.status_code = -5;
            report.sys_errno = 22;
            return write_report_and_exit(ctx, report, 105);
        }

        int applied_io_nice = userdebug5(ctx.job.nice_value);
        if (applied_io_nice < 0)
        {
            report.status_code = -10;
            report.sys_errno = errno;
            return write_report_and_exit(ctx, report, 110);
        }
        report.observed_nice = applied_io_nice;

        // 研究场景里把 CPU nice 固定为 0，避免“CPU 谁先跑起来”掩盖块层效果。
        if (setpriority(PRIO_PROCESS, 0, 0) != 0)
        {
            report.status_code = -11;
            report.sys_errno = errno;
            return write_report_and_exit(ctx, report, 111);
        }
        report.observed_cpu_nice = getpriority(PRIO_PROCESS, 0);

        if (!ctx.job.use_direct_device)
        {
            unlink(const_cast<char *>(ctx.job.file_path));
        }
        int fd = openat(AT_FDCWD,
                        ctx.job.file_path,
                        ctx.job.use_direct_device
                            ? (ctx.job.write_request ? O_RDWR : O_RDONLY)
                            : (ctx.job.write_request ? (O_CREAT | O_TRUNC | O_RDWR) : O_RDONLY));
        if (fd < 0)
        {
            report.status_code = -20;
            report.sys_errno = errno;
            return write_report_and_exit(ctx, report, 120);
        }

        if (ctx.job.use_direct_device && rewind_worker_region(fd, ctx, report) != 0)
        {
            close(fd);
            return write_report_and_exit(ctx, report, 123);
        }

        if (ensure_write_buffer() != 0)
        {
            close(fd);
            report.status_code = -25;
            report.sys_errno = errno;
            return write_report_and_exit(ctx, report, 125);
        }
        if (ctx.job.write_request)
        {
            fill_write_buffer(ctx.job.fill_byte);
        }

        char ready = 'R';
        if (write_exact(ctx.ready_w, &ready, 1) != 1)
        {
            close(fd);
            report.status_code = -30;
            report.sys_errno = errno;
            return write_report_and_exit(ctx, report, 130);
        }
        close_if_open(ctx.ready_w);

        char go = 0;
        if (read_exact(ctx.go_r, &go, 1) != 1 || go != 'G')
        {
            close(fd);
            report.status_code = -40;
            report.sys_errno = errno;
            return write_report_and_exit(ctx, report, 140);
        }
        close_if_open(ctx.go_r);

        report.start_us = now_usec();
        const unsigned long long deadline_us = report.start_us + ctx.job.runtime_usec;
        const unsigned long long region_start = ctx.job.start_offset_bytes;
        const unsigned long long region_end = region_start + ctx.job.region_bytes;
        unsigned long long current_offset = region_start;

        while (now_usec() < deadline_us)
        {
            if (ctx.job.use_direct_device && current_offset + static_cast<unsigned long long>(k_direct_chunk_bytes) > region_end)
            {
                if (rewind_worker_region(fd, ctx, report) != 0)
                {
                    close(fd);
                    return write_report_and_exit(ctx, report, 151);
                }
                current_offset = region_start;
            }

            int io_rc = ctx.job.write_request
                            ? write_exact(fd, g_write_buffer, k_direct_chunk_bytes)
                            : read_exact(fd, g_write_buffer, k_direct_chunk_bytes);
            if (io_rc != k_direct_chunk_bytes)
            {
                close(fd);
                report.status_code = -50;
                report.sys_errno = errno;
                return write_report_and_exit(ctx, report, 150);
            }
            if (ctx.job.write_request && !ctx.job.use_direct_device && fdatasync(fd) != 0)
            {
                close(fd);
                report.status_code = -60;
                report.sys_errno = errno;
                return write_report_and_exit(ctx, report, 160);
            }

            report.bytes_completed += static_cast<unsigned long long>(k_direct_chunk_bytes);
            current_offset += static_cast<unsigned long long>(k_direct_chunk_bytes);

            if (ctx.job.pause_usec_after_chunk > 0)
            {
                sleep_usec_approx(ctx.job.pause_usec_after_chunk);
            }
        }

        close(fd);
        report.status_code = 0;
        return write_report_and_exit(ctx, report, 0);
    }

    int spawn_child(child_ctx &ctx, const io_job &job)
    {
        init_child_ctx(ctx, job);

        int ready_pipe[2] = {-1, -1};
        int go_pipe[2] = {-1, -1};
        int report_pipe[2] = {-1, -1};
        if (pipe(ready_pipe) != 0 || pipe(go_pipe) != 0 || pipe(report_pipe) != 0)
        {
            close_if_open(ready_pipe[0]);
            close_if_open(ready_pipe[1]);
            close_if_open(go_pipe[0]);
            close_if_open(go_pipe[1]);
            close_if_open(report_pipe[0]);
            close_if_open(report_pipe[1]);
            return -1;
        }

        ctx.ready_r = ready_pipe[0];
        ctx.ready_w = ready_pipe[1];
        ctx.go_r = go_pipe[0];
        ctx.go_w = go_pipe[1];
        ctx.report_r = report_pipe[0];
        ctx.report_w = report_pipe[1];

        int pid = fork();
        if (pid < 0)
        {
            close_if_open(ctx.ready_r);
            close_if_open(ctx.ready_w);
            close_if_open(ctx.go_r);
            close_if_open(ctx.go_w);
            close_if_open(ctx.report_r);
            close_if_open(ctx.report_w);
            return -1;
        }

        if (pid == 0)
        {
            close_child_ctx_child_fds(ctx);
            return child_run_job(ctx);
        }

        ctx.pid = pid;
        close_child_ctx_parent_fds(ctx);
        return 0;
    }

    int wait_child_ready(child_ctx &ctx)
    {
        char ready = 0;
        int rc = read_exact(ctx.ready_r, &ready, 1);
        close_if_open(ctx.ready_r);
        return rc == 1 && ready == 'R' ? 0 : -1;
    }

    void send_child_go(child_ctx &ctx)
    {
        const char go = 'G';
        (void)write_exact(ctx.go_w, &go, 1);
        close_if_open(ctx.go_w);
    }

    int collect_child(child_ctx &ctx)
    {
        ctx.has_report = read_exact(ctx.report_r, &ctx.report, sizeof(ctx.report)) == static_cast<int>(sizeof(ctx.report));
        close_if_open(ctx.report_r);
        return waitpid(ctx.pid, &ctx.wait_status, 0);
    }

    void init_group_ctx(group_ctx &ctx, const io_group_job &job)
    {
        ctx.job = job;
        for (int i = 0; i < k_max_group_workers; ++i)
        {
            zero_bytes(ctx.tag_storage[i], sizeof(ctx.tag_storage[i]));
            zero_bytes(ctx.path_storage[i], sizeof(ctx.path_storage[i]));
        }
    }

    int spawn_group(group_ctx &ctx)
    {
        if (ctx.job.workers <= 0 || ctx.job.workers > k_max_group_workers)
        {
            return -1;
        }

        for (int i = 0; i < ctx.job.workers; ++i)
        {
            build_worker_text(ctx.tag_storage[i], sizeof(ctx.tag_storage[i]), ctx.job.tag, static_cast<unsigned int>(i), "");
            if (ctx.job.use_direct_device)
            {
                (void)append_cstr(ctx.path_storage[i], sizeof(ctx.path_storage[i]), 0, ctx.job.file_prefix);
            }
            else
            {
                build_worker_text(ctx.path_storage[i], sizeof(ctx.path_storage[i]), ctx.job.file_prefix, static_cast<unsigned int>(i), ".dat");
            }

            io_job child_job = {
                ctx.tag_storage[i],
                ctx.path_storage[i],
                ctx.job.use_direct_device,
                ctx.job.write_request,
                ctx.job.base_offset_bytes + static_cast<unsigned long long>(i) * ctx.job.worker_stride_bytes,
                ctx.job.worker_region_bytes,
                ctx.job.runtime_usec,
                ctx.job.nice_value,
                ctx.job.pause_usec_after_chunk,
                static_cast<char>(ctx.job.fill_byte_base + i)};

            if (spawn_child(ctx.workers[i], child_job) != 0)
            {
                return -1;
            }
        }
        return 0;
    }

    int wait_group_ready(group_ctx &ctx)
    {
        for (int i = 0; i < ctx.job.workers; ++i)
        {
            if (wait_child_ready(ctx.workers[i]) != 0)
            {
                return -1;
            }
        }
        return 0;
    }

    void stop_group_before_go(group_ctx &ctx)
    {
        for (int i = 0; i < ctx.job.workers; ++i)
        {
            close_if_open(ctx.workers[i].go_w);
        }
    }

    void cleanup_group_files(const io_group_job &job)
    {
        if (job.use_direct_device)
        {
            return;
        }

        char path[64];
        for (int i = 0; i < job.workers; ++i)
        {
            zero_bytes(path, sizeof(path));
            build_worker_text(path, sizeof(path), job.file_prefix, static_cast<unsigned int>(i), ".dat");
            unlink(path);
        }
    }

    void send_group_go(group_ctx &ctx)
    {
        for (int i = 0; i < ctx.job.workers; ++i)
        {
            send_child_go(ctx.workers[i]);
        }
    }

    void send_group_go_interleaved(group_ctx &a_ctx, group_ctx &b_ctx)
    {
        /*
         * 同步 read/write worker 每个进程一次只会挂一个块层请求。
         * 为了稳定复现“高优先级 backlog 存在时低优先级被压制”，先让 A 组预热出
         * 一批 pending 请求，再放行 B 组。两个组的运行窗口仍然长时间重叠，
         * 吞吐统计按各自 worker 的真实开始/结束时间计算。
         */
        send_group_go(a_ctx);
        sleep_usec_approx(200000);
        send_group_go(b_ctx);
    }

    void print_group_worker_failures(const char *scenario_name, const group_ctx &ctx)
    {
        for (int i = 0; i < ctx.job.workers; ++i)
        {
            const child_ctx &worker = ctx.workers[i];
            bool exited_normally = false;
            const int wait_result = decode_wait_status(worker.wait_status, exited_normally);
            const bool ok = worker.has_report &&
                            worker.report.status_code == 0 &&
                            exited_normally &&
                            wait_result == 0;
            if (!ok)
            {
                printf("[priority-borrow] 场景=%s worker=%s pid=%d io(req=%d got=%d) cpu=%d bytes=%dMiB report=%d errno=%d wait=0x%x\n",
                       scenario_name,
                       worker.job.tag,
                       worker.pid,
                       worker.job.nice_value,
                       worker.has_report ? worker.report.observed_nice : 0,
                       worker.has_report ? worker.report.observed_cpu_nice : 0,
                       worker.has_report ? static_cast<int>(worker.report.bytes_completed / k_mib_bytes) : 0,
                       worker.has_report ? worker.report.status_code : -999,
                       worker.has_report ? worker.report.sys_errno : 0,
                       worker.wait_status);
            }
        }
    }

    int collect_group(group_ctx &ctx, io_group_report &report_out)
    {
        io_group_report report;
        zero_bytes(&report, sizeof(report));
        report.status_code = 0;
        report.worker_count = ctx.job.workers;
        report.successful_workers = 0;
        report.requested_nice = ctx.job.nice_value;
        report.observed_nice_min = 127;
        report.observed_nice_max = -127;
        report.observed_cpu_nice_min = 127;
        report.observed_cpu_nice_max = -127;

        bool all_ok = true;
        for (int i = 0; i < ctx.job.workers; ++i)
        {
            child_ctx &worker = ctx.workers[i];
            if (collect_child(worker) < 0)
            {
                all_ok = false;
                continue;
            }

            bool exited_normally = false;
            const int wait_result = decode_wait_status(worker.wait_status, exited_normally);
            if (!worker.has_report || worker.report.status_code != 0 || !exited_normally || wait_result != 0)
            {
                all_ok = false;
                continue;
            }

            ++report.successful_workers;
            report.total_bytes += worker.report.bytes_completed;
            if (report.earliest_start_us == 0 || worker.report.start_us < report.earliest_start_us)
            {
                report.earliest_start_us = worker.report.start_us;
            }
            if (worker.report.finish_us > report.latest_finish_us)
            {
                report.latest_finish_us = worker.report.finish_us;
            }
            if (worker.report.observed_nice < report.observed_nice_min)
            {
                report.observed_nice_min = worker.report.observed_nice;
            }
            if (worker.report.observed_nice > report.observed_nice_max)
            {
                report.observed_nice_max = worker.report.observed_nice;
            }
            if (worker.report.observed_cpu_nice < report.observed_cpu_nice_min)
            {
                report.observed_cpu_nice_min = worker.report.observed_cpu_nice;
            }
            if (worker.report.observed_cpu_nice > report.observed_cpu_nice_max)
            {
                report.observed_cpu_nice_max = worker.report.observed_cpu_nice;
            }
        }

        if (!all_ok || report.successful_workers != ctx.job.workers)
        {
            report.status_code = -1;
        }
        if (report.successful_workers == 0)
        {
            report.observed_nice_min = ctx.job.nice_value;
            report.observed_nice_max = ctx.job.nice_value;
            report.observed_cpu_nice_min = 0;
            report.observed_cpu_nice_max = 0;
        }

        report_out = report;
        return report.status_code;
    }

    void print_group_result(const char *scenario_name, const io_group_job &job, const io_group_report &report)
    {
        printf("[priority-borrow] 场景=%s group=%s op=%s workers=%d runtime=%ds io(req=%d got=%d..%d) cpu=%d..%d 成功=%d/%d 总IO=%dMiB 吞吐=",
               scenario_name,
               job.tag,
               job.write_request ? "write" : "read",
               job.workers,
               static_cast<int>(job.runtime_usec / k_usec_per_sec),
               job.nice_value,
               report.observed_nice_min,
               report.observed_nice_max,
               report.observed_cpu_nice_min,
               report.observed_cpu_nice_max,
               report.successful_workers,
               report.worker_count,
               static_cast<int>(report.total_bytes / k_mib_bytes));
        print_x100(group_throughput_x100(report));
        printf("MiB/s status=%d\n", report.status_code);
    }

    void print_group_pair_summary(const char *scenario_name,
                                  const io_group_report &a_report,
                                  const io_group_report &b_report)
    {
        if (a_report.status_code != 0 || b_report.status_code != 0)
        {
            printf("[priority-borrow] 场景=%s 无法生成组汇总：至少一个作业组失败\n", scenario_name);
            return;
        }

        const unsigned long long a_speed = group_throughput_x100(a_report);
        const unsigned long long b_speed = group_throughput_x100(b_report);
        printf("[priority-borrow] 场景=%s 组吞吐 A/B=", scenario_name);
        print_x100(ratio_x100(a_speed, b_speed));
        printf("\n");
    }

    int run_single_group_scenario(const char *scenario_name,
                                  const io_group_job &job,
                                  io_group_report &report_out)
    {
        printf("==== PRIORITY BORROW 场景：%s ====\n", scenario_name);

        group_ctx ctx;
        init_group_ctx(ctx, job);
        if (spawn_group(ctx) != 0)
        {
            printf("[priority-borrow] 启动作业组失败：%s\n", scenario_name);
            return -1;
        }
        if (wait_group_ready(ctx) != 0)
        {
            printf("[priority-borrow] 作业组未能全部进入 ready：%s\n", scenario_name);
            stop_group_before_go(ctx);
            (void)collect_group(ctx, report_out);
            print_group_result(scenario_name, job, report_out);
            print_group_worker_failures(scenario_name, ctx);
            cleanup_group_files(job);
            return -1;
        }

        userdebug4();
        send_group_go(ctx);
        (void)collect_group(ctx, report_out);
        print_group_result(scenario_name, job, report_out);
        if (report_out.status_code != 0)
        {
            print_group_worker_failures(scenario_name, ctx);
            cleanup_group_files(job);
            return -1;
        }

        cleanup_group_files(job);
        return 0;
    }

    int run_pair_group_scenario(const char *scenario_name,
                                const io_group_job &job_a,
                                const io_group_job &job_b,
                                io_group_report &report_a,
                                io_group_report &report_b)
    {
        printf("==== PRIORITY BORROW 场景：%s ====\n", scenario_name);

        group_ctx a_ctx;
        group_ctx b_ctx;
        init_group_ctx(a_ctx, job_a);
        init_group_ctx(b_ctx, job_b);
        if (spawn_group(a_ctx) != 0 || spawn_group(b_ctx) != 0)
        {
            printf("[priority-borrow] 启动作业组失败：%s\n", scenario_name);
            return -1;
        }
        if (wait_group_ready(a_ctx) != 0 || wait_group_ready(b_ctx) != 0)
        {
            printf("[priority-borrow] 作业组未能全部进入 ready：%s\n", scenario_name);
            stop_group_before_go(a_ctx);
            stop_group_before_go(b_ctx);
            (void)collect_group(a_ctx, report_a);
            (void)collect_group(b_ctx, report_b);
            print_group_result(scenario_name, job_a, report_a);
            print_group_result(scenario_name, job_b, report_b);
            print_group_worker_failures(scenario_name, a_ctx);
            print_group_worker_failures(scenario_name, b_ctx);
            cleanup_group_files(job_a);
            cleanup_group_files(job_b);
            return -1;
        }

        userdebug2();
        send_group_go_interleaved(a_ctx, b_ctx);
        (void)collect_group(a_ctx, report_a);
        (void)collect_group(b_ctx, report_b);
        userdebug4();

        print_group_result(scenario_name, job_a, report_a);
        print_group_result(scenario_name, job_b, report_b);
        print_group_pair_summary(scenario_name, report_a, report_b);

        if (report_a.status_code != 0 || report_b.status_code != 0)
        {
            print_group_worker_failures(scenario_name, a_ctx);
            print_group_worker_failures(scenario_name, b_ctx);
            cleanup_group_files(job_a);
            cleanup_group_files(job_b);
            return -1;
        }

        cleanup_group_files(job_a);
        cleanup_group_files(job_b);
        return 0;
    }

    void print_group_final_comparison(const io_group_report &a_alone,
                                      const io_group_report &b_alone,
                                      const io_group_report &a_contended,
                                      const io_group_report &b_contended,
                                      const io_group_report &a_light,
                                      const io_group_report &b_borrow,
                                      const io_group_report &a_short,
                                      const io_group_report &b_after)
    {
        const unsigned long long a_alone_x100 = group_throughput_x100(a_alone);
        const unsigned long long b_alone_x100 = group_throughput_x100(b_alone);
        const unsigned long long a_contended_x100 = group_throughput_x100(a_contended);
        const unsigned long long b_contended_x100 = group_throughput_x100(b_contended);
        const unsigned long long a_light_x100 = group_throughput_x100(a_light);
        const unsigned long long b_borrow_x100 = group_throughput_x100(b_borrow);
        const unsigned long long a_short_x100 = group_throughput_x100(a_short);
        const unsigned long long b_after_x100 = group_throughput_x100(b_after);

        printf("==== PRIORITY BORROW 最终结论 ====\n");
        printf("[priority-borrow][目标1] A/B 满载并发：A 组=");
        print_x100(a_contended_x100);
        printf("MiB/s, B 组=");
        print_x100(b_contended_x100);
        printf("MiB/s, A/B=");
        print_x100(ratio_x100(a_contended_x100, b_contended_x100));
        printf("\n");

        printf("[priority-borrow][目标2] A 轻载时：A 组=");
        print_x100(a_light_x100);
        printf("MiB/s, B 组=");
        print_x100(b_borrow_x100);
        printf("MiB/s, B 借用/B 单独=");
        print_x100(ratio_x100(b_borrow_x100, b_alone_x100));
        printf("\n");

        printf("[priority-borrow][目标3] A 先结束后：A 组=");
        print_x100(a_short_x100);
        printf("MiB/s, B 组=");
        print_x100(b_after_x100);
        printf("MiB/s, B 独占/B 单独=");
        print_x100(ratio_x100(b_after_x100, b_alone_x100));
        printf("\n");

        printf("[priority-borrow][基线] A 单独=");
        print_x100(a_alone_x100);
        printf("MiB/s, B 单独=");
        print_x100(b_alone_x100);
        printf("MiB/s\n");

        const unsigned long long full_pressure_ratio = ratio_x100(a_contended_x100, b_contended_x100);
        const unsigned long long borrow_vs_contended = ratio_x100(b_borrow_x100, b_contended_x100);
        const unsigned long long borrow_vs_alone = ratio_x100(b_borrow_x100, b_alone_x100);
        const unsigned long long after_vs_alone = ratio_x100(b_after_x100, b_alone_x100);

        /*
         * 这些阈值只用于让日志自动给出 PASS/FAIL：
         * 目标 1 要求 A 在满载并发时明显压过 B；
         * 目标 2 要求 A 轻载时 B 比满载争抢时拿到更多带宽，同时不能离单独基线太远；
         * 目标 3 用 A 只运行 1 秒、B 运行 6 秒的长窗口验证 A 停止后 B 能接近独占。
         */
        const bool goal1_pass = full_pressure_ratio >= 150ULL;
        const bool goal2_pass = borrow_vs_contended >= 150ULL && borrow_vs_alone >= 50ULL;
        const bool goal3_pass = after_vs_alone >= 70ULL;
        printf("[priority-borrow][验收] 目标1=%s 目标2=%s 目标3=%s borrow/contended=",
               goal1_pass ? "PASS" : "FAIL",
               goal2_pass ? "PASS" : "FAIL",
               goal3_pass ? "PASS" : "FAIL");
        print_x100(borrow_vs_contended);
        printf(" after/alone=");
        print_x100(after_vs_alone);
        printf("\n");
    }
} // namespace

int priority_borrow_research(void)
{
    init_env("/musl/");
    mkdir("/tmp", 0777);
    chdir("/tmp");
    userdebug4();

    /**
     * @brief 预留给 A/B 的两个大区间，避免不同组在长时间实验中互相踩同一段缓存/设备区域。
     *
     * 这里依赖 64 位 `lseek` wrapper，可以安全使用 2GiB 以上偏移。
     */
    constexpr unsigned long long k_raw_device_a_base = 2147483648ULL; // 2.0 GiB
    constexpr unsigned long long k_raw_device_b_base = 3221225472ULL; // 3.0 GiB
    constexpr unsigned long long k_worker_region_bytes = 32ULL * k_mib_bytes;
    constexpr unsigned long long k_worker_stride_bytes = k_worker_region_bytes;
    constexpr unsigned long long k_long_runtime_usec = 6ULL * k_usec_per_sec;
    constexpr unsigned long long k_short_runtime_usec = 1ULL * k_usec_per_sec;
    constexpr int k_workers = 16;

    /**
     * @brief 采用“固定时长”而不是“固定总 IO 量”。
     *
     * 这样 A/B 会在同一时间窗口内持续争抢设备，B 不会在 A 提前跑完后再把吞吐补回来，
     * 更利于观察题目要求的“压制”“借用”“独占”三种行为。
     */
    const io_group_job a_alone_job = {
        "A-alone", "/dev/block/8:0", k_workers, true, false,
        k_raw_device_a_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_long_runtime_usec, 0, 0, 'A'};
    const io_group_job b_alone_job = {
        "B-alone", "/dev/block/8:0", k_workers, true, false,
        k_raw_device_b_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_long_runtime_usec, 19, 0, 'a'};
    const io_group_job a_contended_job = {
        "A-high", "/dev/block/8:0", k_workers, true, false,
        k_raw_device_a_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_long_runtime_usec, 0, 0, 'q'};
    const io_group_job b_contended_job = {
        "B-low", "/dev/block/8:0", k_workers, true, false,
        k_raw_device_b_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_long_runtime_usec, 19, 0, 'Q'};
    const io_group_job a_light_job = {
        "A-light", "/dev/block/8:0", 2, true, false,
        k_raw_device_a_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_long_runtime_usec, 0, 8000, 'g'};
    const io_group_job b_borrow_job = {
        "B-borrow", "/dev/block/8:0", k_workers, true, false,
        k_raw_device_b_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_long_runtime_usec, 19, 0, 'G'};
    const io_group_job a_short_job = {
        "A-short", "/dev/block/8:0", k_workers, true, false,
        k_raw_device_a_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_short_runtime_usec, 0, 0, 'm'};
    const io_group_job b_after_job = {
        "B-after", "/dev/block/8:0", k_workers, true, false,
        k_raw_device_b_base, k_worker_stride_bytes, k_worker_region_bytes,
        k_long_runtime_usec, 19, 0, 'M'};

    io_group_report a_alone;
    io_group_report b_alone;
    io_group_report a_contended;
    io_group_report b_contended;
    io_group_report a_light;
    io_group_report b_borrow;
    io_group_report a_short;
    io_group_report b_after;
    zero_bytes(&a_alone, sizeof(a_alone));
    zero_bytes(&b_alone, sizeof(b_alone));
    zero_bytes(&a_contended, sizeof(a_contended));
    zero_bytes(&b_contended, sizeof(b_contended));
    zero_bytes(&a_light, sizeof(a_light));
    zero_bytes(&b_borrow, sizeof(b_borrow));
    zero_bytes(&a_short, sizeof(a_short));
    zero_bytes(&b_after, sizeof(b_after));

    if (run_single_group_scenario("A 作业组单独占用设备", a_alone_job, a_alone) != 0)
    {
        return -1;
    }
    if (run_single_group_scenario("B 作业组单独占用设备", b_alone_job, b_alone) != 0)
    {
        return -1;
    }
    if (run_pair_group_scenario("A/B 两组长时满载读盘", a_contended_job, b_contended_job, a_contended, b_contended) != 0)
    {
        return -1;
    }
    if (run_pair_group_scenario("A 轻载 + B 借用空闲带宽", a_light_job, b_borrow_job, a_light, b_borrow) != 0)
    {
        return -1;
    }
    if (run_pair_group_scenario("A 短任务结束后 B 继续独占", a_short_job, b_after_job, a_short, b_after) != 0)
    {
        return -1;
    }

    print_group_final_comparison(a_alone, b_alone, a_contended, b_contended, a_light, b_borrow, a_short, b_after);
    return 0;
}
