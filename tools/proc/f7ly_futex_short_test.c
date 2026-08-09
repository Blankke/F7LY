/*
 * F7LY futex/线程退出短测。
 *
 * 编译示例：
 *   riscv64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
 *       tools/proc/f7ly_futex_short_test.c -o f7ly-futex-short-test-rv
 *
 * 覆盖六条短路径：匹配 wait/wake、相对超时、信号中断、普通 pthread join、
 * fork/COW 后的 clear_tid 唤醒，以及 CMP_REQUEUE(wake=0,requeue=N)。后两项
 * 分别防止私有 futex 按瞬时物理页匹配、以及合法 requeue 被当成 no-op。测试
 * 只输出最终 PASS 或第一个失败点，避免串口输出成为同步路径的额外负载。
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE 128
#endif
#ifndef FUTEX_WAKE_PRIVATE
#define FUTEX_WAKE_PRIVATE 129
#endif
#ifndef FUTEX_CMP_REQUEUE_PRIVATE
#define FUTEX_CMP_REQUEUE_PRIVATE 132
#endif

static _Atomic int g_wait_word;
static _Atomic int g_wait_ready;
static _Atomic int g_failure;
static _Atomic int g_cow_gate;
static _Atomic int g_cow_worker_ready;
static _Atomic int g_cow_join_started;
static _Atomic int g_requeue_source;
static _Atomic int g_requeue_target;
static _Atomic int g_requeue_ready;

static int futex_wait_private(_Atomic int *word, int expected,
                              const struct timespec *timeout)
{
    return (int)syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected,
                        timeout, NULL, 0);
}

static int futex_wake_private(_Atomic int *word, int count)
{
    return (int)syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, count,
                        NULL, NULL, 0);
}

static int futex_cmp_requeue_private(_Atomic int *source,
                                     int wake_count,
                                     int requeue_count,
                                     _Atomic int *target,
                                     int expected)
{
    return (int)syscall(SYS_futex, source, FUTEX_CMP_REQUEUE_PRIVATE,
                        wake_count, requeue_count, target, expected);
}

static void fail_test(const char *name)
{
    int saved_errno = errno;
    fprintf(stderr, "F7LY_FUTEX_SHORT_FAIL test=%s errno=%d (%s)\n",
            name, saved_errno, strerror(saved_errno));
    atomic_store_explicit(&g_failure, 1, memory_order_release);
    exit(1);
}

static void join_with_timeout(pthread_t thread, const char *name)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        fail_test("join-clock");
    deadline.tv_sec += 3;
    int join_result = pthread_timedjoin_np(thread, NULL, &deadline);
    if (join_result != 0)
    {
        errno = join_result;
        fail_test(name);
    }
}

static void *wait_wake_worker(void *unused)
{
    (void)unused;
    atomic_store_explicit(&g_wait_ready, 1, memory_order_release);
    while (atomic_load_explicit(&g_wait_word, memory_order_acquire) == 0)
    {
        if (futex_wait_private(&g_wait_word, 0, NULL) < 0 && errno != EINTR)
            fail_test("wait-wake-worker");
    }
    return NULL;
}

static void test_wait_wake(void)
{
    pthread_t thread;
    atomic_store_explicit(&g_wait_word, 0, memory_order_release);
    atomic_store_explicit(&g_wait_ready, 0, memory_order_release);
    if (pthread_create(&thread, NULL, wait_wake_worker, NULL) != 0)
        fail_test("wait-wake-create");

    while (atomic_load_explicit(&g_wait_ready, memory_order_acquire) == 0)
        sched_yield();
    usleep(10000);
    atomic_store_explicit(&g_wait_word, 1, memory_order_release);
    if (futex_wake_private(&g_wait_word, 1) < 0)
        fail_test("wait-wake-wake");
    if (pthread_join(thread, NULL) != 0)
        fail_test("wait-wake-join");
}

static void test_timeout(void)
{
    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 50000000L};
    atomic_store_explicit(&g_wait_word, 0, memory_order_release);
    errno = 0;
    if (futex_wait_private(&g_wait_word, 0, &timeout) != -1 ||
        errno != ETIMEDOUT)
        fail_test("timeout");
}

static void signal_handler(int signal_number)
{
    (void)signal_number;
}

static void *signal_wait_worker(void *unused)
{
    (void)unused;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0)
        fail_test("signal-handler");

    atomic_store_explicit(&g_wait_ready, 1, memory_order_release);
    struct timespec timeout = {.tv_sec = 2, .tv_nsec = 0};
    errno = 0;
    if (futex_wait_private(&g_wait_word, 0, &timeout) != -1 ||
        errno != EINTR)
        fail_test("signal-wait");
    return NULL;
}

static void test_signal_interrupt(void)
{
    pthread_t thread;
    atomic_store_explicit(&g_wait_word, 0, memory_order_release);
    atomic_store_explicit(&g_wait_ready, 0, memory_order_release);
    if (pthread_create(&thread, NULL, signal_wait_worker, NULL) != 0)
        fail_test("signal-create");
    while (atomic_load_explicit(&g_wait_ready, memory_order_acquire) == 0)
        sched_yield();
    usleep(10000);
    if (pthread_kill(thread, SIGUSR1) != 0)
        fail_test("signal-send");
    if (pthread_join(thread, NULL) != 0)
        fail_test("signal-join");
}

static void *exit_worker(void *argument)
{
    int value = *(int *)argument;
    return (void *)(long)(value + 1);
}

static void test_thread_exit_join(void)
{
    for (int index = 0; index < 4; ++index)
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, exit_worker, &index) != 0)
            fail_test("exit-create");
        void *result = NULL;
        if (pthread_join(thread, &result) != 0 ||
            result != (void *)(long)(index + 1))
            fail_test("exit-join");
    }
}

/*
 * fork 会把父地址空间的可写私有页降级为 COW。worker 的 clear_child_tid
 * 位于这样的私有页：main 已经在该地址 FUTEX_WAIT 后，worker 退出时写 0
 * 会拆 COW 页。若 key 使用“当前物理页”，WAKE 将从新页 P1 查找，而 joiner
 * 仍登记在旧页 P0，最终只能等到超时；稳定的 (mm,uaddr) 私有 key 应通过。
 */
static void *cow_exit_worker(void *unused)
{
    (void)unused;
    atomic_store_explicit(&g_cow_worker_ready, 1, memory_order_release);
    while (atomic_load_explicit(&g_cow_gate, memory_order_acquire) == 0)
    {
        if (futex_wait_private(&g_cow_gate, 0, NULL) < 0 && errno != EINTR)
            fail_test("cow-worker-wait");
    }
    return NULL;
}

static void *cow_release_worker(void *unused)
{
    (void)unused;
    while (atomic_load_explicit(&g_cow_join_started, memory_order_acquire) == 0)
        sched_yield();

    /* 确保 main 已进入 pthread_join 的 clear_tid FUTEX_WAIT。 */
    usleep(50000);
    atomic_store_explicit(&g_cow_gate, 1, memory_order_release);
    if (futex_wake_private(&g_cow_gate, 1) < 0)
        fail_test("cow-worker-release");
    return NULL;
}

static void test_private_cow_clear_tid(void)
{
    pthread_t worker;
    pthread_t releaser;
    atomic_store_explicit(&g_cow_gate, 0, memory_order_release);
    atomic_store_explicit(&g_cow_worker_ready, 0, memory_order_release);
    atomic_store_explicit(&g_cow_join_started, 0, memory_order_release);
    if (pthread_create(&worker, NULL, cow_exit_worker, NULL) != 0)
        fail_test("cow-worker-create");
    while (atomic_load_explicit(&g_cow_worker_ready, memory_order_acquire) == 0)
        sched_yield();

    pid_t child = fork();
    if (child < 0)
        fail_test("cow-fork");
    if (child == 0)
        _exit(0);
    if (waitpid(child, NULL, 0) != child)
        fail_test("cow-waitpid");

    if (pthread_create(&releaser, NULL, cow_release_worker, NULL) != 0)
        fail_test("cow-releaser-create");
    atomic_store_explicit(&g_cow_join_started, 1, memory_order_release);
    join_with_timeout(worker, "cow-clear-tid-join");
    join_with_timeout(releaser, "cow-releaser-join");
}

static void *requeue_wait_worker(void *unused)
{
    (void)unused;
    atomic_fetch_add_explicit(&g_requeue_ready, 1, memory_order_release);
    struct timespec timeout = {.tv_sec = 3, .tv_nsec = 0};
    if (futex_wait_private(&g_requeue_source, 0, &timeout) != 0)
        fail_test("cmp-requeue-worker-wait");
    return NULL;
}

static void test_cmp_requeue_zero_wake(void)
{
    pthread_t first;
    pthread_t second;
    atomic_store_explicit(&g_requeue_source, 0, memory_order_release);
    atomic_store_explicit(&g_requeue_target, 0, memory_order_release);
    atomic_store_explicit(&g_requeue_ready, 0, memory_order_release);
    if (pthread_create(&first, NULL, requeue_wait_worker, NULL) != 0 ||
        pthread_create(&second, NULL, requeue_wait_worker, NULL) != 0)
        fail_test("cmp-requeue-create");
    while (atomic_load_explicit(&g_requeue_ready, memory_order_acquire) != 2)
        sched_yield();

    /* ready 在 syscall 前发布；在 QEMU 上留出一个调度片使两个 waiter 都入桶。 */
    usleep(50000);
    if (futex_cmp_requeue_private(&g_requeue_source, 0, 2,
                                  &g_requeue_target, 0) != 2)
        fail_test("cmp-requeue-move");
    if (futex_wake_private(&g_requeue_target, 2) != 2)
        fail_test("cmp-requeue-target-wake");
    join_with_timeout(first, "cmp-requeue-first-join");
    join_with_timeout(second, "cmp-requeue-second-join");
}

int main(void)
{
    atomic_store_explicit(&g_failure, 0, memory_order_release);
    test_wait_wake();
    test_timeout();
    test_signal_interrupt();
    test_thread_exit_join();
    test_private_cow_clear_tid();
    test_cmp_requeue_zero_wake();
    printf("F7LY_FUTEX_SHORT_PASS wait_wake=1 timeout=1 signal=1 thread_exit=1 "
           "private_cow_clear_tid=1 cmp_requeue_zero_wake=1\n");
    return atomic_load_explicit(&g_failure, memory_order_acquire) == 0 ? 0 : 1;
}
