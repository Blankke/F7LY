#!/bin/sh
#
# PMM 空闲页计数窄测：先制造离散的已用/空闲物理页，再密集调用 sysinfo。
# 计时窗口不含 mmap、缺页和 madvise，只衡量全局空闲页查询热路径。

echo "#### OS COMP TEST GROUP START free-page-count-perf ####"

export PATH=/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin
WORKDIR=/tmp/f7ly-free-page-count
mkdir -p "$WORKDIR" || exit 1

cat >"$WORKDIR/bench.c" <<'EOF'
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <time.h>

enum { QUERY_COUNT = 5000 };
static const size_t k_fragment_bytes = 32UL * 1024UL * 1024UL;

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    volatile unsigned char *mapping = mmap(
        NULL, k_fragment_bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        puts("FREE_PAGE_COUNT_RESULT ok=false stage=mmap");
        return 1;
    }

    for (size_t offset = 0; offset < k_fragment_bytes; offset += 4096)
        mapping[offset] = (unsigned char)(offset >> 12);
    for (size_t offset = 4096; offset < k_fragment_bytes; offset += 8192) {
        if (madvise((void *)(mapping + offset), 4096, MADV_DONTNEED) != 0) {
            puts("FREE_PAGE_COUNT_RESULT ok=false stage=madvise");
            return 1;
        }
    }

    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        puts("FREE_PAGE_COUNT_RESULT ok=false stage=warmup");
        return 1;
    }

    volatile unsigned long checksum = 0;
    const uint64_t started_ns = monotonic_ns();
    for (int query = 0; query < QUERY_COUNT; ++query) {
        if (sysinfo(&info) != 0) {
            puts("FREE_PAGE_COUNT_RESULT ok=false stage=sysinfo");
            return 1;
        }
        checksum += info.freeram;
    }
    const uint64_t elapsed_ns = monotonic_ns() - started_ns;

    printf("FREE_PAGE_COUNT_RESULT ok=true queries=%d fragment_bytes=%zu "
           "elapsed_us=%llu per_query_ns=%llu checksum=%lu\n",
           QUERY_COUNT, k_fragment_bytes,
           (unsigned long long)(elapsed_ns / 1000ULL),
           (unsigned long long)(elapsed_ns / QUERY_COUNT), checksum);
    return 0;
}
EOF

if ! command -v cc >/dev/null 2>&1; then
    echo "FREE_PAGE_COUNT_RESULT ok=false reason=no-cc"
    echo "#### OS COMP TEST GROUP END free-page-count-perf ####"
    exit 1
fi

if ! cc -O2 -Wall -Wextra "$WORKDIR/bench.c" -o "$WORKDIR/bench"; then
    echo "FREE_PAGE_COUNT_RESULT ok=false reason=compile"
    echo "#### OS COMP TEST GROUP END free-page-count-perf ####"
    exit 1
fi

"$WORKDIR/bench"
RC=$?

echo "#### OS COMP TEST GROUP END free-page-count-perf ####"
sync
exit "$RC"
