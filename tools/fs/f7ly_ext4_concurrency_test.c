/*
 * F7LY ext4 并发文件操作专项。
 *
 * 用法示例：
 *   riscv64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
 *       tools/fs/f7ly_ext4_concurrency_test.c -o f7ly-ext4-concurrency-test
 *
 * 每个 worker 独占自己的临时文件名，但所有 worker 共享同一个目录，
 * 循环执行完整块写入、fsync、rename、读取校验和 unlink，用于覆盖 ext4
 * 挂载锁、bcache 和目录元数据并发路径。程序只在错误时输出详细信息，
 * 避免把串口输出本身变成测试热路径。
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum
{
    k_worker_count = 4,
    k_iterations = 16,
    k_payload_size = 16 * 1024,
};

static const char *const k_directory = "/f7ly-ext4-concurrency";
static atomic_int g_failed = 0;

struct worker_context
{
    int worker;
};

static int write_all(int fd, const uint8_t *buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length)
    {
        ssize_t written = write(fd, buffer + offset, length - offset);
        if (written <= 0)
            return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int read_and_verify(int fd, const uint8_t *expected, size_t length)
{
    uint8_t buffer[4096];
    size_t offset = 0;
    while (offset < length)
    {
        size_t request = length - offset;
        if (request > sizeof(buffer))
            request = sizeof(buffer);
        ssize_t received = read(fd, buffer, request);
        if (received <= 0 || memcmp(buffer, expected + offset, (size_t)received) != 0)
            return -1;
        offset += (size_t)received;
    }
    return 0;
}

/*
 * 以非块对齐的偏移和长度读取，覆盖 ext4 物理块 scratch 缓冲区的首尾复制
 * 路径。多个 worker 同时执行时可稳定暴露 I/O 完成后才复制共享缓冲区的竞态。
 */
static int pread_and_verify_unaligned(int fd, const uint8_t *expected, size_t length)
{
    uint8_t buffer[733];
    size_t offset = 1;
    while (offset < length)
    {
        size_t request = length - offset;
        if (request > sizeof(buffer))
            request = sizeof(buffer);
        ssize_t received = pread(fd, buffer, request, (off_t)offset);
        if (received <= 0 || memcmp(buffer, expected + offset, (size_t)received) != 0)
            return -1;
        offset += (size_t)received;
    }
    return 0;
}

static void report_failure(int worker, int iteration, const char *operation)
{
    int saved_errno = errno;
    fprintf(stderr, "F7LY_EXT4_CONCURRENCY_FAIL worker=%d iteration=%d operation=%s errno=%d (%s)\n",
            worker, iteration, operation, saved_errno, strerror(saved_errno));
    atomic_store_explicit(&g_failed, 1, memory_order_release);
}

static void *worker_main(void *argument)
{
    const struct worker_context *context = (const struct worker_context *)argument;
    uint8_t payload[k_payload_size];
    char temporary_path[128];
    char ready_path[128];

    for (size_t index = 0; index < sizeof(payload); ++index)
        payload[index] = (uint8_t)(context->worker * 31 + (int)(index % 251));

    for (int iteration = 0; iteration < k_iterations; ++iteration)
    {
        int temporary_length = snprintf(temporary_path, sizeof(temporary_path),
                                        "%s/w%d-%d.tmp", k_directory,
                                        context->worker, iteration);
        int ready_length = snprintf(ready_path, sizeof(ready_path),
                                    "%s/w%d-%d.ready", k_directory,
                                    context->worker, iteration);
        if (temporary_length < 0 || (size_t)temporary_length >= sizeof(temporary_path) ||
            ready_length < 0 || (size_t)ready_length >= sizeof(ready_path))
        {
            errno = ENAMETOOLONG;
            report_failure(context->worker, iteration, "snprintf");
            return NULL;
        }

        int fd = open(temporary_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (fd < 0)
        {
            report_failure(context->worker, iteration, "open");
            return NULL;
        }
        if (write_all(fd, payload, sizeof(payload)) != 0)
        {
            report_failure(context->worker, iteration, "write");
            close(fd);
            return NULL;
        }
        if (fsync(fd) != 0)
        {
            report_failure(context->worker, iteration, "fsync");
            close(fd);
            return NULL;
        }
        if (close(fd) != 0)
        {
            report_failure(context->worker, iteration, "close-write");
            return NULL;
        }
        if (rename(temporary_path, ready_path) != 0)
        {
            report_failure(context->worker, iteration, "rename");
            return NULL;
        }

        fd = open(ready_path, O_RDONLY);
        if (fd < 0)
        {
            report_failure(context->worker, iteration, "open-ready");
            return NULL;
        }
        if (read_and_verify(fd, payload, sizeof(payload)) != 0)
        {
            report_failure(context->worker, iteration, "read-verify");
            close(fd);
            return NULL;
        }
        if (pread_and_verify_unaligned(fd, payload, sizeof(payload)) != 0)
        {
            report_failure(context->worker, iteration, "pread-verify-unaligned");
            close(fd);
            return NULL;
        }
        if (close(fd) != 0)
        {
            report_failure(context->worker, iteration, "close-read");
            return NULL;
        }
        if (unlink(ready_path) != 0)
        {
            report_failure(context->worker, iteration, "unlink");
            return NULL;
        }
    }
    return NULL;
}

int main(void)
{
    if (mkdir(k_directory, 0755) != 0 && errno != EEXIST)
    {
        report_failure(-1, -1, "mkdir");
        return 1;
    }

    pthread_t threads[k_worker_count];
    struct worker_context contexts[k_worker_count];
    int created = 0;
    for (int worker = 0; worker < k_worker_count; ++worker)
    {
        contexts[worker].worker = worker;
        int result = pthread_create(&threads[worker], NULL, worker_main, &contexts[worker]);
        if (result != 0)
        {
            errno = result;
            report_failure(worker, -1, "pthread-create");
            break;
        }
        ++created;
    }
    for (int worker = 0; worker < created; ++worker)
        pthread_join(threads[worker], NULL);

    sync();
    if (atomic_load_explicit(&g_failed, memory_order_acquire) != 0 ||
        rmdir(k_directory) != 0)
    {
        if (errno != ENOENT)
            report_failure(-1, -1, "rmdir");
        return 1;
    }
    sync();
    printf("F7LY_EXT4_CONCURRENCY_PASS workers=%d iterations=%d payload=%d\n",
           k_worker_count, k_iterations, k_payload_size);
    return 0;
}
