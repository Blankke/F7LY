#include "dtb.hh"
#include "libs/klib.hh"
#include "printer.hh"
#include "platform.hh"

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

    // LoongArch 下 QEMU 传进来的 DTB 地址是物理地址；内核后续既可能在启用分页前访问，
    // 也可能在启用分页后再次解析 DTB。为了让这两种时机都能稳定访问，
    // 这里统一把 DTB 规范化为 DMWIN 直映地址保存到 _dtb_addr；
    // k_dtb_addr 仍继续保留物理地址，给页表映射等场景使用。
    static uint64 normalize_dtb_addr_for_kernel(uint64 dtb_addr)
    {
#ifdef LOONGARCH
        if (dtb_addr != 0 && (dtb_addr & VIRT_DMWIN_MASK) == 0)
        {
            return dtb_addr | DMWIN_MASK;
        }
#endif
        return dtb_addr;
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

    static bool load_fdt_cursor(uint64 dtb_addr, FdtCursor &cursor)
    {
        if (dtb_addr == 0)
        {
            return false;
        }

        const auto *header = reinterpret_cast<const fdt_header *>(dtb_addr);
        if (read_be32(&header->magic) != FDT_MAGIC)
        {
            return false;
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
        if (total_size < sizeof(fdt_header) || total_size > k_max_fdt_size ||
            version < 17 || last_compatible_version > 17 ||
            (off_struct & 3U) != 0 || (off_reservation_map & 7U) != 0 ||
            !blob_range_valid(total_size, off_struct, struct_size) ||
            !blob_range_valid(total_size, off_strings, strings_size) ||
            !blob_range_valid(total_size, off_reservation_map, 16) ||
            struct_size < sizeof(uint32))
        {
            return false;
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

    int target_depth = -1;
    bool target_enabled = true;
    bool candidate_valid = false;
    bool candidate_is_local = false;
    uint8 candidate[6]{};

    FdtWalker walker(cursor);
    FdtEvent event{};
    while (walker.next(event))
    {
        if (event.type == FdtEventType::begin_node)
        {
            uint64 unit_address = 0;
            if (target_depth < 0 && parse_node_unit_address(event.name, unit_address) &&
                unit_address == device_address)
            {
                target_depth = event.depth;
                target_enabled = true;
                candidate_valid = false;
                candidate_is_local = false;
            }
            continue;
        }
        if (event.type == FdtEventType::end_node)
        {
            if (target_depth == event.depth)
            {
                if (target_enabled && candidate_valid)
                {
                    memmove(mac, candidate, sizeof(candidate));
                    return true;
                }
                target_depth = -1;
            }
            continue;
        }
        if (target_depth < 0 || event.depth != target_depth)
        {
            continue;
        }
        if (strcmp(event.property, "status") == 0)
        {
            target_enabled = event.length >= 2 && memcmp(event.value, "ok", 2) == 0;
        }
        else if (event.length >= sizeof(candidate) &&
                 (strcmp(event.property, "local-mac-address") == 0 ||
                  (!candidate_is_local && strcmp(event.property, "mac-address") == 0)))
        {
            uint8 property_mac[6]{};
            memmove(property_mac, event.value, sizeof(property_mac));
            bool property_valid = property_mac[0] != 0xff &&
                                  (property_mac[0] & 1U) == 0;
            bool any_nonzero = false;
            for (uint32 index = 0; index < sizeof(property_mac); ++index)
            {
                any_nonzero = any_nonzero || property_mac[index] != 0;
            }
            property_valid = property_valid && any_nonzero;
            if (property_valid)
            {
                memmove(candidate, property_mac, sizeof(candidate));
                candidate_valid = true;
                candidate_is_local = strcmp(event.property, "local-mac-address") == 0;
            }
        }
    }
    return false;
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

void DtbManager::find_dtb_and_initrd(uint64 dtb_addr, uint64 kernel_end_phys) {
    #ifdef LOONGARCH
    uint64 conv_base = 0x9000000000000000UL;
    #else
    uint64 conv_base = 0; // RISC-V usually direct map or identical or handled by VMM
    #endif

    auto check_dtb = [&](uint64 p) -> bool {
        if (p == 0 || p % 8 != 0)
        {
            return false;
        }
        FdtCursor cursor{};
        const uint64 kernel_address = p | conv_base;
        return load_fdt_cursor(kernel_address, cursor) && validate_fdt_structure(cursor);
    };
    
    // helper to parse hex
    auto parse_hex8 = [&](volatile char* p) -> uint64 {
        uint64 v = 0;
        for(int i=0; i<8; i++) {
            char c = p[i];
            int d = 0;
            if(c>='0' && c<='9') d = c-'0';
            else if(c>='a' && c<='f') d = c-'a'+10;
            else if(c>='A' && c<='F') d = c-'A'+10;
            v = (v << 4) | d;
        }
        return v;
    };

    uint64 final_dtb = 0;

    if (check_dtb(dtb_addr)) {
        printfMagenta("[DTB] Received Valid DTB at 0x%lx\n", dtb_addr);
        final_dtb = dtb_addr;
    } else {
#ifdef BOARD_LS2K1000
        // 实机地址空间包含大量 MMIO 空洞，不能像 QEMU 一样盲扫前 256MiB。
        // 固件参数中的 DTB 不完整时立即终止，避免在错误地址上继续解析内存。
        panic("[DTB] LS2K1000 firmware supplied an invalid DTB at 0x%lx", dtb_addr);
#else
        printfMagenta("[DTB] Received Invalid DTB at 0x%lx (Magic wrong or align). Scanning RAM...\n", dtb_addr);
        // Scan 0 to 256MB
        for (uint64 p = 0; p < 0x10000000; p += 0x1000) { // 4KB steps
            if (check_dtb(p)) {
                printfYellow("[DTB] Found FDT at Physical 0x%lx\n", p);
                final_dtb = p;
                break;
            }
        }
        if (final_dtb == 0) {
            printfMagenta("[DTB] FDT NOT FOUND in first 256MB of RAM! System may halt.\n");
            // Try 0x200000 (standard load offset)?
            if (check_dtb(0x200000)) { final_dtb = 0x200000; printfYellow("[DTB] Found at 0x200000\n"); }
        }
#endif
    }

    if (final_dtb != 0) {
#ifdef LOONGARCH
        // 对外统一保存物理地址。U-Boot/固件既可能传物理地址，也可能传
        // cached DMW 别名；若把后者直接交给 PMM，DTB 页面将无法从物理
        // RAM 区间中排除，随后可能被页分配器覆盖。
        k_dtb_addr = VIRT2PHY(final_dtb);
#else
        k_dtb_addr = final_dtb;
#endif
        DtbManager::init(k_dtb_addr);
    } else {
#ifdef LOONGARCH
        k_dtb_addr = VIRT2PHY(dtb_addr);
#else
        k_dtb_addr = dtb_addr; // Fallback
#endif
        DtbManager::init(k_dtb_addr);
    }

    uint64 described_initrd_start = 0;
    uint64 described_initrd_end = 0;
    if (DtbManager::get_initrd(described_initrd_start, described_initrd_end))
    {
        k_initrd_start = described_initrd_start;
        k_initrd_end = described_initrd_end;
        printfMagenta("[DTB] Using firmware-described initrd 0x%lx-0x%lx\n",
                      k_initrd_start, k_initrd_end);
        return;
    }

#ifdef BOARD_LS2K1000
    // 实机内存布局必须以 DTB reserved-memory/chosen 为权威。扫描一段“看起来像
    // RAM”的地址寻找 ext4 magic 既慢，也可能误认普通数据或触碰固件保留区。
    k_initrd_start = 0;
    k_initrd_end = 0;
    printfMagenta("[DTB] LS2K1000 DTB 未声明 initrd，使用 SATA 根文件系统\n");
    return;
#endif

    // Align to 4K
    if (kernel_end_phys % 0x1000) kernel_end_phys = (kernel_end_phys + 0x1000) & ~0xFFFUL;

    if (kernel_end_phys < 0x200000) kernel_end_phys = 0x1000000; // safety

    printfMagenta("[DTB] Scanning for Initrd (EXT4/CPIO) from 0x%lx...\n", kernel_end_phys);
    bool found_initrd = false;
    // Scan up to 128MB (0x08000000)
    for (uint64 p = kernel_end_phys; p < 0x08000000; p += 0x1000) { 
         uint64 v = p | conv_base;
         
         // Check EXT4: Magic 0xEF53 at offset 0x438 (1080)
         // Superblock starts at 1024. Magic is at 1024 + 0x38 = 1080 = 0x438
         volatile uint16 *ext4_magic = (volatile uint16 *)(v + 0x438);
         if (*ext4_magic == 0xEF53) {
             printfYellow("[DTB] Found EXT4 Initrd at 0x%lx\n", p);
             volatile uint32 *s_log_block_size = (volatile uint32 *)(v + 1024 + 0x18);
             volatile uint32 *s_blocks_count = (volatile uint32 *)(v + 1024 + 0x4);
             
             uint32 block_size = 1024 << (*s_log_block_size);
             uint64 total_size = (uint64)(*s_blocks_count) * block_size;
             
             printfYellow("       Size: %ld bytes (Blocks: %d, BSize: %d)\n", total_size, *s_blocks_count, block_size);
             
             k_initrd_start = p;
             k_initrd_end = p + total_size;
             found_initrd = true;
             break;
         }
         
         // Check CPIO: "070701" at offset 0
         volatile char *cpio = (volatile char*)(v);
         if (cpio[0]=='0' && cpio[1]=='7' && cpio[2]=='0' && cpio[3]=='7' && cpio[4]=='0' && cpio[5]=='1') {
             printfYellow("[DTB] Found CPIO Initrd at 0x%lx\n", p);
             k_initrd_start = p;
             // Try to parse parsing... hex at offset 54
             uint64 filesize = parse_hex8(cpio + 54);
             
             if (filesize == 0) filesize = 32*1024*1024; // Fallback
             k_initrd_end = p + filesize; 
             found_initrd = true;
             break;
         }
    }
    if (!found_initrd) {
        printfRed("[DTB] Initrd NOT FOUND in scanning.\n");
    }
}
