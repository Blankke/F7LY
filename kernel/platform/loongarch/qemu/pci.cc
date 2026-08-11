#include "platform/pci.hh"

#include "hal/loongarch/platform_board.hh"
#include "mem/loongarch/pagetable.hh"
#include "mem/virtual_memory_manager.hh"
#include "printer.hh"
#include "platform_board_config.hh"

namespace platform::pci
{
namespace
{
constexpr uint32 k_max_buses = 256;
constexpr uint32 k_max_devices = 32;
constexpr uint32 k_max_functions = 8;
constexpr uint32 k_function_config_size = 4096;

constexpr uint16 k_vendor_device_register = 0x00;
constexpr uint16 k_command_register = 0x04;
constexpr uint16 k_header_type_register = 0x0e;
constexpr uint16 k_bar0_register = 0x10;
constexpr uint8 k_endpoint_bar_count = 6;
constexpr uint8 k_bridge_bar_count = 2;

constexpr uint16 k_command_io = 1U << 0;
constexpr uint16 k_command_memory = 1U << 1;
constexpr uint16 k_command_master = 1U << 2;
constexpr uint16 k_command_intx_disable = 1U << 10;

constexpr uint32 k_bar_io_space = 1U << 0;
constexpr uint32 k_bar_memory_type_mask = 3U << 1;
constexpr uint32 k_bar_memory_type_32 = 0U << 1;
constexpr uint32 k_bar_memory_type_64 = 2U << 1;
constexpr uint32 k_bar_attribute_mask = 0xfU;
constexpr uint32 k_bar_address_mask_32 = ~k_bar_attribute_mask;

constexpr uint64 k_uint64_max = ~uint64{0};

bool g_mmio_window_mapped = false;
uint64 g_mmio_next_offset = 0;

// 该画像的 BAR 窗口最少按页分配，因此同时存在的 memory BAR 不可能
// 超过窗口页数。固定表避免在设备发现早期依赖堆分配，并让 capability
// 校验能拿到每个 BAR 的真实硬件大小。
constexpr uint32 k_max_configured_bars =
    loongarch::board::k_pci_mmio.size / PGSIZE;
struct ConfiguredBar
{
    bool present;
    FunctionAddress function;
    uint8 index;
    MemoryBar region;
};
ConfiguredBar g_configured_bars[k_max_configured_bars]{};
uint32 g_configured_bar_count = 0;

FunctionAddress g_configured_functions[k_max_configured_bars]{};
uint32 g_configured_function_count = 0;

// LoongArch MAT=0 表示强序未缓存，和配置空间使用的 0x8... DMW 别名
// 语义一致。设备页不能带缓存一致内存的 PTE_MAT，也不能允许取指。
constexpr uint64 k_device_page_flags = PTE_W | PTE_P | PTE_D | PTE_NX;
static_assert((k_device_page_flags & loongarch::pte_mat_m) == 0);

constexpr uint64 function_offset(FunctionAddress address)
{
    // generic PCIe ECAM 每个 bus 占 1 MiB、device 占 32 KiB、function
    // 占 4 KiB。旧实现使用 256 字节配置机制的位移，会读到错误设备。
    return (static_cast<uint64>(address.bus) << 20) |
           (static_cast<uint64>(address.device) << 15) |
           (static_cast<uint64>(address.function) << 12);
}

uint64 checked_config_address(FunctionAddress address, uint16 register_offset,
                              uint64 width, uint64 alignment)
{
    if (address.bus >= k_max_buses || address.device >= k_max_devices ||
        address.function >= k_max_functions)
    {
        panic("[pci] invalid BDF: %u:%u.%u",
              address.bus, address.device, address.function);
    }
    if (width == 0 || register_offset >= k_function_config_size ||
        width > k_function_config_size - register_offset)
    {
        panic("[pci] config access out of range: %u:%u.%u reg=%u size=%p",
              address.bus, address.device, address.function,
              register_offset, width);
    }
    if (alignment != 0 && (register_offset & (alignment - 1)) != 0)
    {
        panic("[pci] unaligned config access: %u:%u.%u reg=%u align=%p",
              address.bus, address.device, address.function,
              register_offset, alignment);
    }

    const uint64 offset = function_offset(address) + register_offset;
    if (offset >= loongarch::board::k_pci_ecam.size ||
        width > loongarch::board::k_pci_ecam.size - offset)
    {
        panic("[pci] BDF outside ECAM window: %u:%u.%u reg=%u",
              address.bus, address.device, address.function, register_offset);
    }
    return loongarch::board::mmio_address(
        loongarch::board::k_pci_ecam.physical_base + offset);
}

bool is_power_of_two(uint64 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

uint64 normalized_bar_size(uint64 requested_size)
{
    // PCI BAR 探测结果本应是二次幂；静默向上取整会掩盖硬件描述错误，
    // 所以只对小于页的合法 BAR 扩展映射粒度，其余异常直接失败。
    if (!is_power_of_two(requested_size) || requested_size < 16)
    {
        panic("[pci] invalid memory BAR size: %p", requested_size);
    }
    return requested_size < PGSIZE ? PGSIZE : requested_size;
}

uint64 align_up_checked(uint64 value, uint64 alignment)
{
    const uint64 mask = alignment - 1;
    if (value > k_uint64_max - mask)
    {
        panic("[pci] BAR alignment overflow: value=%p alignment=%p",
              value, alignment);
    }
    return (value + mask) & ~mask;
}

void map_device_pages(uint64 virtual_address, uint64 physical_address,
                      uint64 size)
{
    const uint64 first_va = PGROUNDDOWN(virtual_address);
    const uint64 first_pa = PGROUNDDOWN(physical_address);

    for (uint64 page_offset = 0; page_offset < size; page_offset += PGSIZE)
    {
        const uint64 page_va = first_va + page_offset;
        const uint64 page_pa = first_pa + page_offset;
        mem::Pte existing = mem::k_pagetable.walk(page_va, false);
        if (!existing.is_null() && existing.is_valid())
        {
            // 同一设备不能同时存在 cached/uncached 页表别名，否则寄存器值
            // 可能不一致。已存在映射必须保持同一物理页和 MAT=0。
            if (reinterpret_cast<uint64>(existing.pa()) != page_pa ||
                existing.mat() != 0)
            {
                panic("[pci] conflicting MMIO mapping: va=%p pa=%p pte=%p",
                      page_va, page_pa, existing.get_data());
            }
            continue;
        }

        if (!mem::k_vmm.map_pages(mem::k_pagetable, page_va, PGSIZE,
                                  page_pa, k_device_page_flags))
        {
            panic("[pci] map MMIO failed: va=%p pa=%p", page_va, page_pa);
        }
    }
}

void ensure_mmio_window_mapped()
{
    mem::k_vmm.lock_page_table_updates();
    if (!g_mmio_window_mapped)
    {
        map_device_pages(loongarch::board::k_pci_mmio_kernel_virtual_base,
                         loongarch::board::k_pci_mmio.physical_base,
                         loongarch::board::k_pci_mmio.size);
        g_mmio_window_mapped = true;
    }
    mem::k_vmm.unlock_page_table_updates();
}

struct BarAllocation
{
    uint64 bus_address;
};

BarAllocation allocate_mmio(uint64 requested_size)
{
    const uint64 size = normalized_bar_size(requested_size);
    ensure_mmio_window_mapped();

    mem::k_vmm.lock_page_table_updates();
    const uint64 offset = align_up_checked(g_mmio_next_offset, size);
    const uint64 window_size = loongarch::board::k_pci_mmio.size;
    if (offset > window_size || size > window_size - offset)
    {
        panic("[pci] MMIO window exhausted: request=%p next=%p capacity=%p",
              size, offset, window_size);
    }
    g_mmio_next_offset = offset + size;
    mem::k_vmm.unlock_page_table_updates();

    const uint64 base = loongarch::board::k_pci_mmio.physical_base;
    if (base > k_uint64_max - offset)
    {
        panic("[pci] MMIO address overflow: base=%p offset=%p", base, offset);
    }
    return {.bus_address = base + offset};
}

bool same_function(FunctionAddress left, FunctionAddress right)
{
    return left.bus == right.bus && left.device == right.device &&
           left.function == right.function;
}

bool function_is_configured(FunctionAddress address)
{
    for (uint32 index = 0; index < g_configured_function_count; ++index)
    {
        if (same_function(g_configured_functions[index], address))
        {
            return true;
        }
    }
    return false;
}

void record_bar(FunctionAddress function, uint8 index, uint64 bus_address,
                uint64 size)
{
    if (g_configured_bar_count >= k_max_configured_bars)
    {
        panic("[pci] configured BAR table exhausted");
    }
    g_configured_bars[g_configured_bar_count++] = {
        .present = true,
        .function = function,
        .index = index,
        .region = {.bus_address = bus_address, .size = size},
    };
}

void record_function(FunctionAddress address)
{
    if (g_configured_function_count >= k_max_configured_bars)
    {
        panic("[pci] configured function table exhausted");
    }
    g_configured_functions[g_configured_function_count++] = address;
}

uint8 bar_count(FunctionAddress address)
{
    const uint8 header_type = config_read8(address, k_header_type_register) & 0x7f;
    if (header_type == 0)
    {
        return k_endpoint_bar_count;
    }
    if (header_type == 1)
    {
        return k_bridge_bar_count;
    }
    panic("[pci] unsupported header type: %u:%u.%u type=%u",
          address.bus, address.device, address.function, header_type);
}
} // namespace

uint8 config_read8(FunctionAddress address, uint16 register_offset)
{
    const uint64 pointer = checked_config_address(address, register_offset, 1, 1);
    return *reinterpret_cast<volatile uint8 *>(pointer);
}

uint16 config_read16(FunctionAddress address, uint16 register_offset)
{
    const uint64 pointer = checked_config_address(address, register_offset, 2, 2);
    return *reinterpret_cast<volatile uint16 *>(pointer);
}

uint32 config_read32(FunctionAddress address, uint16 register_offset)
{
    const uint64 pointer = checked_config_address(address, register_offset, 4, 4);
    return *reinterpret_cast<volatile uint32 *>(pointer);
}

void config_read(FunctionAddress address, uint16 register_offset,
                 void *buffer, uint64 length)
{
    if (length == 0)
    {
        return;
    }
    if (buffer == nullptr)
    {
        panic("[pci] null config read buffer");
    }

    const uint64 pointer = checked_config_address(address, register_offset,
                                                   length, 1);
    auto *destination = static_cast<uint8 *>(buffer);
    auto *source = reinterpret_cast<volatile uint8 *>(pointer);
    for (uint64 index = 0; index < length; ++index)
    {
        destination[index] = source[index];
    }
}

void config_write8(FunctionAddress address, uint16 register_offset, uint8 value)
{
    const uint64 pointer = checked_config_address(address, register_offset, 1, 1);
    *reinterpret_cast<volatile uint8 *>(pointer) = value;
}

void config_write16(FunctionAddress address, uint16 register_offset, uint16 value)
{
    const uint64 pointer = checked_config_address(address, register_offset, 2, 2);
    *reinterpret_cast<volatile uint16 *>(pointer) = value;
}

void config_write32(FunctionAddress address, uint16 register_offset, uint32 value)
{
    const uint64 pointer = checked_config_address(address, register_offset, 4, 4);
    *reinterpret_cast<volatile uint32 *>(pointer) = value;
}

bool find_device(uint16 vendor_id, uint16 device_id, uint32 instance,
                 FunctionAddress &result)
{
    const uint32 expected_id = (static_cast<uint32>(device_id) << 16) | vendor_id;

    for (uint32 bus = 0; bus < k_max_buses; ++bus)
    {
        for (uint32 device = 0; device < k_max_devices; ++device)
        {
            const FunctionAddress function_zero{
                static_cast<uint16>(bus), static_cast<uint16>(device), 0};
            if ((config_read32(function_zero, k_vendor_device_register) & 0xffffU) ==
                0xffffU)
            {
                continue;
            }

            const bool multifunction =
                (config_read8(function_zero, k_header_type_register) & 0x80U) != 0;
            const uint32 function_count = multifunction ? k_max_functions : 1;
            for (uint32 function = 0; function < function_count; ++function)
            {
                const FunctionAddress candidate{
                    static_cast<uint16>(bus), static_cast<uint16>(device),
                    static_cast<uint16>(function)};
                if (config_read32(candidate, k_vendor_device_register) != expected_id)
                {
                    continue;
                }
                if (instance == 0)
                {
                    result = candidate;
                    return true;
                }
                --instance;
            }
        }
    }
    return false;
}

void enable_and_allocate_bars(FunctionAddress address)
{
    // block/net 初始化可能通过不同路径重复引用同一 function。BAR 分配必须
    // 幂等，否则第二次调用会泄漏窗口并改变正在使用的设备地址。
    if (function_is_configured(address))
    {
        return;
    }

    const uint8 count = bar_count(address);
    const uint16 original_command = config_read16(address, k_command_register);

    // 探测期间关闭 decode 与 DMA，避免写入全 1 的 BAR 被设备短暂解码。
    config_write16(address, k_command_register,
                   original_command & ~(k_command_io | k_command_memory |
                                        k_command_master));
    __sync_synchronize();

    for (uint8 bar = 0; bar < count; ++bar)
    {
        const uint8 programmed_bar = bar;
        const uint16 low_register = k_bar0_register + bar * sizeof(uint32);
        const uint32 original_low = config_read32(address, low_register);

        // 当前平台没有 PCI I/O port 窗口；保留 I/O BAR 原值，也不新增
        // command.IO。VirtIO 使用的都是 memory BAR。
        if ((original_low & k_bar_io_space) != 0)
        {
            continue;
        }

        const uint32 memory_type = original_low & k_bar_memory_type_mask;
        if (memory_type != k_bar_memory_type_32 &&
            memory_type != k_bar_memory_type_64)
        {
            panic("[pci] invalid memory BAR type: %u:%u.%u BAR%u value=%x",
                  address.bus, address.device, address.function, bar, original_low);
        }
        if (memory_type == k_bar_memory_type_64 && bar + 1 >= count)
        {
            panic("[pci] truncated 64-bit BAR: %u:%u.%u BAR%u",
                  address.bus, address.device, address.function, bar);
        }

        uint32 original_high = 0;
        if (memory_type == k_bar_memory_type_64)
        {
            original_high = config_read32(address, low_register + sizeof(uint32));
            config_write32(address, low_register + sizeof(uint32), 0xffffffffU);
        }
        config_write32(address, low_register, 0xffffffffU);
        __sync_synchronize();

        const uint32 probed_low = config_read32(address, low_register);
        uint32 probed_high = 0;
        if (memory_type == k_bar_memory_type_64)
        {
            probed_high = config_read32(address, low_register + sizeof(uint32));
            config_write32(address, low_register + sizeof(uint32), original_high);
        }
        config_write32(address, low_register, original_low);
        __sync_synchronize();

        uint64 size = 0;
        if (memory_type == k_bar_memory_type_64)
        {
            const uint64 mask = (static_cast<uint64>(probed_high) << 32) |
                                (probed_low & k_bar_address_mask_32);
            if (mask == 0)
            {
                ++bar;
                continue;
            }
            size = (~mask) + 1;
        }
        else
        {
            const uint32 mask = probed_low & k_bar_address_mask_32;
            if (mask == 0)
            {
                continue;
            }
            size = static_cast<uint32>(~mask + 1U);
        }

        const BarAllocation mapping = allocate_mmio(size);
        const uint32 attributes = original_low & k_bar_attribute_mask;
        if (memory_type == k_bar_memory_type_64)
        {
            config_write32(address, low_register + sizeof(uint32),
                           static_cast<uint32>(mapping.bus_address >> 32));
            config_write32(address, low_register,
                           static_cast<uint32>(mapping.bus_address) | attributes);
            ++bar;
        }
        else
        {
            if (mapping.bus_address > 0xffffffffULL)
            {
                panic("[pci] 32-bit BAR allocated above 4 GiB: address=%p",
                      mapping.bus_address);
            }
            config_write32(address, low_register,
                           static_cast<uint32>(mapping.bus_address) | attributes);
        }
        __sync_synchronize();
        record_bar(address, programmed_bar, mapping.bus_address, size);
    }

    // VirtIO 只需要 memory decode 和 DMA。显式清除 INTx disable，避免固件
    // 遗留状态让已登记的共享中断永远不到达；不无条件打开未实现的 I/O 空间。
    uint16 enabled_command = original_command | k_command_memory | k_command_master;
    enabled_command &= ~k_command_intx_disable;
    config_write16(address, k_command_register, enabled_command);
    __sync_synchronize();
    record_function(address);
}

bool memory_bar(FunctionAddress address, uint8 bar_index, MemoryBar &result)
{
    for (uint32 index = 0; index < g_configured_bar_count; ++index)
    {
        const ConfiguredBar &bar = g_configured_bars[index];
        if (bar.present && bar.index == bar_index &&
            same_function(bar.function, address))
        {
            result = bar.region;
            return true;
        }
    }
    return false;
}

uint64 mapped_mmio_address(uint64 bus_address, uint64 size)
{
    if (size == 0)
    {
        panic("[pci] cannot map an empty MMIO range");
    }

    const uint64 base = loongarch::board::k_pci_mmio.physical_base;
    const uint64 window_size = loongarch::board::k_pci_mmio.size;
    if (bus_address < base)
    {
        panic("[pci] BAR address below MMIO window: bus=%p", bus_address);
    }
    const uint64 offset = bus_address - base;
    if (offset >= window_size || size > window_size - offset)
    {
        panic("[pci] BAR range outside MMIO window: bus=%p size=%p",
              bus_address, size);
    }

    ensure_mmio_window_mapped();
    return loongarch::board::k_pci_mmio_kernel_virtual_base + offset;
}
} // namespace platform::pci
