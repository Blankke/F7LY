/**
 * @file iozone_research.cc
 * @brief iozone mClock 研究入口与工具函数
 *
 * 用法说明：
 * 1. 构建：make ARCH=riscv build
 * 2. 运行：make ARCH=riscv run
 * 3. 当前分支会由 initcode-rv 直接进入 iozone mClock 研究入口。
 *
 * 设计目标：
 * - 用最小可复现的双进程 iozone 负载验证高优保底、空闲借带宽与停止后独占。
 * - 这里优先输出“每个子进程耗时 + 近似吞吐 + 退出状态”，方便结合内核日志看调度效果。
 */

#include "user.hh"

namespace
{
    /**
     * @brief 描述一个 iozone 子任务的参数集合
     *
     * @note 字段说明见各成员后的注释
     */
    struct iozone_job
    {
        const char *tag;       ///< 子任务标签，用于日志区分
        const char *file_path; ///< 子任务要操作的文件路径
        const char *size_arg;  ///< 传递给 iozone 的大小参数字符串（如 "8m"）
        int nice_value;        ///< 进程优先级 (nice 值)，范围通常是 -20..19
    };

    /**
     * @brief 保存由 fork/exec 启动的子进程的运行结果和计时信息
     *
     * @note 字段说明见各成员后的注释
     */
    struct child_result
    {
        int pid;                           ///< 子进程 PID
        int wait_status;                   ///< waitpid 返回的原始状态码
        unsigned long long start_us;       ///< 子进程启动时间（微秒）
        unsigned long long finish_us;      ///< 子进程结束时间（微秒）
        const iozone_job *job;             ///< 指向触发该子进程的 iozone_job 配置
    };

    /**
     * @brief 获取当前时间（微秒）
     *
     * 返回自纪元以来的微秒数；若 gettimeofday 失败则返回 0
     * @return 当前时间（微秒）或 0 表示失败
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
     * @brief 将 MiB 单位转换为字节数
     *
     * @param mib MiB 数
     * @return 对应的字节数
     */
    unsigned long long mib_to_bytes(unsigned long long mib)
    {
        return mib * 1024ULL * 1024ULL;
    }

    /**
     * @brief 从字符串（例如 "8m"）中解析出 MiB 数值部分
     *
     * 只解析前导的十进制数字，遇到非数字字符则停止解析。
     * @param size_arg 大小参数字符串
     * @return 解析得到的 MiB 数
     */
    unsigned long long parse_size_mib(const char *size_arg)
    {
        unsigned long long value = 0;
        for (int i = 0; size_arg[i] != '\0'; ++i)
        {
            char ch = size_arg[i];
            if (ch >= '0' && ch <= '9')
            {
                value = value * 10ULL + (unsigned long long)(ch - '0');
                continue;
            }
            break;
        }
        return value;
    }

    /**
     * @brief 打印单个子任务运行结果的摘要
     *
     * 计算运行时长、传输字节数以及近似吞吐并打印日志
     * @param result 子进程运行结果信息
     */
    void print_result(const child_result &result)
    {
        unsigned long long duration_us = result.finish_us > result.start_us
                                             ? result.finish_us - result.start_us
                                             : 0;
        // 从 job->size_arg 中解析 MiB 数
        unsigned long long size_mib = parse_size_mib(result.job->size_arg);
        unsigned long long bytes = mib_to_bytes(size_mib);
        // mib_per_sec_times_100 保存 MiB/s * 100（保留两位小数的整数形式）
        unsigned long long mib_per_sec_times_100 = 0;
        if (duration_us != 0)
        {
            mib_per_sec_times_100 = (size_mib * 100ULL * 1000000ULL) / duration_us;
        }

        printf("[iozone] 子任务=%s pid=%d nice=%d size=%s 耗时=%d us 近似吞吐=%d.%d MiB/s raw_status=0x%x\n",
               result.job->tag,
               result.pid,
               result.job->nice_value,
               result.job->size_arg,
               (int)duration_us,
               (int)(mib_per_sec_times_100 / 100ULL),
               (int)(mib_per_sec_times_100 % 100ULL),
               result.wait_status);
        printf("[iozone] 子任务=%s 文件=%s 传输字节=%d\n",
               result.job->tag,
               result.job->file_path,
               (int)bytes);
    }

    /**
     * @brief fork 并在子进程中通过 execve 启动 iozone 可执行程序
     *
     * @param job 要传递给子进程的 iozone_job 配置
     * @return 在父进程中返回子进程 PID；子进程成功时不会返回（失败会调用 exit）
     */
    int spawn_iozone_child(const iozone_job &job)
    {
        int pid = fork();
        if (pid != 0)
        {
            // 父进程直接返回子进程 PID
            return pid;
        }

        // 子进程：先设置 nice 值以调整优先级
        if (setpriority(PRIO_PROCESS, 0, job.nice_value) != 0)
        {
            printf("[iozone] setpriority 失败 tag=%s errno=%d\n", job.tag, errno);
            exit(111);
        }

        // 构造 execve 的 argv 列表，使用内置的 /musl/iozone 可执行文件
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

        // 用 execve 替换子进程镜像，失败时打印并退出
        execve(argv[0], argv, 0);
        printf("[iozone] execve 失败 tag=%s errno=%d\n", job.tag, errno);
        exit(112);
        return -1;
    }

    /**
     * @brief 以单进程方式运行一个 iozone 子任务并等待其结束
     *
     * @param scenario_name 场景描述，用于日志输出
     * @param job 要运行的 iozone_job
     */
    void run_single_job(const char *scenario_name, const iozone_job &job)
    {
        printf("==== IOZONE 场景开始：%s ====\n", scenario_name);
        // result 保存该子任务的计时与返回信息
        child_result result = {};
        result.job = &job;
        result.start_us = now_usec();
        result.pid = spawn_iozone_child(job);
        // 等待子进程结束并记录退出状态
        waitpid(result.pid, &result.wait_status, 0);
        result.finish_us = now_usec();
        print_result(result);
        printf("==== IOZONE 场景结束：%s ====\n", scenario_name);
    }

    /**
     * @brief 同时启动两个 iozone 子任务用于并发行为测试
     *
     * @param scenario_name 场景描述，用于日志输出
     * @param job_a 第一个任务描述
     * @param job_b 第二个任务描述
     * @note 本函数依次等待两个子进程：先等待 job_a，再等待 job_b
     */
    void run_pair_jobs(const char *scenario_name, const iozone_job &job_a, const iozone_job &job_b)
    {
        printf("==== IOZONE 场景开始：%s ====\n", scenario_name);
        child_result result_a = {};
        child_result result_b = {};
        result_a.job = &job_a;
        result_b.job = &job_b;

        // 分别记录两个子进程的启动时间与 PID
        result_a.start_us = now_usec();
        result_a.pid = spawn_iozone_child(job_a);
        result_b.start_us = now_usec();
        result_b.pid = spawn_iozone_child(job_b);

        // 依次等待并记录结束时间与状态
        waitpid(result_a.pid, &result_a.wait_status, 0);
        result_a.finish_us = now_usec();
        waitpid(result_b.pid, &result_b.wait_status, 0);
        result_b.finish_us = now_usec();

        print_result(result_a);
        print_result(result_b);
        printf("==== IOZONE 场景结束：%s ====\n", scenario_name);
    }
}

/**
 * @brief iozone mclock 研究入口函数（riscv）
 *
 * 在用户空间 initcode 中可直接调用该函数启动研究流程。
 * 主要工作：初始化运行环境、创建测试文件、依次运行单流/并发场景并记录结果
 * @return 0 成功
 */
int iozone_mclock_research_riscv(void)
{
    printf("#### IOZONE MCLOCK RESEARCH START riscv ####\n");
    // 初始化用户态环境（例如将 /musl/ 挂到搜索路径下）
    init_env("/musl/");
    // 确保 /tmp 可用并切换到该目录作为测试工作目录
    mkdir("/tmp", 0777);
    chdir("/tmp");

    // 先跑镜像里现成的 iozone 基线入口，用以校验当前文件系统/块层链路
    iozone_test("/musl/");

    // 定义一组测试任务（iozone_job），用于后续的单流与并发场景
    // baseline: 基线测试，中等大小、默认 nice
    const iozone_job baseline = {"baseline", "/tmp/iozone_baseline.dat", "8m", 0};
    // high_a: 高优先级（nice -20）的大文件写入
    const iozone_job high_a = {"A-high", "/tmp/iozone_a_high.dat", "8m", -20};
    // low_b: 低优先级（nice 19）的大文件写入
    const iozone_job low_b = {"B-low", "/tmp/iozone_b_low.dat", "8m", 19};
    // light_high: 高优但负载较小（2MiB），用于测试轻载下优先级效果
    const iozone_job light_high = {"A-light", "/tmp/iozone_a_light.dat", "2m", -20};
    // long_low: 低优长流（16MiB），用于观察长流与高优的交互
    const iozone_job long_low = {"B-long", "/tmp/iozone_b_long.dat", "16m", 19};
    // pair_high/pair_low: 用于并发最小复现场景（均为 2MiB）
    const iozone_job pair_high = {"pair-A-high", "/tmp/iozone_pair_a.dat", "2m", -20};
    const iozone_job pair_low = {"pair-B-low", "/tmp/iozone_pair_b.dat", "2m", 19};

    // 1. 单流基线：估计设备上限
    run_single_job("单流基线", baseline);

    // 2. A/B 单独运行：确认两者单独运行时都能打满或接近设备能力
    run_single_job("A单独运行", high_a);
    run_single_job("B单独运行", long_low);

    // 3. A/B 同时长期运行：验证 A 优先
    const iozone_job pair_high_long = {"A-high-long", "/tmp/iozone_pair_a_long.dat", "32m", -20};
    const iozone_job pair_low_long = {"B-low-long", "/tmp/iozone_pair_b_long.dat", "32m", 19};
    run_pair_jobs("A+B长期并发：验证A优先", pair_high_long, pair_low_long);

    // 4. A轻载+B长流：验证B借用A未用满带宽
    run_pair_jobs("A轻载+B长流：验证B借用空闲带宽", light_high, long_low);

    printf("#### IOZONE MCLOCK RESEARCH END riscv ####\n");
    return 0;
}
