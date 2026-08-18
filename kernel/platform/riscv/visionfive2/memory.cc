#include "platform/memory.hh"

namespace platform::memory
{
uint64 physical_address(uint64 address)
{
    return address;
}

uint64 kernel_access_address(uint64 address)
{
    return address;
}

uint64 managed_physical_top()
{
    // visionfive2 分支完成 SD、ext4 与交互 Shell 实机验收时使用的窗口。
    // JH7110 DTB 会按板载容量报告到 0x140000000 甚至更高，但主线尚未完成
    // 4GiB 以上 PMM、页表及所有 DMA 路径的实机验收；先保持与已知可启动
    // 版本一致，避免把 heap/shm 放到未经验证的高地址后在 VMM 初始化卡死。
    return 0x80000000ULL;
}
} // namespace platform::memory
