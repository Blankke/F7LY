#include <EASTL/string.h>
#include "fs/vfs/ops.hh"
#include "fs/vfs/file/file.hh"

// 将flags转换为可读的字符串表示
eastl::string flags_to_string(uint flags);

int vfs_openat(eastl::string absolute_path, fs::file* &file, uint flags, int mode);
int vfs_is_dir(eastl::string &absolute_path);
int vfs_path2filetype(eastl::string &absolute_path);
int create_and_write_file(const char *path, const char *data);
int vfs_is_file_exist(const char *path);
uint vfs_read_file(const char *path, uint64 buffer_addr, size_t offset, size_t size);
int vfs_fstat(fs::file *f, fs::Kstat *st);
int vfs_getdents(fs::file *const file, struct linux_dirent64 *dirp, uint count);
int vfs_mkdir(const char *path, uint64_t mode);
int vfs_frename(const char *oldpath, const char *newpath);
int vfs_truncate(fs::file *f, size_t length);
int vfs_chmod(eastl::string pathname, mode_t mode);
int vfs_fallocate(fs::file *f,off_t offset, size_t length);
int vfs_free_file(struct fs::file *file);
int vfs_copy_file_range(int f_in,off_t offset_in, int f_out,off_t offset_out,size_t size,uint flags);