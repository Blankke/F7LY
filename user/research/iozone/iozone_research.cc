/**
 * @file iozone_research.cc
 * @brief iozone 研究入口与结果汇总
 *
 * 用法说明：
 * 1. 构建：make ARCH=riscv build
 * 2. 运行：make ARCH=riscv run
 * 3. 当前入口会先执行镜像中的完整 iozone 测试，再运行研究场景。
 */

#include "user.hh"

namespace
{
    /**
     * @brief 描述一个 iozone 子任务
     */
    struct iozone_job
    {
        const char *tag;       ///< 子任务标签
        const char *file_path; ///< 目标文件路径
        const char *size_arg;  ///< 传给 iozone 的文件大小参数
        int nice_value;        ///< 进程 nice 值
    };

    /**
     * @brief 保存一个 iozone 子任务的执行结果
     */
    struct child_result
    {
        int pid;                           ///< 子进程 PID
        int wait_status;                   ///< waitpid 原始返回状态
        unsigned long long start_us;       ///< 启动时间
        unsigned long long finish_us;      ///< 结束时间
        const iozone_job *job;             ///< 关联任务配置
    };

    /**
     * @brief 获取当前时间（微秒）
     */
    unsigned long long now_usec()
    {
        user_timeval tv = {};
        if (gettimeofday(&tv, 0) != 0)
        {
            return 0;
        }
        return (unsigned long long)tv.tv_sec * 1000000ULL + (unsigned long long)tv.tv_usec;
    }

    /**
     * @brief 将 MiB 转成字节数
     */
    unsigned long long mib_to_bytes(unsigned long long mib)
    {
        return mib * 1024ULL * 1024ULL;
    }

    /**
     * @brief 从形如 "8m" 的字符串中提取 MiB 数值
     */
    unsigned long long parse_size_mib(const char *size_arg)
    {
        unsigned long long value = 0;
        for (int i = 0; size_arg[i] != '\0'; ++i)
        {
            char ch = size_arg[i];
            if (ch < '0' || ch > '9')
            {
                break;
            }
            value = value * 10ULL + (unsigned long long)(ch - '0');
        }
        return value;
    }

    /**
     * @brief 打印单个任务的结果摘要
     */
    void print_result(const child_result &result)
    {
        const unsigned long long duration_us =
            result.finish_us > result.start_us ? result.finish_us - result.start_us : 0;
        const unsigned long long size_mib = parse_size_mib(result.job->size_arg);
        const unsigned long long bytes = mib_to_bytes(size_mib);

        unsigned long long mib_per_sec_times_100 = 0;
        if (duration_us != 0)
        {
            mib_per_sec_times_100 = (size_mib * 100ULL * 1000000ULL) / duration_us;
        }

        printf("[iozone] 场景=%s pid=%d nice=%d size=%s 耗时=%d us 吞吐=%d.%d MiB/s raw_status=0x%x\n",
               result.job->tag,
               result.pid,
               result.job->nice_value,
               result.job->size_arg,
               (int)duration_us,
               (int)(mib_per_sec_times_100 / 100ULL),
               (int)(mib_per_sec_times_100 % 100ULL),
               result.wait_status);
        printf("[iozone] 文件=%s 字节=%d\n",
               result.job->file_path,
               (int)bytes);
    }

    /**
     * @brief fork 并在子进程中启动 iozone
     */
    int spawn_iozone_child(const iozone_job &job)
    {
        int pid = fork();
        if (pid != 0)
        {
            return pid;
        }

        if (setpriority(PRIO_PROCESS, 0, job.nice_value) != 0)
        {
            exit(111);
        }

        char *argv[16] = {0};
        argv[0] = (char *)"/musl/iozone";
        argv[1] = (char *)"-i";
        argv[2] = (char *)"0";
        argv[3] = (char *)"-i";
        argv[4] = (char *)"1";
        argv[5] = (char *)"-r";
        argv[6] = (char *)"4k";
        argv[7] = (char *)"-s";
        argv[8] = (char *)job.size_arg;
        argv[9] = (char *)"-f";
        argv[10] = (char *)job.file_path;
        argv[11] = 0;

        execve(argv[0], argv, 0);
        exit(112);
        return -1;
    }

    /**
     * @brief 运行单个 iozone 场景
     */
    void run_single_job(const char *scenario_name, const iozone_job &job)
    {
        printf("==== IOZONE 场景：%s ====\n", scenario_name);

        child_result result = {};
        result.job = &job;
        result.start_us = now_usec();
        result.pid = spawn_iozone_child(job);
        waitpid(result.pid, &result.wait_status, 0);
        result.finish_us = now_usec();
        print_result(result);
    }

    /**
     * @brief 并发运行两个 iozone 场景
     */
    void run_pair_jobs(const char *scenario_name, const iozone_job &job_a, const iozone_job &job_b)
    {
        printf("==== IOZONE 场景：%s ====\n", scenario_name);

        child_result result_a = {};
        child_result result_b = {};
        result_a.job = &job_a;
        result_b.job = &job_b;

        result_a.start_us = now_usec();
        result_a.pid = spawn_iozone_child(job_a);
        result_b.start_us = now_usec();
        result_b.pid = spawn_iozone_child(job_b);

        waitpid(result_a.pid, &result_a.wait_status, 0);
        result_a.finish_us = now_usec();
        waitpid(result_b.pid, &result_b.wait_status, 0);
        result_b.finish_us = now_usec();

        print_result(result_a);
        print_result(result_b);
    }
} // namespace

/**
 * @brief iozone 研究入口
 */
int iozone_priority_borrow_research(void)
{
    init_env("/musl/");
    mkdir("/tmp", 0777);
    chdir("/tmp");

    if (iozone_test("/musl/") != 0)
    {
        return -1;
    }

    const iozone_job baseline = {"baseline", "/tmp/iozone_baseline.dat", "8m", 0};
    const iozone_job high_a = {"A-high", "/tmp/iozone_a_high.dat", "8m", -20};
    const iozone_job light_high = {"A-light", "/tmp/iozone_a_light.dat", "2m", -20};
    const iozone_job long_low = {"B-long", "/tmp/iozone_b_long.dat", "16m", 19};
    const iozone_job pair_high_long = {"A-high-long", "/tmp/iozone_pair_a_long.dat", "32m", -20};
    const iozone_job pair_low_long = {"B-low-long", "/tmp/iozone_pair_b_long.dat", "32m", 19};

    run_single_job("单流基线", baseline);
    run_single_job("A单独运行", high_a);
    run_single_job("B单独运行", long_low);
    run_pair_jobs("A+B长期并发", pair_high_long, pair_low_long);
    run_pair_jobs("A轻载+B长流", light_high, long_low);
    return 0;
}
