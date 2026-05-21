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

static inline uint32 bswap32(uint32 x);
static inline uint64 bswap64(uint64 x);

namespace
{
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
        char *struct_base = nullptr;
        char *strings_base = nullptr;
        uint32 struct_size = 0;
    };

    static bool load_fdt_cursor(uint64 dtb_addr, FdtCursor &cursor)
    {
        if (dtb_addr == 0)
        {
            return false;
        }

        fdt_header *hdr = (fdt_header *)dtb_addr;
        if (bswap32(hdr->magic) != FDT_MAGIC)
        {
            return false;
        }

        uint32 off_struct = bswap32(hdr->off_dt_struct);
        uint32 off_strings = bswap32(hdr->off_dt_strings);

        cursor.struct_base = (char *)dtb_addr + off_struct;
        cursor.strings_base = (char *)dtb_addr + off_strings;
        cursor.struct_size = bswap32(hdr->size_dt_struct);
        return true;
    }

    static uint32 read_fdt_u32(char *p)
    {
        return bswap32(*(uint32 *)p);
    }

    static uint64 read_fdt_cells(const char *data, int cells)
    {
        uint64 value = 0;
        for (int i = 0; i < cells; ++i)
        {
            value = (value << 32) | bswap32(*(const uint32 *)(data + i * 4));
        }
        return value;
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
            value = (value << 4) | digit;
        }

        addr = value;
        return true;
    }
} // namespace

static inline uint32 bswap32(uint32 x) {
    return ((x << 24) & 0xff000000) |
           ((x << 8) & 0x00ff0000) |
           ((x >> 8) & 0x0000ff00) |
           ((x >> 24) & 0x000000ff);
}

static inline uint64 bswap64(uint64 x) {
    uint32 hi = x >> 32;
    uint32 lo = x & 0xffffffff;
    return ((uint64)bswap32(lo) << 32) | bswap32(hi);
}

void DtbManager::init(uint64 dtb_addr) {
    _dtb_addr = normalize_dtb_addr_for_kernel(dtb_addr);
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

    char *p = cursor.struct_base;
    char *struct_end = cursor.struct_base + cursor.struct_size;
    int depth = 0;
    int root_addr_cells = 2;
    int root_size_cells = 2;
    int region_count = 0;

    bool in_memory_node = false;
    int memory_depth = -1;
    bool memory_device_type_ok = false;
    uint64 memory_node_addr = 0;
    bool memory_node_addr_valid = false;

    while (p < struct_end)
    {
        while (((uint64)p % 4) != 0)
        {
            ++p;
        }

        uint32 token = read_fdt_u32(p);
        p += 4;

        if (token == FDT_END)
        {
            break;
        }

        if (token == FDT_BEGIN_NODE)
        {
            char *name = p;
            p += strlen(name) + 1;

            if (depth == 0)
            {
                in_memory_node = false;
                memory_depth = -1;
                memory_device_type_ok = false;
                memory_node_addr = 0;
                memory_node_addr_valid = false;
            }
            else if (depth == 1)
            {
                in_memory_node = strncmp(name, "memory", 6) == 0;
                memory_depth = in_memory_node ? depth : -1;
                memory_device_type_ok = !in_memory_node;
                memory_node_addr = 0;
                memory_node_addr_valid = parse_node_unit_address(name, memory_node_addr);
            }

            ++depth;
            continue;
        }

        if (token == FDT_END_NODE)
        {
            --depth;
            if (memory_depth == depth)
            {
                in_memory_node = false;
                memory_depth = -1;
                memory_device_type_ok = false;
                memory_node_addr = 0;
                memory_node_addr_valid = false;
            }
            continue;
        }

        if (token == FDT_NOP)
        {
            continue;
        }

        if (token != FDT_PROP)
        {
            break;
        }

        uint32 len = read_fdt_u32(p);
        p += 4;
        uint32 nameoff = read_fdt_u32(p);
        p += 4;

        char *prop_name = cursor.strings_base + nameoff;
        char *prop_val = p;
        p += len;

        if (depth == 1)
        {
            if (strcmp(prop_name, "#address-cells") == 0 && len >= 4)
            {
                root_addr_cells = (int)read_fdt_u32(prop_val);
            }
            else if (strcmp(prop_name, "#size-cells") == 0 && len >= 4)
            {
                root_size_cells = (int)read_fdt_u32(prop_val);
            }
        }

        if (!in_memory_node)
        {
            continue;
        }

        if (strcmp(prop_name, "device_type") == 0)
        {
            memory_device_type_ok = strcmp(prop_val, "memory") == 0;
            continue;
        }

        if (!memory_device_type_ok || strcmp(prop_name, "reg") != 0)
        {
            continue;
        }

        int entry_cells = root_addr_cells + root_size_cells;
        int total_cells = (int)len / 4;
        if (entry_cells <= 0 || total_cells < entry_cells)
        {
            continue;
        }

        for (int cell_index = 0; cell_index + entry_cells <= total_cells; cell_index += entry_cells)
        {
            uint64 base = read_fdt_cells(prop_val + cell_index * 4, root_addr_cells);
            uint64 size = read_fdt_cells(prop_val + (cell_index + root_addr_cells) * 4, root_size_cells);

            // QEMU LoongArch virt 的 memory 节点名称仍然使用真实的 32-bit 物理基址，
            // 但 reg 高 32 位会带一个并不参与当前内核物理寻址的标记值。
            // 如果直接把 64-bit 拼接值当地址，会得到与实际执行完全不一致的假地址，
            // 从而误判内存不连续。这里在“节点名地址”和 reg 低 32 位一致时，
            // 退回到节点名给出的真实物理基址，并同样按低 32 位读取 size。
            if (memory_node_addr_valid &&
                base > 0xFFFFFFFFULL &&
                (base & 0xFFFFFFFFULL) == memory_node_addr)
            {
                base = memory_node_addr;
                size = size & 0xFFFFFFFFULL;
            }

            if (base == 0 && memory_node_addr_valid && memory_node_addr != 0)
            {
                base = memory_node_addr;
            }

            if (size == 0)
            {
                continue;
            }

            if (region_count < max_regions)
            {
                regions[region_count].base = base;
                regions[region_count].size = size;
            }
            ++region_count;
        }
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

bool DtbManager::get_initrd(uint64& start, uint64& end) {
    if (!_dtb_addr) {
        // printfRed("[DTB] Not initialized!\n");
        return false;
    }
    
    fdt_header* hdr = (fdt_header*)_dtb_addr;
    if (bswap32(hdr->magic) != FDT_MAGIC) {
        printfRed("[DTB] Bad magic: %x\n", bswap32(hdr->magic));
        return false;
    }
    printfYellow("DtbManager::get_initrd called\n");
    
    uint32 off_struct = bswap32(hdr->off_dt_struct);
    uint32 off_strings = bswap32(hdr->off_dt_strings);
    
    char* struct_base = (char*)_dtb_addr + off_struct;
    char* strings_base = (char*)_dtb_addr + off_strings;
    
    char* p = struct_base;
    bool in_chosen = false;
    
    start = 0;
    end = 0;

    int depth = 0;
    int chosen_depth = -1;

    while(true) {
        // Alignment
        while (((uint64)p % 4) != 0) p++;
        
        uint32 token = bswap32(*(uint32*)p);
        p += 4;
        
        if (token == FDT_END) break;
        
        if (token == FDT_BEGIN_NODE) {
            char* name = p;
            p += strlen(name) + 1;
            // printfWhite("Node: %s (depth %d)\n", name, depth);
            if (depth == 1 && strcmp(name, "chosen") == 0) {
                in_chosen = true;
                chosen_depth = depth;
                // printfYellow("[DTB] Found /chosen node\n");
            } 
            depth++;
        } else if (token == FDT_END_NODE) {
            depth--;
            if (chosen_depth == depth) {
                in_chosen = false;
                chosen_depth = -1;
            }
        } else if (token == FDT_PROP) {
            uint32 len = bswap32(*(uint32*)p);
            p += 4;
            uint32 nameoff = bswap32(*(uint32*)p);
            p += 4;
            
            char* prop_name = strings_base + nameoff;
            char* prop_val = p;
            p += len;
            
            // printfWhite("  Prop: %s, len: %d\n", prop_name, len);

            if (in_chosen) {
                // printfWhite("[DTB] /chosen prop: %s, len: %d\n", prop_name, len);
                if (strcmp(prop_name, "linux,initrd-start") == 0) {
                    if (len == 4) start = bswap32(*(uint32*)prop_val);
                    else if (len == 8) start = bswap64(*(uint64*)prop_val);
                    // printfYellow("[DTB] initrd-start: 0x%lx\n", start);
                } else if (strcmp(prop_name, "linux,initrd-end") == 0) {
                    if (len == 4) end = bswap32(*(uint32*)prop_val);
                    else if (len == 8) end = bswap64(*(uint64*)prop_val);
                    // printfYellow("[DTB] initrd-end: 0x%lx\n", end);
                }
            }
        } else if (token == FDT_NOP) {
            continue;
        }
    }
    
    if (start != 0 && end != 0) return true;
    return false;
}

void DtbManager::find_dtb_and_initrd(uint64 dtb_addr, uint64 kernel_end_phys) {
    #ifdef LOONGARCH
    uint64 conv_base = 0x9000000000000000UL;
    #else
    uint64 conv_base = 0; // RISC-V usually direct map or identical or handled by VMM
    #endif

    auto check_dtb = [&](uint64 p) -> bool {
        if (p % 8 != 0) return false;
        volatile unsigned int *ptr = (volatile unsigned int *)(p | conv_base);
        // FDT Magic 0xd00dfeed (Big Endian) -> 0xedfe0dd0 (Little Endian)
        return *ptr == 0xedfe0dd0;
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
    }

    if (final_dtb != 0) {
        k_dtb_addr = final_dtb;
        DtbManager::init(k_dtb_addr);
    } else {
        k_dtb_addr = dtb_addr; // Fallback
        DtbManager::init(k_dtb_addr);
    }

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
