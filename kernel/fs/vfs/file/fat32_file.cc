#include "fs/vfs/file/fat32_file.hh"
#include "fs/fat32/fat32.hh"
#include "mem/userspace_stream.hh"

namespace fs {

fat32_file::fat32_file(FileAttrs attrs, eastl::string path, struct fat32_entry *entry) 
    : file(attrs, path) {
    fat_info.entry = entry;
    // Assume entry is already ref-counted (caller called edup or lookup)
}

fat32_file::~fat32_file() {
    if (fat_info.entry) {
        eput(fat_info.entry);
    }
}

long fat32_file::read(uint64 buf, size_t len, long off, bool upgrade) {
    if (off < 0) off = _file_ptr;
    
    // eread takes user_dst. 0 = kernel address.
    // buf comes from syscall_handler which copies user data to kernel buffer first.
    int n = eread(fat_info.entry, 0, buf, off, len); 
    
    if (n > 0 && upgrade) {
        _file_ptr = off + n;
    }
    return n;
}

long fat32_file::write(uint64 buf, size_t len, long off, bool upgrade) {
    if (off < 0) off = _file_ptr;
    
    // ewrite takes user_src. 0 = kernel address.
    int n = ewrite(fat_info.entry, 0, buf, off, len); 

    if (n > 0 && upgrade) {
        _file_ptr = off + n;
        if ((uint64)_file_ptr > _stat.size) _stat.size = _file_ptr;
    }
    return n;
}

bool fat32_file::read_ready() { return true; }
bool fat32_file::write_ready() { return true; }

size_t fat32_file::read_sub_dir(ubuf &dst) {
    // Placeholder
    return 0;
}

off_t fat32_file::lseek(off_t offset, int whence) {
    long new_off = 0;
    if (whence == 0) // SEEK_SET
        new_off = offset;
    else if (whence == 1) // SEEK_CUR
        new_off = _file_ptr + offset;
    else if (whence == 2) // SEEK_END
        new_off = fat_info.entry->file_size + offset;
    
    if (new_off < 0) return -1;
    _file_ptr = new_off;
    return _file_ptr;
}

}
