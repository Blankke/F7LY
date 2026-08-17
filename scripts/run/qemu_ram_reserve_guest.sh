#!/bin/sh
#
# 模拟 QEMU 为 8 GiB guest RAM 建立匿名宿主映射。测试只触碰首尾页，
# 物理内存开销很小，但会覆盖单 VMA > 2 GiB、LoongArch 页表骨架预建、
# 中段 mprotect 拆分/合并和整段 munmap。

echo "#### OS COMP TEST GROUP START qemu-ram-reserve ####"

export PATH=/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin
WORKDIR=/tmp/f7ly-qemu-ram-reserve
mkdir -p "$WORKDIR" || exit 1

cat >"$WORKDIR/probe.c" <<'EOF'
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>

enum { ITERATIONS = 2 };
static const size_t k_guest_ram_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
static const size_t k_protect_bytes = 2ULL * 1024ULL * 1024ULL;

static uint64_t monotonic_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int main(void) {
    for (int iteration = 0; iteration < ITERATIONS; ++iteration) {
        const uint64_t started_us = monotonic_us();
        volatile unsigned char *ram = mmap(
            NULL, k_guest_ram_bytes, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (ram == MAP_FAILED) {
            printf("QEMU_RAM_RESERVE_RESULT ok=false stage=mmap iteration=%d errno=%d\n",
                   iteration, errno);
            return 1;
        }

        ram[0] = (unsigned char)iteration;
        ram[k_guest_ram_bytes - 1] = (unsigned char)(iteration + 1);

        void *middle = (void *)(ram + k_guest_ram_bytes / 2);
        if (mprotect(middle, k_protect_bytes, PROT_NONE) != 0 ||
            mprotect(middle, k_protect_bytes, PROT_READ | PROT_WRITE) != 0) {
            const int saved_errno = errno;
            munmap((void *)ram, k_guest_ram_bytes);
            printf("QEMU_RAM_RESERVE_RESULT ok=false stage=mprotect iteration=%d errno=%d\n",
                   iteration, saved_errno);
            return 1;
        }

        if (munmap((void *)ram, k_guest_ram_bytes) != 0) {
            printf("QEMU_RAM_RESERVE_RESULT ok=false stage=munmap iteration=%d errno=%d\n",
                   iteration, errno);
            return 1;
        }
        printf("QEMU_RAM_RESERVE_SAMPLE iteration=%d bytes=%zu elapsed_us=%llu\n",
               iteration, k_guest_ram_bytes,
               (unsigned long long)(monotonic_us() - started_us));
        fflush(stdout);
    }

    printf("QEMU_RAM_RESERVE_RESULT ok=true iterations=%d bytes=%zu\n",
           ITERATIONS, k_guest_ram_bytes);
    return 0;
}
EOF

if ! command -v cc >/dev/null 2>&1; then
    echo "QEMU_RAM_RESERVE_RESULT ok=false reason=no-cc"
    echo "#### OS COMP TEST GROUP END qemu-ram-reserve ####"
    exit 1
fi

if ! cc -O2 -Wall -Wextra "$WORKDIR/probe.c" -o "$WORKDIR/probe"; then
    echo "QEMU_RAM_RESERVE_RESULT ok=false reason=compile"
    echo "#### OS COMP TEST GROUP END qemu-ram-reserve ####"
    exit 1
fi

"$WORKDIR/probe"
RC=$?

echo "#### OS COMP TEST GROUP END qemu-ram-reserve ####"
sync
exit "$RC"
