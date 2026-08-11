#!/bin/sh
#
# 进程地址空间销毁窄测：子进程先触碰 256 MiB，和父进程握手后退出；
# 计时窗口只覆盖“通知退出 -> waitpid 返回”，不包含 mmap/缺页时间。
# 本脚本由 buildstorm_perf_probe.sh 写入临时评测镜像，不改原始镜像。

echo "#### OS COMP TEST GROUP START mm-teardown-perf ####"

mount -t proc proc /proc 2>/dev/null || true
export PATH=/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin

WORKDIR=/tmp/f7ly-mm-teardown
mkdir -p "$WORKDIR" || exit 1

cat >"$WORKDIR/bench.c" <<'EOF'
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ITERATIONS = 20 };
static const size_t k_mapping_bytes = 256UL * 1024UL * 1024UL;

static uint64_t monotonic_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static int u64_cmp(const void *lhs, const void *rhs) {
    const uint64_t a = *(const uint64_t *)lhs;
    const uint64_t b = *(const uint64_t *)rhs;
    return (a > b) - (a < b);
}

static void write_byte(int fd, char value) {
    while (write(fd, &value, 1) < 0) {
        if (errno != EINTR) {
            perror("write");
            _exit(3);
        }
    }
}

static void read_byte(int fd) {
    char value;
    ssize_t got;
    do {
        got = read(fd, &value, 1);
    } while (got < 0 && errno == EINTR);
    if (got != 1) {
        if (got < 0)
            perror("read");
        _exit(4);
    }
}

int main(void) {
    uint64_t samples[ITERATIONS];

    for (int iteration = 0; iteration < ITERATIONS; ++iteration) {
        int ready[2];
        int release[2];
        if (pipe(ready) != 0 || pipe(release) != 0) {
            perror("pipe");
            return 1;
        }

        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            return 1;
        }
        if (child == 0) {
            close(ready[0]);
            close(release[1]);
            volatile unsigned char *mapping = mmap(
                NULL, k_mapping_bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapping == MAP_FAILED) {
                perror("mmap");
                _exit(5);
            }
            for (size_t offset = 0; offset < k_mapping_bytes; offset += 4096)
                mapping[offset] = (unsigned char)(offset >> 12);

            write_byte(ready[1], 'R');
            read_byte(release[0]);
            _exit(0);
        }

        close(ready[1]);
        close(release[0]);
        read_byte(ready[0]);
        const uint64_t started_us = monotonic_us();
        write_byte(release[1], 'G');

        int status = 0;
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) {
                perror("waitpid");
                return 1;
            }
        }
        samples[iteration] = monotonic_us() - started_us;
        close(ready[0]);
        close(release[1]);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "child iteration %d failed: status=%d\n",
                    iteration, status);
            return 1;
        }
        printf("MM_TEARDOWN_SAMPLE iteration=%d exit_wait_us=%llu\n",
               iteration,
               (unsigned long long)samples[iteration]);
        fflush(stdout);
    }

    qsort(samples, ITERATIONS, sizeof(samples[0]), u64_cmp);
    printf("MM_TEARDOWN_RESULT ok=true iterations=%d bytes=%zu "
           "min_us=%llu median_us=%llu p95_us=%llu max_us=%llu\n",
           ITERATIONS, k_mapping_bytes,
           (unsigned long long)samples[0],
           (unsigned long long)samples[ITERATIONS / 2],
           (unsigned long long)samples[(ITERATIONS * 95 - 1) / 100],
           (unsigned long long)samples[ITERATIONS - 1]);
    return 0;
}
EOF

if ! command -v cc >/dev/null 2>&1; then
    echo "MM_TEARDOWN_RESULT ok=false reason=no-cc"
    echo "#### OS COMP TEST GROUP END mm-teardown-perf ####"
    exit 1
fi

if ! cc -O2 -Wall -Wextra "$WORKDIR/bench.c" -o "$WORKDIR/bench"; then
    echo "MM_TEARDOWN_RESULT ok=false reason=compile"
    echo "#### OS COMP TEST GROUP END mm-teardown-perf ####"
    exit 1
fi

if command -v f7ly-perf >/dev/null 2>&1; then
    f7ly-perf reset metrics 2>/dev/null || true
fi

"$WORKDIR/bench"
RC=$?

if command -v f7ly-perf >/dev/null 2>&1; then
    echo "MM_TEARDOWN_PERF_BEGIN"
    cat /proc/f7ly/perf/metrics 2>/dev/null
    cat /proc/f7ly/perf/syscalls 2>/dev/null
    echo "MM_TEARDOWN_PERF_END"
fi

echo "#### OS COMP TEST GROUP END mm-teardown-perf ####"
sync
exit "$RC"
