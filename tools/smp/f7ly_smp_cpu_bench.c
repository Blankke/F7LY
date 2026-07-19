/*
 * F7LY 静态多核 CPU 压测器
 *
 * 用法示例：
 *   /f7ly_smp_cpu_bench --workers 2 --seconds 3 --max-prime 2000
 *
 * 这是一个可交叉静态编译到 RISC-V/LoongArch 的 sysbench cpu 风格负载。
 * 它使用 pthread worker（与 sysbench CPU workload 的线程模型一致），每个
 * worker 都绑定到确定 CPU，周期性调用 getcpu(2) 采样实际运行核。因此输出
 * 同时验证 CPU 密集负载、线程调度、亲和性和 getcpu 返回路径。
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>

enum
{
    k_max_workers = 8,
    k_default_workers = 2,
    k_default_seconds = 3,
    k_default_max_prime = 2000,
};

enum WorkerErrorStage
{
    k_error_none = 0,
    k_error_set_affinity = 1,
    k_error_start_getcpu = 2,
    k_error_sample_getcpu = 3,
    k_error_start_clock = 4,
    k_error_loop_clock = 5,
    k_error_end_clock = 6,
    k_error_end_getcpu = 7,
};

struct WorkerResult
{
    int requested_cpu;
    int start_cpu;
    int end_cpu;
    // 直接保留起止 getcpu 的原始写回值。八核压测出现过“系统调用返回 0、
    // 但用户输出位置未更新”的现象；这些字段能区分内核 copy_out、寄存器
    // 返回和用户态后续写入三类问题，避免把诊断误判为亲和性迁移。
    long start_getcpu_return;
    int start_getcpu_raw_cpu;
    long end_getcpu_return;
    int end_getcpu_raw_cpu;
    int error_number;
    int error_stage;
    uint64_t events;
    uint64_t elapsed_ns;
    uint64_t observed_cpu_mask;
};

struct WorkerContext
{
    int requested_cpu;
    int seconds;
    uint32_t max_prime;
    struct WorkerResult result;
};

// 每个 worker 独占一个槽位，既防止编译器删除素数计算，也不会引入共享写竞争。
static volatile uint64_t g_work_sink[k_max_workers];
static atomic_int g_ready_workers;
static atomic_int g_start_gate;
static atomic_int g_abort_gate;

struct GetcpuTrace
{
    long syscall_return;
    int raw_cpu;
};

static uint64_t timespec_to_ns(const struct timespec *value)
{
    return (uint64_t)value->tv_sec * 1000000000ULL + (uint64_t)value->tv_nsec;
}

// 亲和性和 getcpu 是本压测器的核心验收接口。直接遵循 RV/LA Linux syscall ABI，
// 避免 libc 可变参数 syscall() 在异常路径中依赖 TLS errno 而掩盖内核原始返回值。
static long raw_syscall3(long number, long arg0, long arg1, long arg2)
{
#if defined(__riscv)
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a7 __asm__("a7") = number;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#elif defined(__loongarch__)
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a7 __asm__("a7") = number;
    __asm__ volatile("syscall 0"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#else
    errno = 0;
    const long result = syscall(number, arg0, arg1, arg2);
    return result == -1 ? -(long)errno : result;
#endif
}

static long raw_getcpu(unsigned int *cpu_id, unsigned int *node_id)
{
#if defined(__riscv)
    register long a0 __asm__("a0") = (long)cpu_id;
    register long a1 __asm__("a1") = (long)node_id;
    register long a2 __asm__("a2") = 0;
    register long a7 __asm__("a7") = SYS_getcpu;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#elif defined(__loongarch__)
    register long a0 __asm__("a0") = (long)cpu_id;
    register long a1 __asm__("a1") = (long)node_id;
    register long a2 __asm__("a2") = 0;
    register long a7 __asm__("a7") = SYS_getcpu;
    __asm__ volatile("syscall 0"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#else
    errno = 0;
    const long result = syscall(SYS_getcpu, cpu_id, node_id, NULL);
    return result == -1 ? -(long)errno : result;
#endif
}

static int raw_syscall_failed(long result)
{
    return result < 0 && result >= -4095;
}

static int read_current_cpu(int *cpu_id,
                            int *error_number,
                            struct GetcpuTrace *trace)
{
    // 直接把 getcpu 的用户输出指针指向调用者提供的槽位，而不是经由一个
    // 编译器临时变量再复制。这样既符合 Linux ABI，也能严格验证内核对真实
    // 用户输出地址的 copy_out 是否可靠。
    unsigned int node = 0;
    *cpu_id = -1;
    const long result = raw_getcpu((unsigned int *)cpu_id, &node);
    if (trace != NULL)
    {
        trace->syscall_return = result;
        trace->raw_cpu = *cpu_id;
    }
    if (raw_syscall_failed(result))
    {
        *error_number = (int)-result;
        return -1;
    }
    if (result != 0)
    {
        *error_number = EIO;
        return -1;
    }
    return 0;
}

static int pin_current_worker(int cpu_id, int *error_number)
{
    uint64_t mask = 1ULL << (unsigned int)cpu_id;
    const long result = raw_syscall3(SYS_sched_setaffinity,
                                     0,
                                     (long)sizeof(mask),
                                     (long)&mask);
    if (raw_syscall_failed(result))
    {
        *error_number = (int)-result;
        return -1;
    }
    if (result != 0)
    {
        *error_number = EIO;
        return -1;
    }
    return 0;
}

static void scheduler_yield_once(void)
{
    (void)raw_syscall3(SYS_sched_yield, 0, 0, 0);
}

static uint32_t count_primes(uint32_t max_prime)
{
    uint32_t count = 0;
    for (uint32_t candidate = 2; candidate <= max_prime; ++candidate)
    {
        int prime = 1;
        for (uint32_t divisor = 2; divisor * divisor <= candidate; ++divisor)
        {
            if (candidate % divisor == 0)
            {
                prime = 0;
                break;
            }
        }
        count += (uint32_t)prime;
    }
    return count;
}

static void record_error(struct WorkerResult *result, int stage, int error_number)
{
    if (result->error_number == 0)
    {
        result->error_number = error_number;
        result->error_stage = stage;
    }
}

static void observe_cpu(struct WorkerResult *result, int error_stage)
{
    int cpu_id = -1;
    int error_number = 0;
    if (read_current_cpu(&cpu_id, &error_number, NULL) != 0)
    {
        record_error(result, error_stage, error_number);
        return;
    }
    if (cpu_id >= 0 && cpu_id < 64)
    {
        result->observed_cpu_mask |= 1ULL << (unsigned int)cpu_id;
    }
}

static void wait_for_start_gate(void)
{
    // 所有 worker 完成绑核和起始 getcpu 后一起释放，避免创建顺序影响 CPU
    // 利用率；使用原子门闩而非额外 IPC，从而保持测量集中于线程调度能力。
    atomic_fetch_add_explicit(&g_ready_workers, 1, memory_order_release);
    while (atomic_load_explicit(&g_start_gate, memory_order_acquire) == 0 &&
           atomic_load_explicit(&g_abort_gate, memory_order_acquire) == 0)
    {
        scheduler_yield_once();
    }
}

static void *worker_main(void *opaque)
{
    struct WorkerContext *context = (struct WorkerContext *)opaque;
    struct WorkerResult *result = &context->result;
    memset(result, 0, sizeof(*result));
    result->requested_cpu = context->requested_cpu;
    result->start_cpu = -1;
    result->end_cpu = -1;
    result->start_getcpu_return = -1;
    result->start_getcpu_raw_cpu = -1;
    result->end_getcpu_return = -1;
    result->end_getcpu_raw_cpu = -1;

    int error_number = 0;
    if (pin_current_worker(context->requested_cpu, &error_number) != 0)
    {
        record_error(result, k_error_set_affinity, error_number);
    }
    else
    {
        struct GetcpuTrace start_trace = {};
        if (read_current_cpu(&result->start_cpu, &error_number, &start_trace) != 0)
        {
            result->start_getcpu_return = start_trace.syscall_return;
            result->start_getcpu_raw_cpu = start_trace.raw_cpu;
            record_error(result, k_error_start_getcpu, error_number);
        }
        else
        {
            result->start_getcpu_return = start_trace.syscall_return;
            result->start_getcpu_raw_cpu = start_trace.raw_cpu;
            observe_cpu(result, k_error_sample_getcpu);
        }
    }

    wait_for_start_gate();
    if (atomic_load_explicit(&g_abort_gate, memory_order_acquire) != 0)
    {
        return NULL;
    }
    if (result->error_number != 0)
    {
        return NULL;
    }

    struct timespec start_time;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0)
    {
        record_error(result, k_error_start_clock, errno);
        return NULL;
    }
    const uint64_t start_ns = timespec_to_ns(&start_time);
    const uint64_t deadline_ns = start_ns + (uint64_t)context->seconds * 1000000000ULL;

    do
    {
        // 保持与 sysbench cpu 相同的“重复计算素数”负载语义。
        g_work_sink[context->requested_cpu] += count_primes(context->max_prime);
        ++result->events;
        if ((result->events & 15ULL) == 0)
        {
            observe_cpu(result, k_error_sample_getcpu);
        }
        if (result->error_number != 0 || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        {
            if (result->error_number == 0)
            {
                record_error(result, k_error_loop_clock, errno);
            }
            break;
        }
    } while (timespec_to_ns(&now) < deadline_ns);

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 && result->error_number == 0)
    {
        record_error(result, k_error_end_clock, errno);
    }

    if (result->error_number != 0)
    {
        return NULL;
    }

    const uint64_t end_ns = timespec_to_ns(&now);
    if (end_ns < start_ns)
    {
        record_error(result, k_error_end_clock, EIO);
        return NULL;
    }
    result->elapsed_ns = end_ns - start_ns;

    struct GetcpuTrace end_trace = {};
    if (read_current_cpu(&result->end_cpu, &error_number, &end_trace) != 0 && result->error_number == 0)
    {
        record_error(result, k_error_end_getcpu, error_number);
    }
    result->end_getcpu_return = end_trace.syscall_return;
    result->end_getcpu_raw_cpu = end_trace.raw_cpu;
    observe_cpu(result, k_error_end_getcpu);

    return NULL;
}

static int parse_positive_int(const char *value, int maximum, const char *name)
{
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (value[0] == '\0' || end == NULL || *end != '\0' || parsed <= 0 || parsed > maximum)
    {
        fprintf(stderr, "参数 %s 必须是 1 到 %d 的整数：%s\n", name, maximum, value);
        return -1;
    }
    return (int)parsed;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法：%s [--workers N] [--seconds N] [--max-prime N]\n"
            "示例：%s --workers 2 --seconds 3 --max-prime 2000\n",
            program,
            program);
}

static int worker_exit_code(const struct WorkerResult *result)
{
    const uint64_t requested_mask = 1ULL << (unsigned int)result->requested_cpu;
    int failure_bits = 0;
    if (result->error_number != 0)
    {
        return 0x80 | ((result->error_stage & 0x07) << 4) |
               (result->error_number & 0x0f);
    }
    if (result->start_cpu != result->requested_cpu)
    {
        failure_bits |= 2;
    }
    if (result->end_cpu != result->requested_cpu)
    {
        failure_bits |= 4;
    }
    if (result->observed_cpu_mask != requested_mask)
    {
        failure_bits |= 8;
    }
    return failure_bits == 0 ? 0 : (0x10 | failure_bits);
}

static const char *error_stage_name(int exit_code)
{
    if (exit_code < 0x80)
    {
        return "none";
    }

    switch ((exit_code >> 4) & 0x07)
    {
    case k_error_set_affinity:
        return "sched_setaffinity";
    case k_error_start_getcpu:
        return "getcpu_start";
    case k_error_sample_getcpu:
        return "getcpu_sample";
    case k_error_start_clock:
        return "clock_start";
    case k_error_loop_clock:
        return "clock_loop";
    case k_error_end_clock:
        return "clock_end";
    case k_error_end_getcpu:
        return "getcpu_end";
    default:
        return "unknown";
    }
}

int main(int argc, char **argv)
{
    int workers = k_default_workers;
    int seconds = k_default_seconds;
    int max_prime = k_default_max_prime;

    for (int index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--workers") == 0 && index + 1 < argc)
        {
            workers = parse_positive_int(argv[++index], k_max_workers, "--workers");
        }
        else if (strcmp(argv[index], "--seconds") == 0 && index + 1 < argc)
        {
            seconds = parse_positive_int(argv[++index], 120, "--seconds");
        }
        else if (strcmp(argv[index], "--max-prime") == 0 && index + 1 < argc)
        {
            max_prime = parse_positive_int(argv[++index], 50000, "--max-prime");
        }
        else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            print_usage(argv[0]);
            return 2;
        }

        if (workers < 0 || seconds < 0 || max_prime < 0)
        {
            return 2;
        }
    }

    pthread_t worker_threads[k_max_workers];
    struct WorkerContext contexts[k_max_workers];
    int created_workers = 0;
    int completed_workers = 0;
    int failed = 0;
    uint64_t total_events = 0;
    uint64_t maximum_elapsed_ns = 0;

    atomic_init(&g_ready_workers, 0);
    atomic_init(&g_start_gate, 0);
    atomic_init(&g_abort_gate, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("F7LY_SMP_CPU_BEGIN workers=%d seconds=%d max_prime=%d mode=pthread\n",
           workers,
           seconds,
           max_prime);
    printf("F7LY_SMP_CPU_CRITERIA status=PASS_requires_affinity_getcpu_start_end_and_all_samples\n");

    for (int worker = 0; worker < workers; ++worker)
    {
        memset(&contexts[worker], 0, sizeof(contexts[worker]));
        contexts[worker].requested_cpu = worker;
        contexts[worker].seconds = seconds;
        contexts[worker].max_prime = (uint32_t)max_prime;
        int create_result = pthread_create(&worker_threads[worker], NULL, worker_main, &contexts[worker]);
        if (create_result != 0)
        {
            fprintf(stderr, "pthread_create worker=%d 失败：%s\n", worker, strerror(create_result));
            failed = 1;
            break;
        }
        ++created_workers;
    }

    if (created_workers != workers)
    {
        atomic_store_explicit(&g_abort_gate, 1, memory_order_release);
        atomic_store_explicit(&g_start_gate, 1, memory_order_release);
    }
    else
    {
        // 等待每个线程完成绑核和首次 getcpu；主线程不占用固定核，主动让出
        // 时间片，使 workers 即使恰好覆盖所有 CPU 也能进入这个栅栏。
        while (atomic_load_explicit(&g_ready_workers, memory_order_acquire) < workers)
        {
            scheduler_yield_once();
        }
        atomic_store_explicit(&g_start_gate, 1, memory_order_release);
    }

    for (int worker = 0; worker < created_workers; ++worker)
    {
        int join_result = pthread_join(worker_threads[worker], NULL);
        if (join_result != 0)
        {
            fprintf(stderr, "pthread_join worker=%d 失败：%s\n", worker, strerror(join_result));
            failed = 1;
            continue;
        }
        ++completed_workers;
    }

    for (int worker = 0; worker < workers; ++worker)
    {
        const struct WorkerResult *result = &contexts[worker].result;
        const int exit_code = worker < created_workers ? worker_exit_code(result) : -1;
        const int worker_passed = exit_code == 0;
        const int worker_error = exit_code >= 0x80;
        const int affinity_failure_bits = exit_code < 0
                                              ? 0xff
                                              : (worker_error ? 0 : exit_code & 0x0f);
        const int errno_low4 = worker_error ? exit_code & 0x0f : 0;
        printf("F7LY_SMP_CPU_WORKER worker=%d requested_cpu=%d "
               "affinity_check=getcpu_start_end_and_samples start_cpu=%d end_cpu=%d "
               "start_getcpu_return=%ld start_getcpu_raw_cpu=%d "
               "end_getcpu_return=%ld end_getcpu_raw_cpu=%d "
               "observed_cpu_mask=0x%llx exit_code=%d error_stage=%s "
               "errno_low4=0x%x affinity_failure_bits=0x%x events=%llu "
               "elapsed_ns=%llu status=%s\n",
               worker,
               worker,
               result->start_cpu,
               result->end_cpu,
               result->start_getcpu_return,
               result->start_getcpu_raw_cpu,
               result->end_getcpu_return,
               result->end_getcpu_raw_cpu,
               (unsigned long long)result->observed_cpu_mask,
               exit_code,
               error_stage_name(exit_code),
               errno_low4,
               affinity_failure_bits,
               (unsigned long long)result->events,
               (unsigned long long)result->elapsed_ns,
               worker_passed ? "PASS" : "FAIL");
        if (!worker_passed)
        {
            failed = 1;
        }
        else
        {
            total_events += result->events;
            if (result->elapsed_ns > maximum_elapsed_ns)
            {
                maximum_elapsed_ns = result->elapsed_ns;
            }
        }
    }

    printf("F7LY_SMP_CPU_RESULT workers=%d completed_workers=%d mode=pthread\n",
           workers,
           completed_workers);
    if (maximum_elapsed_ns != 0)
    {
        // 以最后结束的 worker 作为整轮墙钟时间，避免把各线程耗时相加后
        // 错算成串行吞吐；这个定义与 sysbench CPU 的 events/sec 口径一致。
        const double elapsed_seconds = (double)maximum_elapsed_ns / 1000000000.0;
        const double events_per_second =
            (double)total_events / elapsed_seconds;
        printf("F7LY_SMP_CPU_METRICS workers=%d events=%llu "
               "elapsed_seconds=%.6f events_per_second=%.3f status=%s\n",
               workers,
               (unsigned long long)total_events,
               elapsed_seconds,
               events_per_second,
               failed ? "FAIL" : "PASS");
    }
    printf("F7LY_SMP_CPU_%s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
