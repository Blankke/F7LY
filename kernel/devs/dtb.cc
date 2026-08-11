#include "dtb.hh"
#include "libs/klib.hh"
#include "printer.hh"
#include "platform/memory.hh"

uint64 DtbManager::_dtb_addr = 0;
uint64 k_dtb_addr = 0;
uint64 k_initrd_start = 0;
uint64 k_initrd_end = 0;

struct fdt_header {
    uint32 magic;
    uint32 totalsize;
    uint32 off_dt_struct;
    uint32 off_dt_strings;
    uint32 off_mem_rsvmap;
    uint32 version;
    uint32 last_comp_version;
    uint32 boot_cpuid_phys;
    uint32 size_dt_strings;
    uint32 size_dt_struct;
};

#define FDT_MAGIC 0xd00dfeed
#define FDT_BEGIN_NODE 0x1
#define FDT_END_NODE 0x2
#define FDT_PROP 0x3
#define FDT_NOP 0x4
#define FDT_END 0x9

namespace
{
    constexpr uint64 k_max_fdt_size = 16ULL * 1024 * 1024;

    // _dtb_addr 始终保存“当前平台早期内核可直接访问”的地址；k_dtb_addr
    // 则保存物理坐标，供 PMM 排除 DTB 页面。地址别名规则只由平台实现。
    static uint64 normalize_dtb_addr_for_kernel(uint64 dtb_addr)
    {
        return dtb_addr == 0
                   ? 0
                   : platform::memory::kernel_access_address(dtb_addr);
    }

    struct FdtCursor
    {
        const uint8 *blob_base = nullptr;
        const uint8 *blob_end = nullptr;
        const uint8 *struct_base = nullptr;
        const uint8 *struct_end = nullptr;
        const uint8 *strings_base = nullptr;
        const uint8 *strings_end = nullptr;
        const uint8 *reservation_map = nullptr;
        uint32 total_size = 0;
        uint32 struct_size = 0;
        uint32 strings_size = 0;
    };

    uint32 read_be32(const void *data)
    {
        const auto *bytes = reinterpret_cast<const uint8 *>(data);
        return (static_cast<uint32>(bytes[0]) << 24) |
               (static_cast<uint32>(bytes[1]) << 16) |
               (static_cast<uint32>(bytes[2]) << 8) |
               static_cast<uint32>(bytes[3]);
    }

    uint64 read_be64(const void *data)
    {
        const auto *bytes = reinterpret_cast<const uint8 *>(data);
        return (static_cast<uint64>(read_be32(bytes)) << 32) |
               read_be32(bytes + 4);
    }

    bool blob_range_valid(uint32 total_size, uint32 offset, uint32 size)
    {
        return offset <= total_size && size <= total_size - offset;
    }

    static bool load_fdt_cursor(uint64 dtb_addr, FdtCursor &cursor,
                                const char **failure_reason = nullptr)
    {
        if (failure_reason != nullptr)
        {
            *failure_reason = nullptr;
        }
        auto fail = [&](const char *reason) {
            if (failure_reason != nullptr)
            {
                *failure_reason = reason;
            }
            return false;
        };

        if (dtb_addr == 0)
        {
            return fail("address is zero");
        }

        const auto *header = reinterpret_cast<const fdt_header *>(dtb_addr);
        if (read_be32(&header->magic) != FDT_MAGIC)
        {
            return fail("bad FDT magic");
        }

        const uint32 total_size = read_be32(&header->totalsize);
        const uint32 off_struct = read_be32(&header->off_dt_struct);
        const uint32 off_strings = read_be32(&header->off_dt_strings);
        const uint32 off_reservation_map = read_be32(&header->off_mem_rsvmap);
        const uint32 struct_size = read_be32(&header->size_dt_struct);
        const uint32 strings_size = read_be32(&header->size_dt_strings);
        const uint32 version = read_be32(&header->version);
        const uint32 last_compatible_version = read_be32(&header->last_comp_version);

        // 本内核只接受包含 size_dt_struct/size_dt_strings 的现代 FDT。限制
        // totalsize 还能阻止损坏头部把后续边界检查扩展到任意内存。
        if (total_size < sizeof(fdt_header) || total_size > k_max_fdt_size)
        {
            return fail("totalsize is outside 40B..16MiB");
        }
        if (version < 17 || last_compatible_version > 17)
        {
            return fail("unsupported FDT version");
        }
        if ((off_struct & 3U) != 0)
        {
            return fail("structure offset is not 4-byte aligned");
        }
        if ((off_reservation_map & 7U) != 0)
        {
            return fail("reservation-map offset is not 8-byte aligned");
        }
        if (struct_size < sizeof(uint32) ||
            !blob_range_valid(total_size, off_struct, struct_size))
        {
            return fail("structure block is outside the blob");
        }
        if (!blob_range_valid(total_size, off_strings, strings_size))
        {
            return fail("strings block is outside the blob");
        }
        if (!blob_range_valid(total_size, off_reservation_map, 16))
        {
            return fail("reservation map is outside the blob");
        }

        const auto *blob = reinterpret_cast<const uint8 *>(dtb_addr);
        cursor.blob_base = blob;
        cursor.blob_end = blob + total_size;
        cursor.struct_base = blob + off_struct;
        cursor.struct_end = cursor.struct_base + struct_size;
        cursor.strings_base = blob + off_strings;
        cursor.strings_end = cursor.strings_base + strings_size;
        cursor.reservation_map = blob + off_reservation_map;
        cursor.total_size = total_size;
        cursor.struct_size = struct_size;
        cursor.strings_size = strings_size;
        return true;
    }

    const char *bounded_string(const uint8 *start, const uint8 *end)
    {
        if (start == nullptr || start >= end)
        {
            return nullptr;
        }
        for (const uint8 *cursor = start; cursor < end; ++cursor)
        {
            if (*cursor == 0)
            {
                return reinterpret_cast<const char *>(start);
            }
        }
        return nullptr;
    }

    const char *property_name(const FdtCursor &cursor, uint32 name_offset)
    {
        if (name_offset >= cursor.strings_size)
        {
            return nullptr;
        }
        return bounded_string(cursor.strings_base + name_offset, cursor.strings_end);
    }

    static bool read_fdt_cells(const uint8 *data, int cells, uint64 &value)
    {
        if (data == nullptr || cells <= 0 || cells > 2)
        {
            return false;
        }
        value = 0;
        for (int i = 0; i < cells; ++i)
        {
            value = (value << 32) | read_be32(data + i * 4);
        }
        return true;
    }

    enum class FdtEventType
    {
        begin_node,
        end_node,
        property,
    };

    struct FdtEvent
    {
        FdtEventType type = FdtEventType::property;
        int depth = -1; // 根节点深度为 0。
        const char *name = nullptr;
        const char *property = nullptr;
        const uint8 *value = nullptr;
        uint32 length = 0;
    };

    class FdtWalker
    {
    public:
        explicit FdtWalker(const FdtCursor &cursor)
            : _cursor(cursor), _position(cursor.struct_base)
        {
        }

        bool next(FdtEvent &event)
        {
            while (_valid && !_finished)
            {
                const uint64 position = reinterpret_cast<uint64>(_position);
                const uint64 aligned = (position + 3U) & ~3ULL;
                if (aligned < position || aligned > reinterpret_cast<uint64>(_cursor.struct_end))
                {
                    _valid = false;
                    break;
                }
                _position = reinterpret_cast<const uint8 *>(aligned);
                if (static_cast<uint64>(_cursor.struct_end - _position) < sizeof(uint32))
                {
                    _valid = false;
                    break;
                }

                const uint32 token = read_be32(_position);
                _position += sizeof(uint32);
                if (token == FDT_NOP)
                {
                    continue;
                }
                if (token == FDT_END)
                {
                    _finished = _depth == 0;
                    _valid = _finished;
                    break;
                }
                if (token == FDT_BEGIN_NODE)
                {
                    const char *name = bounded_string(_position, _cursor.struct_end);
                    if (name == nullptr)
                    {
                        _valid = false;
                        break;
                    }
                    const uint8 *name_end = _position;
                    while (name_end < _cursor.struct_end && *name_end != 0)
                    {
                        ++name_end;
                    }
                    _position = name_end + 1;
                    event = {};
                    event.type = FdtEventType::begin_node;
                    event.depth = _depth++;
                    event.name = name;
                    return true;
                }
                if (token == FDT_END_NODE)
                {
                    if (_depth <= 0)
                    {
                        _valid = false;
                        break;
                    }
                    --_depth;
                    event = {};
                    event.type = FdtEventType::end_node;
                    event.depth = _depth;
                    return true;
                }
                if (token != FDT_PROP ||
                    static_cast<uint64>(_cursor.struct_end - _position) < 8)
                {
                    _valid = false;
                    break;
                }

                const uint32 length = read_be32(_position);
                const uint32 name_offset = read_be32(_position + 4);
                _position += 8;
                const uint64 padded_length = (static_cast<uint64>(length) + 3U) & ~3ULL;
                if (padded_length < length ||
                    padded_length > static_cast<uint64>(_cursor.struct_end - _position))
                {
                    _valid = false;
                    break;
                }
                const char *name = property_name(_cursor, name_offset);
                if (name == nullptr || _depth <= 0)
                {
                    _valid = false;
                    break;
                }

                event = {};
                event.type = FdtEventType::property;
                event.depth = _depth - 1;
                event.property = name;
                event.value = _position;
                event.length = length;
                _position += padded_length;
                return true;
            }
            return false;
        }

        bool valid() const { return _valid && _finished; }

    private:
        const FdtCursor &_cursor;
        const uint8 *_position = nullptr;
        int _depth = 0;
        bool _valid = true;
        bool _finished = false;
    };

    bool validate_fdt_structure(const FdtCursor &cursor)
    {
        FdtWalker walker(cursor);
        FdtEvent event{};
        while (walker.next(event))
        {
        }
        return walker.valid();
    }

    static bool parse_node_unit_address(const char *node_name, uint64 &addr)
    {
        if (node_name == nullptr)
        {
            return false;
        }

        const char *at = strchr(node_name, '@');
        if (at == nullptr || *(at + 1) == '\0')
        {
            return false;
        }

        uint64 value = 0;
        for (const char *p = at + 1; *p != '\0'; ++p)
        {
            char c = *p;
            uint64 digit = 0;
            if (c >= '0' && c <= '9')
            {
                digit = (uint64)(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                digit = (uint64)(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                digit = (uint64)(c - 'A' + 10);
            }
            else
            {
                return false;
            }
            if (value > (~0ULL >> 4))
            {
                return false;
            }
            value = (value << 4) | digit;
        }

        addr = value;
        return true;
    }

    constexpr int k_max_address_translation_depth = 32;

    // FDT 的 reg 使用父节点声明的单元宽度，而 ranges 使用“当前总线的
    // child address + 父总线的 parent address + 当前总线的 size”。这里
    // 只保存仍在遍历路径上的节点，因此不需要堆内存，也不会缓存失效指针。
    struct FdtAddressFrame
    {
        uint32 child_address_cells = 2;
        uint32 child_size_cells = 1;
        bool cell_layout_valid = true;

        bool ranges_present = false;
        const uint8 *ranges = nullptr;
        uint32 ranges_length = 0;

        const uint8 *reg = nullptr;
        uint32 reg_length = 0;
        bool enabled = true;

        bool mac_valid = false;
        bool mac_is_local = false;
        uint8 mac[6]{};

        bool interrupt_controller = false;
        const uint8 *interrupts_extended = nullptr;
        uint32 interrupts_extended_length = 0;
    };

    struct FdtCpuInterruptController
    {
        uint32 phandle = 0;
        uint32 interrupt_cells = 0;
        uint64 hartid = 0;
        bool cpu_enabled = false;
    };

    constexpr int k_max_cpu_interrupt_controllers = 32;

    enum class FdtAddressResult
    {
        success,
        no_mapping,
        malformed,
        unsupported,
    };

    static bool valid_mac_property(const uint8 *value, uint32 length,
                                   uint8 mac[6])
    {
        if (value == nullptr || length != 6)
        {
            return false;
        }

        memmove(mac, value, 6);
        if (mac[0] == 0xff || (mac[0] & 1U) != 0)
        {
            return false;
        }
        for (uint32 index = 0; index < 6; ++index)
        {
            if (mac[index] != 0)
            {
                return true;
            }
        }
        return false;
    }

    static bool fdt_status_is_enabled(const uint8 *value, uint32 length)
    {
        // status 是字符串属性；只接受规范的 "ok"/"okay"，避免把诸如
        // "okra" 的损坏值按前两个字符误判成已启用。
        return (length == 3 && memcmp(value, "ok\0", 3) == 0) ||
               (length == 5 && memcmp(value, "okay\0", 5) == 0);
    }

    static FdtAddressResult translate_through_bus(
        const FdtAddressFrame &bus, const FdtAddressFrame &parent,
        uint64 child_address, uint64 &parent_address)
    {
        if (!bus.cell_layout_valid || !parent.cell_layout_valid)
        {
            return FdtAddressResult::malformed;
        }
        if (bus.child_address_cells == 0 || bus.child_address_cells > 2 ||
            parent.child_address_cells == 0 || parent.child_address_cells > 2 ||
            bus.child_size_cells > 2)
        {
            // uint64 无法忠实表达 PCI 等三单元、带空间标志的地址。静默截断
            // 会匹配到错误设备，因此由上层明确放弃这个候选节点。
            return FdtAddressResult::unsupported;
        }
        if (!bus.ranges_present)
        {
            // 缺少 ranges 表示该总线没有声明到父地址域的映射；它不同于
            // 长度为零的 ranges，后者才表示恒等映射。
            return FdtAddressResult::no_mapping;
        }
        if (bus.ranges_length == 0)
        {
            parent_address = child_address;
            return FdtAddressResult::success;
        }
        if (bus.child_size_cells == 0 || (bus.ranges_length & 3U) != 0)
        {
            return FdtAddressResult::malformed;
        }

        const uint32 entry_cells = bus.child_address_cells +
                                   parent.child_address_cells +
                                   bus.child_size_cells;
        const uint32 entry_bytes = entry_cells * sizeof(uint32);
        if (entry_cells == 0 || bus.ranges_length % entry_bytes != 0)
        {
            return FdtAddressResult::malformed;
        }

        for (uint32 offset = 0; offset < bus.ranges_length; offset += entry_bytes)
        {
            const uint8 *entry = bus.ranges + offset;
            uint64 child_base = 0;
            uint64 parent_base = 0;
            uint64 size = 0;
            if (!read_fdt_cells(entry, static_cast<int>(bus.child_address_cells),
                                child_base) ||
                !read_fdt_cells(entry + bus.child_address_cells * sizeof(uint32),
                                static_cast<int>(parent.child_address_cells),
                                parent_base) ||
                !read_fdt_cells(
                    entry + (bus.child_address_cells + parent.child_address_cells) *
                                sizeof(uint32),
                    static_cast<int>(bus.child_size_cells), size))
            {
                return FdtAddressResult::malformed;
            }

            if (size == 0 || child_address < child_base)
            {
                continue;
            }
            const uint64 delta = child_address - child_base;
            if (delta >= size)
            {
                continue;
            }
            if (parent_base > ~0ULL - delta)
            {
                return FdtAddressResult::malformed;
            }
            parent_address = parent_base + delta;
            return FdtAddressResult::success;
        }
        return FdtAddressResult::no_mapping;
    }

    static FdtAddressResult translate_reg_address(
        const FdtAddressFrame frames[k_max_address_translation_depth],
        int node_depth, uint64 child_address, uint64 &physical_address)
    {
        uint64 translated = child_address;
        // 深度 0 是根节点；直接位于根下的设备已经使用根物理地址域。从
        // 设备父总线开始逐级应用 ranges，直到抵达根地址域。
        for (int bus_depth = node_depth - 1; bus_depth > 0; --bus_depth)
        {
            uint64 parent_address = 0;
            const FdtAddressResult result = translate_through_bus(
                frames[bus_depth], frames[bus_depth - 1], translated,
                parent_address);
            if (result != FdtAddressResult::success)
            {
                return result;
            }
            translated = parent_address;
        }
        physical_address = translated;
        return FdtAddressResult::success;
    }
} // namespace

void DtbManager::init(uint64 dtb_addr) {
    _dtb_addr = normalize_dtb_addr_for_kernel(dtb_addr);
}

uint64 DtbManager::get_dtb_size()
{
    FdtCursor cursor{};
    return load_fdt_cursor(_dtb_addr, cursor) ? cursor.total_size : 0;
}

int DtbManager::get_memory_regions(DtbMemoryRegion *regions, int max_regions)
{
    if (regions == nullptr || max_regions <= 0)
    {
        return 0;
    }

    FdtCursor cursor{};
    if (!load_fdt_cursor(_dtb_addr, cursor))
    {
        return 0;
    }

    int root_addr_cells = 2;
    int root_size_cells = 2;
    int region_count = 0;

    int memory_depth = -1;
    bool memory_name_matches = false;
    bool memory_device_type_matches = false;
    bool memory_enabled = true;
    uint64 memory_node_addr = 0;
    bool memory_node_addr_valid = false;
    const uint8 *memory_reg = nullptr;
    uint32 memory_reg_length = 0;

    auto store_memory_node = [&]() {
        if ((!memory_name_matches && !memory_device_type_matches) || !memory_enabled ||
            memory_reg == nullptr || root_addr_cells <= 0 || root_addr_cells > 2 ||
            root_size_cells <= 0 || root_size_cells > 2)
        {
            return;
        }

        const int entry_cells = root_addr_cells + root_size_cells;
        const int total_cells = static_cast<int>(memory_reg_length / 4);
        if ((memory_reg_length & 3U) != 0 || total_cells < entry_cells)
        {
            return;
        }

        for (int cell_index = 0; cell_index + entry_cells <= total_cells;
             cell_index += entry_cells)
        {
            uint64 base = 0;
            uint64 size = 0;
            if (!read_fdt_cells(memory_reg + cell_index * 4, root_addr_cells, base) ||
                !read_fdt_cells(memory_reg + (cell_index + root_addr_cells) * 4,
                                root_size_cells, size))
            {
                continue;
            }

            // 部分旧 LoongArch QEMU DTB 在 reg 高 32 位携带地址标记，而节点名
            // 保留真实物理基址。仅在二者低位严格一致时修正该已知固件格式。
            if (memory_node_addr_valid && base > 0xFFFFFFFFULL &&
                (base & 0xFFFFFFFFULL) == memory_node_addr)
            {
                const uint64 address_marker = base & 0xFFFFFFFF00000000ULL;
                base = memory_node_addr;
                if ((size & 0xFFFFFFFF00000000ULL) == address_marker)
                {
                    size &= 0xFFFFFFFFULL;
                }
            }
            if (base == 0 && memory_node_addr_valid && memory_node_addr != 0)
            {
                base = memory_node_addr;
            }
            if (size == 0 || base > ~0ULL - size)
            {
                continue;
            }
            if (region_count < max_regions)
            {
                regions[region_count] = {base, size};
            }
            ++region_count;
        }
    };

    FdtWalker walker(cursor);
    FdtEvent event{};
    while (walker.next(event))
    {
        if (event.type == FdtEventType::begin_node)
        {
            if (event.depth == 1 && strncmp(event.name, "memory", 6) == 0)
            {
                memory_depth = event.depth;
                memory_name_matches = event.name[6] == '\0' || event.name[6] == '@';
                memory_device_type_matches = false;
                memory_enabled = true;
                memory_node_addr = 0;
                memory_node_addr_valid = parse_node_unit_address(event.name, memory_node_addr);
                memory_reg = nullptr;
                memory_reg_length = 0;
            }
            continue;
        }
        if (event.type == FdtEventType::end_node)
        {
            if (event.depth == memory_depth)
            {
                store_memory_node();
                memory_depth = -1;
            }
            continue;
        }
        if (event.depth == 0)
        {
            if (strcmp(event.property, "#address-cells") == 0 && event.length == 4)
            {
                root_addr_cells = static_cast<int>(read_be32(event.value));
            }
            else if (strcmp(event.property, "#size-cells") == 0 && event.length == 4)
            {
                root_size_cells = static_cast<int>(read_be32(event.value));
            }
            continue;
        }
        if (event.depth != memory_depth)
        {
            continue;
        }
        if (strcmp(event.property, "device_type") == 0)
        {
            memory_device_type_matches = event.length >= 7 &&
                                         memcmp(event.value, "memory", 7) == 0;
        }
        else if (strcmp(event.property, "status") == 0)
        {
            memory_enabled = event.length >= 2 && memcmp(event.value, "ok", 2) == 0;
        }
        else if (strcmp(event.property, "reg") == 0)
        {
            memory_reg = event.value;
            memory_reg_length = event.length;
        }
    }

    if (!walker.valid())
    {
        return 0;
    }

    int stored = region_count < max_regions ? region_count : max_regions;
    for (int i = 0; i < stored; ++i)
    {
        for (int j = i + 1; j < stored; ++j)
        {
            if (regions[j].base < regions[i].base)
            {
                DtbMemoryRegion tmp = regions[i];
                regions[i] = regions[j];
                regions[j] = tmp;
            }
        }
    }
    return stored;
}

int DtbManager::get_reserved_regions(DtbMemoryRegion *regions, int max_regions)
{
    if (regions == nullptr || max_regions <= 0)
    {
        return 0;
    }

    FdtCursor cursor{};
    if (!load_fdt_cursor(_dtb_addr, cursor))
    {
        return 0;
    }

    int count = 0;
    auto append_region = [&](uint64 base, uint64 size) {
        if (size == 0 || base > ~0ULL - size)
        {
            return;
        }
        if (count < max_regions)
        {
            regions[count] = {base, size};
            ++count;
        }
    };

    // FDT header 自带的 reservation map 是一组 big-endian (address,size)，
    // 以 (0,0) 结束。它通常记录固件运行区等不一定出现在设备树节点中的内存。
    const uint8 *reservation = cursor.reservation_map;
    bool reservation_map_terminated = false;
    while (reservation <= cursor.blob_end &&
           static_cast<uint64>(cursor.blob_end - reservation) >= 16)
    {
        const uint64 base = read_be64(reservation);
        const uint64 size = read_be64(reservation + 8);
        reservation += 16;
        if (base == 0 && size == 0)
        {
            reservation_map_terminated = true;
            break;
        }
        append_region(base, size);
    }
    if (!reservation_map_terminated)
    {
        boardPrintfError("[DTB] reservation map has no terminator inside totalsize\n");
        printfRed("[DTB] reservation map has no terminator inside totalsize\n");
        return 0;
    }

    int root_addr_cells = 2;
    int root_size_cells = 2;
    int reserved_memory_depth = -1;
    int reserved_addr_cells = 2;
    int reserved_size_cells = 2;
    int child_depth = -1;
    bool child_enabled = true;
    const uint8 *child_reg = nullptr;
    uint32 child_reg_length = 0;

    auto store_child = [&]() {
        if (!child_enabled || child_reg == nullptr || reserved_addr_cells <= 0 ||
            reserved_addr_cells > 2 || reserved_size_cells <= 0 ||
            reserved_size_cells > 2 || (child_reg_length & 3U) != 0)
        {
            return;
        }
        const int entry_cells = reserved_addr_cells + reserved_size_cells;
        const int total_cells = static_cast<int>(child_reg_length / 4);
        for (int cell = 0; cell + entry_cells <= total_cells; cell += entry_cells)
        {
            uint64 base = 0;
            uint64 size = 0;
            if (read_fdt_cells(child_reg + cell * 4, reserved_addr_cells, base) &&
                read_fdt_cells(child_reg + (cell + reserved_addr_cells) * 4,
                               reserved_size_cells, size))
            {
                append_region(base, size);
            }
        }
    };

    FdtWalker walker(cursor);
    FdtEvent event{};
    while (walker.next(event))
    {
        if (event.type == FdtEventType::begin_node)
        {
            if (event.depth == 1 && strcmp(event.name, "reserved-memory") == 0)
            {
                reserved_memory_depth = event.depth;
                reserved_addr_cells = root_addr_cells;
                reserved_size_cells = root_size_cells;
            }
            else if (reserved_memory_depth >= 0 &&
                     event.depth == reserved_memory_depth + 1)
            {
                child_depth = event.depth;
                child_enabled = true;
                child_reg = nullptr;
                child_reg_length = 0;
            }
            continue;
        }
        if (event.type == FdtEventType::end_node)
        {
            if (event.depth == child_depth)
            {
                store_child();
                child_depth = -1;
            }
            if (event.depth == reserved_memory_depth)
            {
                reserved_memory_depth = -1;
            }
            continue;
        }
        if (event.depth == 0)
        {
            if (strcmp(event.property, "#address-cells") == 0 && event.length == 4)
            {
                root_addr_cells = static_cast<int>(read_be32(event.value));
            }
            else if (strcmp(event.property, "#size-cells") == 0 && event.length == 4)
            {
                root_size_cells = static_cast<int>(read_be32(event.value));
            }
            continue;
        }
        if (event.depth == reserved_memory_depth)
        {
            if (strcmp(event.property, "#address-cells") == 0 && event.length == 4)
            {
                reserved_addr_cells = static_cast<int>(read_be32(event.value));
            }
            else if (strcmp(event.property, "#size-cells") == 0 && event.length == 4)
            {
                reserved_size_cells = static_cast<int>(read_be32(event.value));
            }
            continue;
        }
        if (event.depth != child_depth)
        {
            continue;
        }
        if (strcmp(event.property, "status") == 0)
        {
            child_enabled = event.length >= 2 && memcmp(event.value, "ok", 2) == 0;
        }
        else if (strcmp(event.property, "reg") == 0)
        {
            child_reg = event.value;
            child_reg_length = event.length;
        }
    }

    if (!walker.valid())
    {
        boardPrintfError("[DTB] reserved-memory traversal failed after initial validation\n");
        printfRed("[DTB] reserved-memory traversal failed after initial validation\n");
        return 0;
    }

    for (int left = 0; left < count; ++left)
    {
        for (int right = left + 1; right < count; ++right)
        {
            if (regions[right].base < regions[left].base)
            {
                const DtbMemoryRegion temporary = regions[left];
                regions[left] = regions[right];
                regions[right] = temporary;
            }
        }
    }
    return count;
}

int DtbManager::get_cpu_hartids(uint64 *hartids, int max_harts)
{
    if (hartids == nullptr || max_harts <= 0)
    {
        return 0;
    }

    FdtCursor cursor{};
    if (!load_fdt_cursor(_dtb_addr, cursor))
    {
        return 0;
    }

    int cpus_depth = -1;
    int cpus_addr_cells = 1;
    int cpu_depth = -1;
    bool cpu_name_matches = false;
    bool cpu_device_type_matches = false;
    bool cpu_enabled = true;
    const uint8 *cpu_reg = nullptr;
    uint32 cpu_reg_length = 0;
    int found = 0;

    auto store_cpu = [&]() {
        if ((!cpu_name_matches && !cpu_device_type_matches) || !cpu_enabled ||
            cpu_reg == nullptr || cpus_addr_cells <= 0 || cpus_addr_cells > 2 ||
            cpu_reg_length < static_cast<uint32>(cpus_addr_cells * 4))
        {
            return;
        }
        uint64 cpu_hartid = 0;
        if (!read_fdt_cells(cpu_reg, cpus_addr_cells, cpu_hartid))
        {
            return;
        }
        for (int index = 0; index < found; ++index)
        {
            if (hartids[index] == cpu_hartid)
            {
                return;
            }
        }
        if (found < max_harts)
        {
            hartids[found++] = cpu_hartid;
        }
    };

    FdtWalker walker(cursor);
    FdtEvent event{};
    while (walker.next(event))
    {
        if (event.type == FdtEventType::begin_node)
        {
            if (event.depth == 1 && strcmp(event.name, "cpus") == 0)
            {
                cpus_depth = event.depth;
                cpus_addr_cells = 1;
            }
            else if (cpus_depth >= 0 && event.depth == cpus_depth + 1)
            {
                cpu_depth = event.depth;
                cpu_name_matches = strncmp(event.name, "cpu@", 4) == 0;
                cpu_device_type_matches = false;
                cpu_enabled = true;
                cpu_reg = nullptr;
                cpu_reg_length = 0;
            }
            continue;
        }
        if (event.type == FdtEventType::end_node)
        {
            if (cpu_depth == event.depth)
            {
                store_cpu();
                cpu_depth = -1;
            }
            if (cpus_depth == event.depth)
            {
                cpus_depth = -1;
            }
            continue;
        }

        // /cpus 节点本身的地址单元数决定子 cpu@ 节点 reg 的编码宽度。
        if (cpus_depth >= 0 && event.depth == cpus_depth &&
            strcmp(event.property, "#address-cells") == 0 && event.length == 4)
        {
            cpus_addr_cells = static_cast<int>(read_be32(event.value));
            continue;
        }

        if (cpu_depth < 0 || event.depth != cpu_depth)
        {
            continue;
        }

        if (strcmp(event.property, "device_type") == 0)
        {
            cpu_device_type_matches = event.length >= 3 &&
                                      memcmp(event.value, "cpu", 3) == 0;
        }
        else if (strcmp(event.property, "status") == 0)
        {
            cpu_enabled = event.length >= 2 && memcmp(event.value, "ok", 2) == 0;
        }
        else if (strcmp(event.property, "reg") == 0)
        {
            cpu_reg = event.value;
            cpu_reg_length = event.length;
        }
    }

    return walker.valid() ? found : 0;
}

bool DtbManager::get_timebase_frequency(uint64 &frequency_hz)
{
    frequency_hz = 0;

    FdtCursor cursor{};
    if (!load_fdt_cursor(_dtb_addr, cursor))
    {
        return false;
    }

    int cpus_depth = -1;
    bool property_found = false;
    bool property_valid = true;
    FdtWalker walker(cursor);
    FdtEvent event{};
    while (walker.next(event))
    {
        if (event.type == FdtEventType::begin_node && event.depth == 1 &&
            strcmp(event.name, "cpus") == 0)
        {
            cpus_depth = event.depth;
            continue;
        }
        if (event.type == FdtEventType::end_node && event.depth == cpus_depth)
        {
            cpus_depth = -1;
            continue;
        }
        if (event.type != FdtEventType::property || event.depth != cpus_depth ||
            strcmp(event.property, "timebase-frequency") != 0)
        {
            continue;
        }

        // Devicetree 的 timebase-frequency 是单个 u32。重复属性或私自扩展为
        // 其它宽度都不能静默截断，否则定时器换算会整体漂移。
        if (property_found || event.length != sizeof(uint32))
        {
            property_valid = false;
            continue;
        }
        property_found = true;
        frequency_hz = read_be32(event.value);
        property_valid = frequency_hz != 0;
    }

    return walker.valid() && property_found && property_valid;
}

int DtbManager::get_riscv_plic_contexts(uint64 plic_address,
                                         DtbRiscvPlicContext *contexts,
                                         int max_contexts)
{
    if (contexts == nullptr || max_contexts <= 0)
    {
        return 0;
    }

    FdtCursor cursor{};
    if (!load_fdt_cursor(_dtb_addr, cursor))
    {
        return 0;
    }

    // 第一遍只建立 CPU interrupt-controller phandle -> hartid 表。PLIC 的
    // interrupts-extended 可以混排 M/S context，只有借助这张表才能知道每个
    // 元组实际属于哪个 hart，不能从元组序号反推。
    FdtCpuInterruptController controllers[k_max_cpu_interrupt_controllers]{};
    int controller_count = 0;
    bool controller_table_valid = true;

    int cpus_depth = -1;
    int cpus_address_cells = 1;
    int cpu_depth = -1;
    bool cpu_name_matches = false;
    bool cpu_device_type_matches = false;
    bool cpu_enabled = true;
    const uint8 *cpu_reg = nullptr;
    uint32 cpu_reg_length = 0;

    int interrupt_controller_depth = -1;
    bool interrupt_controller_marker = false;
    bool interrupt_controller_valid = true;
    bool interrupt_controller_phandle_seen = false;
    uint32 interrupt_controller_phandle = 0;
    bool interrupt_cells_seen = false;
    uint32 interrupt_cells = 0;

    bool cpu_controller_seen = false;
    bool cpu_controller_valid = true;
    uint32 cpu_controller_phandle = 0;
    uint32 cpu_controller_cells = 0;

    auto finish_interrupt_controller = [&]() {
        if (!interrupt_controller_marker)
        {
            return;
        }
        const bool complete = interrupt_controller_valid &&
                              interrupt_controller_phandle_seen &&
                              interrupt_controller_phandle != 0 &&
                              interrupt_cells_seen && interrupt_cells != 0;
        if (cpu_controller_seen)
        {
            // 一个 CPU 节点出现多个 interrupt-controller 时无法确定 PLIC
            // phandle 应关联到哪一个，宁可使整张映射失效。
            cpu_controller_valid = false;
            return;
        }
        cpu_controller_seen = true;
        cpu_controller_valid = complete;
        cpu_controller_phandle = interrupt_controller_phandle;
        cpu_controller_cells = interrupt_cells;
    };

    auto finish_cpu = [&]() {
        if ((!cpu_name_matches && !cpu_device_type_matches) ||
            cpu_reg == nullptr || cpus_address_cells <= 0 ||
            cpus_address_cells > 2 ||
            cpu_reg_length < static_cast<uint32>(cpus_address_cells * 4) ||
            !cpu_controller_seen || !cpu_controller_valid)
        {
            return;
        }

        uint64 hartid = 0;
        if (!read_fdt_cells(cpu_reg, cpus_address_cells, hartid))
        {
            controller_table_valid = false;
            return;
        }
        for (int index = 0; index < controller_count; ++index)
        {
            if (controllers[index].phandle == cpu_controller_phandle)
            {
                controller_table_valid = false;
                return;
            }
        }
        if (controller_count >= k_max_cpu_interrupt_controllers)
        {
            controller_table_valid = false;
            return;
        }
        controllers[controller_count++] = {
            .phandle = cpu_controller_phandle,
            .interrupt_cells = cpu_controller_cells,
            .hartid = hartid,
            .cpu_enabled = cpu_enabled,
        };
    };

    FdtWalker cpu_walker(cursor);
    FdtEvent event{};
    while (cpu_walker.next(event))
    {
        if (event.type == FdtEventType::begin_node)
        {
            if (event.depth == 1 && strcmp(event.name, "cpus") == 0)
            {
                cpus_depth = event.depth;
                cpus_address_cells = 1;
            }
            else if (cpus_depth >= 0 && event.depth == cpus_depth + 1)
            {
                cpu_depth = event.depth;
                cpu_name_matches = strncmp(event.name, "cpu@", 4) == 0;
                cpu_device_type_matches = false;
                cpu_enabled = true;
                cpu_reg = nullptr;
                cpu_reg_length = 0;
                cpu_controller_seen = false;
                cpu_controller_valid = true;
                cpu_controller_phandle = 0;
                cpu_controller_cells = 0;
            }
            else if (cpu_depth >= 0 && event.depth == cpu_depth + 1)
            {
                interrupt_controller_depth = event.depth;
                interrupt_controller_marker = false;
                interrupt_controller_valid = true;
                interrupt_controller_phandle_seen = false;
                interrupt_controller_phandle = 0;
                interrupt_cells_seen = false;
                interrupt_cells = 0;
            }
            continue;
        }
        if (event.type == FdtEventType::end_node)
        {
            if (event.depth == interrupt_controller_depth)
            {
                finish_interrupt_controller();
                interrupt_controller_depth = -1;
            }
            if (event.depth == cpu_depth)
            {
                finish_cpu();
                cpu_depth = -1;
            }
            if (event.depth == cpus_depth)
            {
                cpus_depth = -1;
            }
            continue;
        }

        if (event.depth == cpus_depth &&
            strcmp(event.property, "#address-cells") == 0)
        {
            if (event.length == sizeof(uint32))
            {
                cpus_address_cells = static_cast<int>(read_be32(event.value));
            }
            else
            {
                controller_table_valid = false;
            }
            continue;
        }
        if (event.depth == cpu_depth)
        {
            if (strcmp(event.property, "device_type") == 0)
            {
                cpu_device_type_matches = event.length >= 3 &&
                                          memcmp(event.value, "cpu", 3) == 0;
            }
            else if (strcmp(event.property, "status") == 0)
            {
                cpu_enabled = fdt_status_is_enabled(event.value, event.length);
            }
            else if (strcmp(event.property, "reg") == 0)
            {
                cpu_reg = event.value;
                cpu_reg_length = event.length;
            }
            continue;
        }
        if (event.depth != interrupt_controller_depth)
        {
            continue;
        }

        if (strcmp(event.property, "interrupt-controller") == 0)
        {
            interrupt_controller_marker = true;
            interrupt_controller_valid = interrupt_controller_valid &&
                                         event.length == 0;
        }
        else if (strcmp(event.property, "#interrupt-cells") == 0)
        {
            if (interrupt_cells_seen || event.length != sizeof(uint32))
            {
                interrupt_controller_valid = false;
            }
            else
            {
                interrupt_cells_seen = true;
                interrupt_cells = read_be32(event.value);
            }
        }
        else if (strcmp(event.property, "phandle") == 0 ||
                 strcmp(event.property, "linux,phandle") == 0)
        {
            if (event.length != sizeof(uint32))
            {
                interrupt_controller_valid = false;
            }
            else
            {
                const uint32 phandle = read_be32(event.value);
                if (interrupt_controller_phandle_seen &&
                    interrupt_controller_phandle != phandle)
                {
                    interrupt_controller_valid = false;
                }
                interrupt_controller_phandle_seen = true;
                interrupt_controller_phandle = phandle;
            }
        }
    }

    if (!cpu_walker.valid() || !controller_table_valid || controller_count == 0)
    {
        boardPrintfError("[DTB] invalid CPU interrupt-controller phandle table\n");
        return 0;
    }

    // 第二遍按 reg+ranges 的物理坐标准确定位当前画像指定的 PLIC；只看节点名
    // 或 unit-address 会在存在桥接 ranges 时绑定到错误控制器。
    FdtAddressFrame frames[k_max_address_translation_depth]{};
    bool depth_overflow = false;
    bool plic_found = false;
    bool plic_ambiguous = false;
    bool plic_candidate_malformed = false;
    const uint8 *interrupts_extended = nullptr;
    uint32 interrupts_extended_length = 0;

    auto inspect_plic_candidate = [&](int depth) {
        FdtAddressFrame &node = frames[depth];
        if (!node.enabled || !node.interrupt_controller || node.reg == nullptr ||
            depth <= 0)
        {
            return;
        }

        const FdtAddressFrame &parent = frames[depth - 1];
        const uint32 entry_cells = parent.child_address_cells +
                                   parent.child_size_cells;
        const uint32 entry_bytes = entry_cells * sizeof(uint32);
        if (!node.cell_layout_valid || !parent.cell_layout_valid ||
            parent.child_address_cells == 0 ||
            parent.child_address_cells > 2 || parent.child_size_cells > 2 ||
            entry_cells == 0 || (node.reg_length & 3U) != 0 ||
            node.reg_length < entry_bytes || node.reg_length % entry_bytes != 0)
        {
            plic_candidate_malformed = true;
            return;
        }

        for (uint32 offset = 0; offset < node.reg_length; offset += entry_bytes)
        {
            uint64 bus_address = 0;
            if (!read_fdt_cells(node.reg + offset,
                                static_cast<int>(parent.child_address_cells),
                                bus_address))
            {
                plic_candidate_malformed = true;
                return;
            }
            uint64 physical_address = 0;
            const FdtAddressResult translation = translate_reg_address(
                frames, depth, bus_address, physical_address);
            if (translation == FdtAddressResult::malformed ||
                translation == FdtAddressResult::unsupported)
            {
                plic_candidate_malformed = true;
                continue;
            }
            if (translation != FdtAddressResult::success ||
                physical_address != plic_address)
            {
                continue;
            }
            if (plic_found)
            {
                plic_ambiguous = true;
                continue;
            }
            plic_found = true;
            interrupts_extended = node.interrupts_extended;
            interrupts_extended_length = node.interrupts_extended_length;
        }
    };

    FdtWalker plic_walker(cursor);
    while (plic_walker.next(event))
    {
        if (event.type == FdtEventType::begin_node)
        {
            if (event.depth >= k_max_address_translation_depth)
            {
                depth_overflow = true;
                continue;
            }
            frames[event.depth] = {};
            frames[event.depth].enabled =
                event.depth == 0 || frames[event.depth - 1].enabled;
            continue;
        }
        if (event.type == FdtEventType::end_node)
        {
            if (event.depth < k_max_address_translation_depth)
            {
                inspect_plic_candidate(event.depth);
            }
            continue;
        }
        if (event.depth >= k_max_address_translation_depth)
        {
            continue;
        }

        FdtAddressFrame &node = frames[event.depth];
        if (strcmp(event.property, "#address-cells") == 0)
        {
            if (event.length != sizeof(uint32))
            {
                node.cell_layout_valid = false;
            }
            else
            {
                node.child_address_cells = read_be32(event.value);
            }
        }
        else if (strcmp(event.property, "#size-cells") == 0)
        {
            if (event.length != sizeof(uint32))
            {
                node.cell_layout_valid = false;
            }
            else
            {
                node.child_size_cells = read_be32(event.value);
            }
        }
        else if (strcmp(event.property, "ranges") == 0)
        {
            if (node.ranges_present)
            {
                node.cell_layout_valid = false;
            }
            node.ranges_present = true;
            node.ranges = event.value;
            node.ranges_length = event.length;
        }
        else if (strcmp(event.property, "reg") == 0)
        {
            if (node.reg != nullptr)
            {
                node.cell_layout_valid = false;
            }
            node.reg = event.value;
            node.reg_length = event.length;
        }
        else if (strcmp(event.property, "status") == 0)
        {
            node.enabled = node.enabled &&
                           fdt_status_is_enabled(event.value, event.length);
        }
        else if (strcmp(event.property, "interrupt-controller") == 0)
        {
            node.interrupt_controller = event.length == 0;
            node.cell_layout_valid = node.cell_layout_valid && event.length == 0;
        }
        else if (strcmp(event.property, "interrupts-extended") == 0)
        {
            if (node.interrupts_extended != nullptr)
            {
                node.cell_layout_valid = false;
            }
            node.interrupts_extended = event.value;
            node.interrupts_extended_length = event.length;
        }
    }

    if (!plic_walker.valid() || depth_overflow || plic_ambiguous || !plic_found ||
        plic_candidate_malformed || interrupts_extended == nullptr ||
        interrupts_extended_length == 0 ||
        (interrupts_extended_length & 3U) != 0)
    {
        boardPrintfError("[DTB] PLIC reg/interrupts-extended is missing or malformed\n");
        return 0;
    }

    int context_count = 0;
    uint32 byte_offset = 0;
    uint32 raw_context = 0;
    while (byte_offset < interrupts_extended_length)
    {
        if (interrupts_extended_length - byte_offset < sizeof(uint32))
        {
            return 0;
        }
        const uint32 phandle = read_be32(interrupts_extended + byte_offset);
        const FdtCpuInterruptController *controller = nullptr;
        for (int index = 0; index < controller_count; ++index)
        {
            if (controllers[index].phandle == phandle)
            {
                controller = &controllers[index];
                break;
            }
        }
        // RISC-V CPU interrupt-controller 必须使用单个 interrupt cause cell。
        // 若 phandle 未知，就连下一个元组从哪里开始都无法确定，必须整体失败。
        if (controller == nullptr || controller->interrupt_cells != 1)
        {
            boardPrintfError("[DTB] PLIC references unknown/unsupported interrupt controller phandle=%u\n",
                             phandle);
            return 0;
        }

        const uint32 tuple_bytes =
            (1U + controller->interrupt_cells) * sizeof(uint32);
        if (tuple_bytes > interrupts_extended_length - byte_offset)
        {
            boardPrintfError("[DTB] truncated PLIC interrupts-extended tuple\n");
            return 0;
        }
        const uint32 interrupt_cause =
            read_be32(interrupts_extended + byte_offset + sizeof(uint32));
        if (interrupt_cause == 9 && controller->cpu_enabled)
        {
            for (int index = 0; index < context_count; ++index)
            {
                if (contexts[index].hartid == controller->hartid)
                {
                    boardPrintfError("[DTB] duplicate S-mode PLIC context for hart=%lu\n",
                                     controller->hartid);
                    return 0;
                }
            }
            if (context_count >= max_contexts)
            {
                boardPrintfError("[DTB] PLIC context table exceeds caller capacity=%d\n",
                                 max_contexts);
                return 0;
            }
            contexts[context_count++] = {
                .hartid = controller->hartid,
                .context_id = raw_context,
            };
        }

        byte_offset += tuple_bytes;
        ++raw_context;
    }

    return context_count;
}

bool DtbManager::get_mac_address(uint64 device_address, uint8 mac[6])
{
    if (mac == nullptr)
    {
        return false;
    }

    FdtCursor cursor{};
    if (!load_fdt_cursor(_dtb_addr, cursor))
    {
        return false;
    }

    FdtAddressFrame frames[k_max_address_translation_depth]{};
    bool depth_overflow = false;
    bool malformed_candidate = false;
    bool unsupported_candidate = false;
    bool result_found = false;
    bool result_ambiguous = false;
    uint8 result_mac[6]{};

    auto inspect_candidate = [&](int depth) {
        FdtAddressFrame &node = frames[depth];
        if (!node.enabled || !node.mac_valid || node.reg == nullptr || depth <= 0)
        {
            return;
        }
        if (!node.cell_layout_valid)
        {
            malformed_candidate = true;
            return;
        }

        const FdtAddressFrame &parent = frames[depth - 1];
        if (!parent.cell_layout_valid || parent.child_address_cells == 0 ||
            parent.child_address_cells > 2 || parent.child_size_cells > 2)
        {
            unsupported_candidate = unsupported_candidate ||
                                    parent.cell_layout_valid;
            malformed_candidate = malformed_candidate ||
                                  !parent.cell_layout_valid;
            return;
        }

        const uint32 entry_cells = parent.child_address_cells +
                                   parent.child_size_cells;
        const uint32 entry_bytes = entry_cells * sizeof(uint32);
        if (entry_cells == 0 || (node.reg_length & 3U) != 0 ||
            node.reg_length < entry_bytes || node.reg_length % entry_bytes != 0)
        {
            malformed_candidate = true;
            return;
        }

        for (uint32 offset = 0; offset < node.reg_length; offset += entry_bytes)
        {
            uint64 bus_address = 0;
            if (!read_fdt_cells(node.reg + offset,
                                static_cast<int>(parent.child_address_cells),
                                bus_address))
            {
                malformed_candidate = true;
                return;
            }

            uint64 physical_address = 0;
            const FdtAddressResult translation = translate_reg_address(
                frames, depth, bus_address, physical_address);
            if (translation == FdtAddressResult::malformed)
            {
                malformed_candidate = true;
                continue;
            }
            if (translation == FdtAddressResult::unsupported)
            {
                unsupported_candidate = true;
                continue;
            }
            if (translation != FdtAddressResult::success ||
                physical_address != device_address)
            {
                continue;
            }

            if (result_found)
            {
                // 两个节点映射到同一物理设备时无法确定哪个 MAC 才权威。
                result_ambiguous = true;
                continue;
            }
            memmove(result_mac, node.mac, sizeof(result_mac));
            result_found = true;
        }
    };

    FdtWalker walker(cursor);
    FdtEvent event{};
    while (walker.next(event))
    {
        if (event.type == FdtEventType::begin_node)
        {
            if (event.depth >= k_max_address_translation_depth)
            {
                depth_overflow = true;
                continue;
            }
            frames[event.depth] = {};
            frames[event.depth].enabled =
                event.depth == 0 || frames[event.depth - 1].enabled;
            continue;
        }
        if (event.type == FdtEventType::end_node)
        {
            if (event.depth < k_max_address_translation_depth)
            {
                inspect_candidate(event.depth);
            }
            continue;
        }
        if (event.depth >= k_max_address_translation_depth)
        {
            continue;
        }

        FdtAddressFrame &node = frames[event.depth];
        if (strcmp(event.property, "#address-cells") == 0)
        {
            if (event.length != sizeof(uint32))
            {
                node.cell_layout_valid = false;
            }
            else
            {
                node.child_address_cells = read_be32(event.value);
            }
            continue;
        }
        if (strcmp(event.property, "#size-cells") == 0)
        {
            if (event.length != sizeof(uint32))
            {
                node.cell_layout_valid = false;
            }
            else
            {
                node.child_size_cells = read_be32(event.value);
            }
            continue;
        }
        if (strcmp(event.property, "ranges") == 0)
        {
            if (node.ranges_present)
            {
                node.cell_layout_valid = false;
            }
            node.ranges_present = true;
            node.ranges = event.value;
            node.ranges_length = event.length;
            continue;
        }
        if (strcmp(event.property, "reg") == 0)
        {
            if (node.reg != nullptr)
            {
                node.cell_layout_valid = false;
            }
            node.reg = event.value;
            node.reg_length = event.length;
            continue;
        }
        if (strcmp(event.property, "status") == 0)
        {
            node.enabled = node.enabled &&
                           fdt_status_is_enabled(event.value, event.length);
        }
        else if (strcmp(event.property, "local-mac-address") == 0 ||
                 strcmp(event.property, "mac-address") == 0)
        {
            uint8 property_mac[6]{};
            const bool is_local =
                strcmp(event.property, "local-mac-address") == 0;
            if (valid_mac_property(event.value, event.length, property_mac) &&
                (is_local || !node.mac_is_local))
            {
                memmove(node.mac, property_mac, sizeof(node.mac));
                node.mac_valid = true;
                node.mac_is_local = is_local;
            }
        }
    }

    if (!walker.valid())
    {
        boardPrintfError("[DTB] MAC lookup stopped on malformed structure block\n");
        return false;
    }
    if (depth_overflow)
    {
        boardPrintfError("[DTB] MAC lookup exceeds maximum node depth %d\n",
                         k_max_address_translation_depth);
        return false;
    }
    if (result_ambiguous)
    {
        boardPrintfError("[DTB] multiple MAC nodes map to physical 0x%lx\n",
                         device_address);
        return false;
    }
    if (!result_found && malformed_candidate)
    {
        boardPrintfError("[DTB] malformed reg/ranges while looking up MAC at 0x%lx\n",
                         device_address);
    }
    else if (!result_found && unsupported_candidate)
    {
        boardPrintfError("[DTB] unsupported address-cell layout while looking up MAC at 0x%lx\n",
                         device_address);
    }
    if (result_found)
    {
        memmove(mac, result_mac, sizeof(result_mac));
    }
    return result_found;
}

bool DtbManager::get_initrd(uint64& start, uint64& end) {
    start = 0;
    end = 0;

    FdtCursor cursor{};
    if (!load_fdt_cursor(_dtb_addr, cursor))
    {
        return false;
    }

    int chosen_depth = -1;
    FdtWalker walker(cursor);
    FdtEvent event{};
    while (walker.next(event))
    {
        if (event.type == FdtEventType::begin_node && event.depth == 1 &&
            strcmp(event.name, "chosen") == 0)
        {
            chosen_depth = event.depth;
            continue;
        }
        if (event.type == FdtEventType::end_node && event.depth == chosen_depth)
        {
            chosen_depth = -1;
            continue;
        }
        if (event.type != FdtEventType::property || event.depth != chosen_depth)
        {
            continue;
        }

        uint64 value = 0;
        if (event.length == 4)
        {
            value = read_be32(event.value);
        }
        else if (event.length == 8)
        {
            value = read_be64(event.value);
        }
        else
        {
            continue;
        }
        if (strcmp(event.property, "linux,initrd-start") == 0)
        {
            start = value;
        }
        else if (strcmp(event.property, "linux,initrd-end") == 0)
        {
            end = value;
        }
    }

    return walker.valid() && start != 0 && end > start;
}

void DtbManager::initialize_boot_dtb(uint64 dtb_addr) {
    auto check_dtb = [&](uint64 p, const char **failure_reason = nullptr) -> bool {
        if (failure_reason != nullptr)
        {
            *failure_reason = nullptr;
        }
        if (p == 0)
        {
            if (failure_reason != nullptr)
            {
                *failure_reason = "address is zero";
            }
            return false;
        }
        if (p % 8 != 0)
        {
            if (failure_reason != nullptr)
            {
                *failure_reason = "address is not 8-byte aligned";
            }
            return false;
        }
        FdtCursor cursor{};
        const uint64 kernel_address =
            platform::memory::kernel_access_address(p);
        if (!load_fdt_cursor(kernel_address, cursor, failure_reason))
        {
            return false;
        }
        if (!validate_fdt_structure(cursor))
        {
            if (failure_reason != nullptr)
            {
                *failure_reason = "malformed structure tokens/nesting/property";
            }
            return false;
        }
        return true;
    };
    uint64 final_dtb = 0;
    const char *dtb_failure_reason = nullptr;

    if (check_dtb(dtb_addr, &dtb_failure_reason)) {
        boardPrintfInfo("[DTB] firmware address valid: 0x%lx\n", dtb_addr);
        final_dtb = dtb_addr;
    } else {
        // DTB 是内存、CPU 与设备资源的权威输入。盲扫 RAM 既可能触碰 MMIO
        // 空洞，也会把损坏的启动契约伪装成“偶尔能启动”，所有平台统一失败。
        panic("[DTB] invalid firmware DTB: address=0x%lx reason=%s",
              dtb_addr,
              dtb_failure_reason != nullptr ? dtb_failure_reason : "unknown");
    }

    // 对外统一保存物理地址。固件可能传物理地址，也可能传平台直映别名；
    // 若把别名交给 PMM，DTB 页面将无法从物理 RAM 区间中排除。
    k_dtb_addr = platform::memory::physical_address(final_dtb);
    DtbManager::init(k_dtb_addr);

    boardPrintfInfo("[DTB] blob ready: physical=0x%lx size=%lu bytes\n",
                    k_dtb_addr, DtbManager::get_dtb_size());

    uint64 described_initrd_start = 0;
    uint64 described_initrd_end = 0;
    if (DtbManager::get_initrd(described_initrd_start, described_initrd_end))
    {
        k_initrd_start = described_initrd_start;
        k_initrd_end = described_initrd_end;
        boardPrintfInfo("[DTB] firmware initrd: 0x%lx-0x%lx\n",
                        k_initrd_start, k_initrd_end);
        printfMagenta("[DTB] Using firmware-described initrd 0x%lx-0x%lx\n",
                      k_initrd_start, k_initrd_end);
        return;
    }

    // initrd 必须由固件通过 /chosen 明确声明。扫描普通 RAM 猜测 ext4/CPIO
    // 既无法可靠确定镜像边界，也可能误认内核数据；缺省路径统一使用主块设备。
    k_initrd_start = 0;
    k_initrd_end = 0;
    boardPrintfInfo("[DTB] no initrd in DTB; root source=platform block backend\n");
}
