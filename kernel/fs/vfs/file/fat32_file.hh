#pragma once
#include "fs/vfs/file/file.hh"
#include "fs/fat32/fat32_inode.hh"

namespace fs {
    class fat32_file : public file {
    public:
         struct vfs_fat32_inode_info fat_info;

         fat32_file(FileAttrs attrs, eastl::string path, struct fat32_entry *entry);
         ~fat32_file();
         long read(uint64 buf, size_t len, long off, bool upgrade) override;
         long write(uint64 buf, size_t len, long off, bool upgrade) override;
         
         bool read_ready() override;
         bool write_ready() override;
         size_t read_sub_dir(ubuf &dst) override;
         off_t lseek(off_t offset, int whence) override;
    };
}
