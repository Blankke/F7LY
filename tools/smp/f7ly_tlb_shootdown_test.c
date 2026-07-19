/*
 * F7LY 跨 CPU TLB shootdown 用户态专项。
 *
 * 编译示例：
 *   riscv64-linux-gnu-gcc -O2 -static -pthread f7ly_tlb_shootdown_test.c -o tlb-test-rv
 *   loongarch64-linux-gnu-gcc -O2 -static -pthread f7ly_tlb_shootdown_test.c -o tlb-test-la
 *
 * CPU1 先写共享页，使本地 TLB 缓存“可写”权限；CPU0 随后把同一页降为只读。
 * CPU1 的下一次写入必须收到 SIGSEGV。随后还会撤销映射，让 CPU1 再次
 * 触发 SIGSEGV，再由 CPU0 在同一虚拟地址建立全新匿名页并验证新翻译。
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>

enum test_phase {
    PHASE_PRIME = 0,
    PHASE_EXPECT_FAULT = 1,
    PHASE_FAULT_OBSERVED = 2,
    PHASE_EXPECT_WRITE = 3,
    PHASE_WRITE_OBSERVED = 4,
    PHASE_EXPECT_UNMAPPED_FAULT = 5,
    PHASE_UNMAPPED_FAULT_OBSERVED = 6,
    PHASE_EXPECT_REMAP = 7,
    PHASE_REMAP_OBSERVED = 8,
    PHASE_NEXT = 9,
    PHASE_STOP = 10,
};

static _Atomic int g_phase = PHASE_STOP;
static _Atomic int g_failures = 0;
static _Atomic int g_faults = 0;
static _Atomic int g_worker_ready = 0;
static volatile uint64_t *g_page;
static int g_rounds = 200;

static _Thread_local sigjmp_buf g_fault_env;
static _Thread_local volatile sig_atomic_t g_expect_fault;

static void fail(const char *message)
{
    fprintf(stderr, "F7LY_TLB_SHOOTDOWN_FAIL %s errno=%d (%s)\n",
            message, errno, strerror(errno));
    exit(1);
}

static void pin_current_thread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0)
    {
        errno = rc;
        fail("pthread_setaffinity_np");
    }
}

static void segv_handler(int signal_number, siginfo_t *info, void *context)
{
    (void)signal_number;
    (void)info;
    (void)context;
    if (!g_expect_fault)
    {
        static const char message[] =
            "F7LY_TLB_SHOOTDOWN_FAIL unexpected SIGSEGV\n";
        ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)written;
        _exit(2);
    }
    g_expect_fault = 0;
    atomic_fetch_add_explicit(&g_faults, 1, memory_order_relaxed);
    siglongjmp(g_fault_env, 1);
}

static void wait_for_phase(int expected)
{
    while (atomic_load_explicit(&g_phase, memory_order_acquire) != expected)
    {
        sched_yield();
    }
}

static void *worker_main(void *argument)
{
    (void)argument;
    pin_current_thread(1);

    for (int round = 0; round < g_rounds; ++round)
    {
        // 先在 CPU1 写一次，把可写页表项真实装入本地 TLB。
        g_page[0] = 0x100000000ULL + (uint64_t)round;
        atomic_store_explicit(&g_phase, PHASE_PRIME, memory_order_release);
        if (round == 0)
        {
            atomic_store_explicit(&g_worker_ready, 1, memory_order_release);
        }
        wait_for_phase(PHASE_EXPECT_FAULT);

        g_expect_fault = 1;
        if (sigsetjmp(g_fault_env, 1) == 0)
        {
            g_page[0] = 0xdead0000ULL + (uint64_t)round;
            g_expect_fault = 0;
            atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&g_phase, PHASE_FAULT_OBSERVED, memory_order_release);

        wait_for_phase(PHASE_EXPECT_WRITE);
        g_page[0] = 0x200000000ULL + (uint64_t)round;
        atomic_store_explicit(&g_phase, PHASE_WRITE_OBSERVED, memory_order_release);

        wait_for_phase(PHASE_EXPECT_UNMAPPED_FAULT);
        g_expect_fault = 1;
        if (sigsetjmp(g_fault_env, 1) == 0)
        {
            // munmap 已同步返回后，这次写入绝不能继续命中旧物理页。
            g_page[0] = 0xbeef0000ULL + (uint64_t)round;
            g_expect_fault = 0;
            atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&g_phase, PHASE_UNMAPPED_FAULT_OBSERVED,
                              memory_order_release);

        wait_for_phase(PHASE_EXPECT_REMAP);
        g_page[0] = 0x400000000ULL + (uint64_t)round;
        atomic_store_explicit(&g_phase, PHASE_REMAP_OBSERVED, memory_order_release);
        if (round + 1 < g_rounds)
        {
            wait_for_phase(PHASE_NEXT);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--rounds") == 0)
    {
        g_rounds = atoi(argv[2]);
    }
    if (g_rounds <= 0)
    {
        fail("invalid rounds");
    }
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2)
    {
        fail("requires at least two online CPUs");
    }

    struct sysinfo memory_info;
    if (sysinfo(&memory_info) != 0 || memory_info.mem_unit == 0)
    {
        fail("sysinfo");
    }
    unsigned long long total_bytes =
        (unsigned long long)memory_info.totalram * memory_info.mem_unit;
    unsigned long long managed_pages =
        (unsigned long long)memory_info.totalram * memory_info.mem_unit /
        (unsigned long long)sysconf(_SC_PAGESIZE);
    printf("F7LY_TLB_SHOOTDOWN_MEMORY total_bytes=%llu managed_pages=%llu\n",
           total_bytes, managed_pages);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = segv_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) != 0)
    {
        fail("sigaction");
    }

    g_page = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE),
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_page == MAP_FAILED)
    {
        fail("mmap");
    }

    pin_current_thread(0);
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    pthread_t worker;
    int rc = pthread_create(&worker, NULL, worker_main, NULL);
    if (rc != 0)
    {
        errno = rc;
        fail("pthread_create");
    }
    while (!atomic_load_explicit(&g_worker_ready, memory_order_acquire))
    {
        sched_yield();
    }

    for (int round = 0; round < g_rounds; ++round)
    {
        wait_for_phase(PHASE_PRIME);
        if (mprotect((void *)g_page, (size_t)sysconf(_SC_PAGESIZE), PROT_READ) != 0)
        {
            fail("mprotect read-only");
        }
        atomic_store_explicit(&g_phase, PHASE_EXPECT_FAULT, memory_order_release);
        wait_for_phase(PHASE_FAULT_OBSERVED);

        if (mprotect((void *)g_page, (size_t)sysconf(_SC_PAGESIZE),
                     PROT_READ | PROT_WRITE) != 0)
        {
            fail("mprotect read-write");
        }
        atomic_store_explicit(&g_phase, PHASE_EXPECT_WRITE, memory_order_release);
        wait_for_phase(PHASE_WRITE_OBSERVED);

        if (g_page[0] != 0x200000000ULL + (uint64_t)round)
        {
            atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);
        }

        void *fixed_address = (void *)g_page;
        if (munmap(fixed_address, page_size) != 0)
        {
            fail("munmap shared page");
        }
        atomic_store_explicit(&g_phase, PHASE_EXPECT_UNMAPPED_FAULT,
                              memory_order_release);
        wait_for_phase(PHASE_UNMAPPED_FAULT_OBSERVED);

        void *remapped = mmap(fixed_address, page_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                              -1, 0);
        if (remapped == MAP_FAILED || remapped != fixed_address)
        {
            fail("mmap MAP_FIXED remap");
        }
        g_page = (volatile uint64_t *)remapped;
        g_page[0] = 0x300000000ULL + (uint64_t)round;
        atomic_store_explicit(&g_phase, PHASE_EXPECT_REMAP, memory_order_release);
        wait_for_phase(PHASE_REMAP_OBSERVED);
        if (g_page[0] != 0x400000000ULL + (uint64_t)round)
        {
            atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);
        }
        if (round + 1 < g_rounds)
        {
            atomic_store_explicit(&g_phase, PHASE_NEXT, memory_order_release);
        }
    }

    atomic_store_explicit(&g_phase, PHASE_STOP, memory_order_release);
    rc = pthread_join(worker, NULL);
    if (rc != 0)
    {
        errno = rc;
        fail("pthread_join");
    }

    int failures = atomic_load_explicit(&g_failures, memory_order_relaxed);
    int faults = atomic_load_explicit(&g_faults, memory_order_relaxed);
    if (failures != 0 || faults != g_rounds * 2)
    {
        fprintf(stderr,
                "F7LY_TLB_SHOOTDOWN_FAIL rounds=%d faults=%d failures=%d\n",
                g_rounds, faults, failures);
        return 1;
    }

    if (munmap((void *)g_page, page_size) != 0)
    {
        fail("final munmap");
    }
    printf("F7LY_TLB_SHOOTDOWN_PASS rounds=%d faults=%d mprotect=%d munmap=%d\n",
           g_rounds, faults, g_rounds, g_rounds);
    return 0;
}
