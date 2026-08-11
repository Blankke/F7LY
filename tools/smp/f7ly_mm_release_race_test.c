/*
 * ProcessMemoryManager 最后引用并发归还回归。
 *
 * 每轮 fork 一个独立地址空间，在其中创建多个 pthread，
 * 然后让所有任务通过原始 SYS_exit 同时退出。这会把 mm 的
 * 倒数第二个和最后一个引用归还压到不同 CPU 上，覆盖
 * “非最终清理者误删正在销毁的 mm”竞态。
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    k_default_rounds = 24,
    k_thread_count = 8,
    k_mapping_bytes = 16 * 1024 * 1024,
};

struct exit_context {
    _Atomic int ready;
    _Atomic int release;
};

__attribute__((noreturn)) static void raw_thread_exit(int status)
{
    (void)syscall(SYS_exit, status);
    __builtin_unreachable();
}

static void *exit_worker(void *opaque)
{
    struct exit_context *context = opaque;
    atomic_fetch_add_explicit(&context->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&context->release, memory_order_acquire)) {
        sched_yield();
    }
    raw_thread_exit(0);
}

static int run_child(void)
{
    volatile unsigned char *mapping = mmap(NULL, k_mapping_bytes,
                                            PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS,
                                            -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "F7LY_MM_RELEASE_RACE_FAIL mmap errno=%d (%s)\n",
                errno, strerror(errno));
        return 20;
    }
    for (size_t offset = 0; offset < k_mapping_bytes; offset += 4096) {
        mapping[offset] = (unsigned char)(offset >> 12);
    }

    struct exit_context context = {0};
    pthread_t workers[k_thread_count - 1];
    for (int index = 0; index < k_thread_count - 1; ++index) {
        int rc = pthread_create(&workers[index], NULL, exit_worker, &context);
        if (rc != 0) {
            fprintf(stderr,
                    "F7LY_MM_RELEASE_RACE_FAIL pthread_create index=%d rc=%d\n",
                    index, rc);
            return 21;
        }
    }

    while (atomic_load_explicit(&context.ready, memory_order_acquire) !=
           k_thread_count - 1) {
        sched_yield();
    }
    atomic_store_explicit(&context.release, 1, memory_order_release);
    raw_thread_exit(0);
}

int main(int argc, char **argv)
{
    int rounds = k_default_rounds;
    if (argc == 3 && strcmp(argv[1], "--rounds") == 0) {
        rounds = atoi(argv[2]);
    }
    if (rounds <= 0) {
        fprintf(stderr, "F7LY_MM_RELEASE_RACE_FAIL invalid rounds\n");
        return 2;
    }

    for (int round = 0; round < rounds; ++round) {
        pid_t child = fork();
        if (child < 0) {
            fprintf(stderr, "F7LY_MM_RELEASE_RACE_FAIL fork round=%d errno=%d\n",
                    round, errno);
            return 3;
        }
        if (child == 0) {
            int rc = run_child();
            _exit(rc);
        }

        int status = 0;
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) {
                fprintf(stderr,
                        "F7LY_MM_RELEASE_RACE_FAIL waitpid round=%d errno=%d\n",
                        round, errno);
                return 4;
            }
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr,
                    "F7LY_MM_RELEASE_RACE_FAIL child round=%d status=%d\n",
                    round, status);
            return 5;
        }
    }

    /* 给最后一轮的非 leader 线程留出延后回收时间。 */
    usleep(100000);
    printf("F7LY_MM_RELEASE_RACE_PASS rounds=%d threads=%d mapping_bytes=%d\n",
           rounds, k_thread_count, k_mapping_bytes);
    return 0;
}
