#include "dtb.hh"
#include "libs/klib.hh"
#include "printer.hh"

uint64 DtbManager::_dtb_addr = 0;

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
            // printfWhite("Node: %s\n", name);
            if (depth == 1 && strcmp(name, "chosen") == 0) {
                in_chosen = true;
                chosen_depth = depth;
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
            
            if (in_chosen) {
                // printfWhite("Prop: %s, len: %d\n", prop_name, len);
                if (strcmp(prop_name, "linux,initrd-start") == 0) {
                    if (len == 4) start = bswap32(*(uint32*)prop_val);
                    else if (len == 8) start = bswap64(*(uint64*)prop_val);
                } else if (strcmp(prop_name, "linux,initrd-end") == 0) {
                    if (len == 4) end = bswap32(*(uint32*)prop_val);
                    else if (len == 8) end = bswap64(*(uint64*)prop_val);
                }
            }
        } else if (token == FDT_NOP) {
            continue;
        }
    }
    
    if (start != 0 && end != 0) return true;
    return false;
}
