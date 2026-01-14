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
    _dtb_addr = dtb_addr;
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
