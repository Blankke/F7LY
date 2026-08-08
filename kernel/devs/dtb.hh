#pragma once
#include "types.hh"

struct DtbMemoryRegion
{
    uint64 base;
    uint64 size;
};

class DtbManager {
public:
    static constexpr int k_max_memory_regions = 8;
    static constexpr int k_max_reserved_regions = 32;

    static void init(uint64 dtb_addr);
    // 返回 FDT header 中声明的完整 blob 大小；DTB 无效时返回 0。
    // PMM 用它精确排除 DTB 所占页面，避免为保护低端 DTB 而丢弃其后的 RAM。
    static uint64 get_dtb_size();
    static bool get_initrd(uint64& start, uint64& end);
    static void find_dtb_and_initrd(uint64 passed_dtb_addr, uint64 kernel_end_phys);
    static int get_memory_regions(DtbMemoryRegion *regions, int max_regions);
    // 同时读取 FDT reservation map 与 /reserved-memory 子节点。返回的区域
    // 使用物理地址，PMM 必须在建立 buddy 前将它们从可分配 RAM 中排除。
    static int get_reserved_regions(DtbMemoryRegion *regions, int max_regions);
    // 读取 /cpus 下可用 CPU 的 hartid。启动代码据此只拉起 QEMU 实际提供的次核。
    static int get_cpu_hartids(uint64 *hartids, int max_harts);
    // 按设备节点的 MMIO 单元地址读取 MAC，供板载网卡避免硬编码固件配置。
    static bool get_mac_address(uint64 device_address, uint8 mac[6]);
private:
    static uint64 _dtb_addr;
};

extern uint64 k_dtb_addr;
extern uint64 k_initrd_start;
extern uint64 k_initrd_end;
