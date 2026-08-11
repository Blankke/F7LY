#!/bin/sh
#
# 全局 clean file page cache 窄测：8 个进程并发遍历同一大型共享库，
# 随后在同一 boot 内再跑一轮，验证跨 FileVmObject 的复用和底层读降幅。

echo "#### OS COMP TEST GROUP START file-page-cache-perf ####"

mount -t proc proc /proc 2>/dev/null || true
export PATH=/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin

case "$(uname -m 2>/dev/null)" in
    loongarch64) SOURCE_TEST_FILE=/usr/lib/loongarch64-linux-gnu/libLLVM.so.19.1 ;;
    riscv64) SOURCE_TEST_FILE=/usr/lib/riscv64-linux-gnu/libLLVM.so.19.1 ;;
    *) SOURCE_TEST_FILE= ;;
esac

if [ -z "$SOURCE_TEST_FILE" ] || [ ! -f "$SOURCE_TEST_FILE" ]; then
    echo "FILE_CACHE_RESULT ok=false reason=missing-test-file path=$SOURCE_TEST_FILE"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi

WORKDIR=/tmp/f7ly-file-cache
mkdir -p "$WORKDIR" || exit 1
cat >"$WORKDIR/bench.c" <<'EOF'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WORKERS = 8 };

static int write_pattern_file(int fd, size_t length, unsigned char value) {
    unsigned char page[4096];
    memset(page, value, sizeof(page));
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > sizeof(page))
            chunk = sizeof(page);
        ssize_t written = pwrite(fd, page, chunk, (off_t)offset);
        if (written != (ssize_t)chunk)
            return -1;
        offset += chunk;
    }
    return 0;
}

static int truncate_resident_sigbus_test(void) {
    const char *path = "/tmp/f7ly-file-cache-truncate.bin";
    pid_t child = fork();
    if (child < 0) {
        perror("fork truncate");
        return 1;
    }
    if (child == 0) {
        int fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0 || ftruncate(fd, 8192) != 0)
            _exit(10);
        const unsigned char marker = 0x5a;
        if (pwrite(fd, &marker, 1, 4096) != 1)
            _exit(11);
        volatile const unsigned char *mapping = mmap(
            NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED)
            _exit(12);
        volatile unsigned char before = mapping[4096];
        (void)before;
        if (ftruncate(fd, 4096) != 0)
            _exit(13);
        // 第二页已经在 truncate 前驻留；正确实现仍必须让这次访问触发 SIGBUS。
        volatile unsigned char after = mapping[4096];
        (void)after;
        _exit(42);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    unlink(path);
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS) {
        printf("FILE_CACHE_TRUNCATE resident_sigbus=true\n");
        return 0;
    }
    printf("FILE_CACHE_TRUNCATE resident_sigbus=false status=%d exited=%d code=%d signaled=%d signal=%d\n",
           status,
           WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    return 1;
}

static int truncate_partial_eof_zero_test(void) {
    const char *path = "/tmp/f7ly-file-cache-partial-eof.bin";
    pid_t child = fork();
    if (child < 0) {
        perror("fork partial eof");
        return 1;
    }
    if (child == 0) {
        int fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0 || write_pattern_file(fd, 8192, 0xa5) != 0)
            _exit(20);
        volatile const unsigned char *mapping = mmap(
            NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED)
            _exit(21);
        // 先把 EOF 所在页驻留，确保测试的不是单纯 nonresident fault。
        if (mapping[4096 + 200] != 0xa5)
            _exit(22);
        if (ftruncate(fd, 4096 + 123) != 0)
            _exit(23);
        // truncate 后边界页必须重读：有效前缀保留，精确从新 EOF 起清零。
        if (mapping[4096 + 122] != 0xa5)
            _exit(24);
        if (mapping[4096 + 123] != 0 || mapping[4096 + 200] != 0)
            _exit(25);
        _exit(0);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    unlink(path);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("FILE_CACHE_TRUNCATE partial_eof_zero=true\n");
        return 0;
    }
    printf("FILE_CACHE_TRUNCATE partial_eof_zero=false status=%d exited=%d code=%d signaled=%d signal=%d\n",
           status,
           WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    return 1;
}

static _Thread_local sigjmp_buf fault_bus_env;
static _Thread_local volatile sig_atomic_t fault_bus_armed;
static volatile int race_done;
static volatile int race_truncate_error;
static volatile unsigned long race_reads;
static volatile unsigned long race_sigbus;

static void fault_bus_handler(int signo) {
    (void)signo;
    if (fault_bus_armed)
        siglongjmp(fault_bus_env, 1);
    _exit(91);
}

static void race_alarm_handler(int signo) {
    (void)signo;
    _exit(92);
}

struct race_context {
    int fd;
    volatile const unsigned char *mapping;
};

static void *truncate_race_writer(void *opaque) {
    struct race_context *context = opaque;
    for (int iteration = 0; iteration < 128; ++iteration) {
        if (ftruncate(context->fd, 4096) != 0 ||
            ftruncate(context->fd, 8192) != 0) {
            race_truncate_error = errno ? errno : EIO;
            break;
        }
        sched_yield();
    }
    __atomic_store_n(&race_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *truncate_race_fault(void *opaque) {
    struct race_context *context = opaque;
    unsigned long attempts = 0;
    while (!__atomic_load_n(&race_done, __ATOMIC_ACQUIRE) || attempts < 256) {
        if (sigsetjmp(fault_bus_env, 1) == 0) {
            fault_bus_armed = 1;
            volatile unsigned char value = context->mapping[4096];
            (void)value;
            fault_bus_armed = 0;
            ++race_reads;
        } else {
            fault_bus_armed = 0;
            ++race_sigbus;
        }
        ++attempts;
        sched_yield();
    }
    return NULL;
}

static int truncate_fault_race_test(void) {
    const char *path = "/tmp/f7ly-file-cache-race.bin";
    pid_t child = fork();
    if (child < 0) {
        perror("fork truncate race");
        return 1;
    }
    if (child == 0) {
        alarm(12);
        signal(SIGALRM, race_alarm_handler);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = fault_bus_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_NODEFER;
        if (sigaction(SIGBUS, &action, NULL) != 0)
            _exit(30);

        int fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0 || write_pattern_file(fd, 8192, 0x3c) != 0)
            _exit(31);
        volatile const unsigned char *mapping = mmap(
            NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED)
            _exit(32);
        volatile unsigned char resident = mapping[4096];
        (void)resident;

        struct race_context context = { fd, mapping };
        pthread_t writer;
        pthread_t faulter;
        if (pthread_create(&writer, NULL, truncate_race_writer, &context) != 0 ||
            pthread_create(&faulter, NULL, truncate_race_fault, &context) != 0)
            _exit(33);
        if (pthread_join(writer, NULL) != 0 || pthread_join(faulter, NULL) != 0)
            _exit(34);
        alarm(0);
        if (race_truncate_error != 0 || race_reads + race_sigbus == 0)
            _exit(35);
        _exit(0);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    unlink(path);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("FILE_CACHE_TRUNCATE fault_race_bounded=true\n");
        return 0;
    }
    printf("FILE_CACHE_TRUNCATE fault_race_bounded=false status=%d exited=%d code=%d signaled=%d signal=%d\n",
           status,
           WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    return 1;
}

static int truncate_correctness_tests(void) {
    int failed = 0;
    failed |= truncate_resident_sigbus_test();
    failed |= truncate_partial_eof_zero_test();
    failed |= truncate_fault_race_test();
    return failed ? 1 : 0;
}

static int touch_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 2;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        perror("fstat");
        close(fd);
        return 3;
    }
    const size_t length = (size_t)st.st_size;
    const volatile unsigned char *mapping = mmap(
        NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        return 4;
    }

    uint64_t checksum = 0;
    for (size_t offset = 0; offset < length; offset += 4096)
        checksum += mapping[offset];
    checksum += mapping[length - 1];
    if (munmap((void *)mapping, length) != 0) {
        perror("munmap");
        return 5;
    }
    return checksum == UINT64_MAX ? 6 : 0;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 64;
    if (strcmp(argv[1], "--truncate") == 0)
        return truncate_correctness_tests();

    pid_t children[WORKERS];
    for (int i = 0; i < WORKERS; ++i) {
        children[i] = fork();
        if (children[i] < 0) {
            perror("fork");
            return 1;
        }
        if (children[i] == 0)
            _exit(touch_file(argv[1]));
    }

    int failed = 0;
    for (int i = 0; i < WORKERS; ++i) {
        int status = 0;
        while (waitpid(children[i], &status, 0) < 0 && errno == EINTR) {
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            failed = 1;
    }
    printf("FILE_CACHE_ROUND workers=%d ok=%s\n", WORKERS,
           failed ? "false" : "true");
    return failed;
}
EOF

if ! cc -O2 -Wall -Wextra -pthread "$WORKDIR/bench.c" -o "$WORKDIR/bench"; then
    echo "FILE_CACHE_RESULT ok=false reason=compile"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi

if [ ! -e /proc/f7ly/perf ]; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=false reason=no-perf"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi

# cc/clang 自身可能加载 libLLVM。编译 helper 后才复制到新 inode，确保被测
# cache key 从未被编译器 mmap；普通 cp 不会预先填充 FileVmObject clean cache。
TEST_FILE="$WORKDIR/cache-target-$$.bin"
rm -f "$TEST_FILE"
if ! cp "$SOURCE_TEST_FILE" "$TEST_FILE"; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=copy-test-file source=$SOURCE_TEST_FILE"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi
sync

snapshot_perf() {
    round=$1
    output=$2
    cat /proc/f7ly/perf >"$output" || return 1
    echo "FILE_CACHE_PERF_BEGIN round=$round"
    cat "$output"
    echo "FILE_CACHE_PERF_END round=$round"
}

reset_perf() {
    echo reset >/proc/f7ly/perf 2>/dev/null
}

if ! reset_perf; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=reset-before-truncate"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi
"$WORKDIR/bench" --truncate
TRUNCATE_RC=$?

if ! reset_perf; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=reset-round1"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi
if ! "$WORKDIR/bench" "$TEST_FILE"; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=round1"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi
if ! snapshot_perf 1 "$WORKDIR/perf-round1"; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=snapshot-round1"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi

if ! reset_perf; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=reset-round2"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi
if ! "$WORKDIR/bench" "$TEST_FILE"; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=round2"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi
if ! snapshot_perf 2 "$WORKDIR/perf-round2"; then
    echo "FILE_CACHE_RESULT ok=false diagnostic=true reason=snapshot-round2"
    echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
    exit 1
fi

HITS1=$(awk '$1 == "file_cache.hits" { print $2 }' "$WORKDIR/perf-round1")
MISSES1=$(awk '$1 == "file_cache.misses" { print $2 }' "$WORKDIR/perf-round1")
HITS2=$(awk '$1 == "file_cache.hits" { print $2 }' "$WORKDIR/perf-round2")
MISSES2=$(awk '$1 == "file_cache.misses" { print $2 }' "$WORKDIR/perf-round2")
READ1=$(awk '$1 == "ext4.read_bytes" { print $2 }' "$WORKDIR/perf-round1")
READ2=$(awk '$1 == "ext4.read_bytes" { print $2 }' "$WORKDIR/perf-round2")
HITS1=${HITS1:-0}; MISSES1=${MISSES1:-0}
HITS2=${HITS2:-0}; MISSES2=${MISSES2:-0}
READ1=${READ1:-0}; READ2=${READ2:-0}
TOTAL2=$((HITS2 + MISSES2))
if [ "$TOTAL2" -gt 0 ]; then
    HIT_PERCENT2=$((HITS2 * 100 / TOTAL2))
else
    HIT_PERCENT2=0
fi
if [ "$TRUNCATE_RC" -eq 0 ] \
    && [ "$MISSES1" -gt 0 ] \
    && [ "$READ1" -gt 0 ] \
    && [ "$HIT_PERCENT2" -ge 90 ] \
    && [ "$READ2" -le $((READ1 / 5)) ]; then
    OK=true
    RESULT_RC=0
else
    OK=false
    RESULT_RC=1
fi
echo "FILE_CACHE_RESULT ok=$OK diagnostic=true truncate_correctness=$([ "$TRUNCATE_RC" -eq 0 ] && echo true || echo false) first_hits=$HITS1 first_misses=$MISSES1 second_hits=$HITS2 second_misses=$MISSES2 second_hit_percent=$HIT_PERCENT2 first_read_bytes=$READ1 second_read_bytes=$READ2 path=$TEST_FILE"

echo "#### OS COMP TEST GROUP END file-page-cache-perf ####"
sync
exit "$RESULT_RC"
