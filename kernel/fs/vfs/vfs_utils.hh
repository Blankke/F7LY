#include <EASTL/string.h>
#include "fs/vfs/ops.hh"
#include "fs/vfs/file/file.hh"
int vfs_openat(eastl::string absolute_path, fs::file* &file, uint flags);
int vfs_is_dir(eastl::string &absolute_path);
int vfs_path2filetype(eastl::string &absolute_path);
int create_and_write_file(const char *path, const char *data);
int is_file_exist(const char *path);
uint vfs_read_file(const char *path, uint64 buffer_addr, size_t offset, size_t size);
int vfs_fstat(fs::file *f, fs::Kstat *st);