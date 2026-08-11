#pragma once
#include "types.hh"

struct DtbMemoryRegion
{
    uint64 base;
    uint64 size;
};

// PLIC 的 context 编号是 interrupts-extended 中元组的原始序号，并不等同于
// hartid。平台层只消费这份已经解析完成的映射，不再自行猜控制器拓扑。
struct DtbRiscvPlicContext
{
    uint64 hartid;
    uint32 context_id;
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
    // 校验并初始化固件提供的 DTB，同时缓存 /chosen 中明确声明的 initrd。
    // 不猜测扫描普通 RAM；没有声明 initrd 时由主块设备直接提供根文件系统。
    static void initialize_boot_dtb(uint64 passed_dtb_addr);
    static int get_memory_regions(DtbMemoryRegion *regions, int max_regions);
    // 同时读取 FDT reservation map 与 /reserved-memory 子节点。返回的区域
    // 使用物理地址，PMM 必须在建立 buddy 前将它们从可分配 RAM 中排除。
    static int get_reserved_regions(DtbMemoryRegion *regions, int max_regions);
    // 读取 /cpus 下可用 CPU 的 hartid。启动代码据此只拉起固件实际声明的次核。
    static int get_cpu_hartids(uint64 *hartids, int max_harts);
    // 读取 /cpus/timebase-frequency。属性缺失、重复、长度错误或值为零时失败。
    static bool get_timebase_frequency(uint64 &frequency_hz);
    // 按 PLIC 的物理 reg 定位节点，再把 interrupts-extended 中 S-mode external
    // interrupt（cause 9）的原始 context 序号关联到 CPU hartid。任何无法安全
    // 跳过的 phandle/interrupt-cells 格式都会整体失败，调用方必须 fail-fast。
    static int get_riscv_plic_contexts(uint64 plic_address,
                                       DtbRiscvPlicContext *contexts,
                                       int max_contexts);
    // 按设备节点 reg 经各级父总线 ranges 翻译后的物理 MMIO 基址读取 MAC，
    // 供板载网卡避免硬编码固件配置；格式不完整时不会用 unit-address 猜测。
    static bool get_mac_address(uint64 device_address, uint8 mac[6]);
private:
    static uint64 _dtb_addr;
};

extern uint64 k_dtb_addr;
extern uint64 k_initrd_start;
extern uint64 k_initrd_end;
