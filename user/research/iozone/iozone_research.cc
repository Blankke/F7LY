/**
 * @file iozone_research.cc
 * @brief iozone 相关的最小复现入口
 *
 * 使用示例：
 * 1. 在 `user/app/initcode-rv.cc` 中启用 `iozone_glibc_random_read_repro();`
 * 2. 构建：`make build ARCH=riscv`
 * 3. 运行：`make run r QEMU_MEM=1G`
 *
 * 说明：
 * - 这里仅保留 iozone 自身的最小复现入口。
 * - priority-borrow 的手写长时 IO 实验已经迁移到 `user/research/priority_borrow_research.cc`。
 */

#include "user.hh"

int iozone_glibc_random_read_repro(void)
{
    // 只跑 glibc 版 iozone random-read，用于快速确认读快路径是否仍会触发 ext4/cache 相关问题。
    init_env("/musl/");
    if (chdir("/glibc") != 0)
    {
        printf("[iozone-repro] chdir /glibc 失败\n");
        return -1;
    }

    char *random_read[16] = {
        (char *)"iozone",
        (char *)"-t",
        (char *)"4",
        (char *)"-i",
        (char *)"0",
        (char *)"-i",
        (char *)"2",
        (char *)"-r",
        (char *)"1k",
        (char *)"-s",
        (char *)"1m",
        0};

    printf("#### IOZONE GLIBC RANDOM-READ REPRO START ####\n");
    int rc = run_test("iozone", random_read, 0);
    printf("#### IOZONE GLIBC RANDOM-READ REPRO END rc=%d ####\n", rc);
    return rc;
}
