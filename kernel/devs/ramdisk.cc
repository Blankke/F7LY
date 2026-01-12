#include "ramdisk.hh"
#include "libs/klib.hh"

namespace dev {

RamDisk::RamDisk(uint64 start_addr, uint64 size) 
    : _start_addr(start_addr), _size(size) {
}

long RamDisk::get_block_size() {
    return 512;
}

int RamDisk::read_blocks_sync(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) {
    uint64 offset = (uint64)start_block * 512;
    
    char* src = (char*)(_start_addr + offset);
    
    for (int i = 0; i < buf_count; ++i) {
        if (offset + buf_list[i].buf_size > _size) {
            return -1; // Out of bounds
        }
        memcpy((void*)buf_list[i].buf_addr, src, buf_list[i].buf_size);
        src += buf_list[i].buf_size;
        offset += buf_list[i].buf_size;
    }
    return 0;
}

int RamDisk::read_blocks(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) {
    return read_blocks_sync(start_block, block_count, buf_list, buf_count);
}

int RamDisk::write_blocks_sync(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) {
    uint64 offset = (uint64)start_block * 512;
    char* dst = (char*)(_start_addr + offset);
    
    for (int i = 0; i < buf_count; ++i) {
        if (offset + buf_list[i].buf_size > _size) {
            return -1; // Out of bounds
        }
        memcpy(dst, (void*)buf_list[i].buf_addr, buf_list[i].buf_size);
        dst += buf_list[i].buf_size;
        offset += buf_list[i].buf_size;
    }
    return 0;
}

int RamDisk::write_blocks(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) {
    return write_blocks_sync(start_block, block_count, buf_list, buf_count);
}

int RamDisk::handle_intr() {
    return 0;
}

}
