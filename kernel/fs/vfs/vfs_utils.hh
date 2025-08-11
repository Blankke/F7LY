#ifndef VFS_UTILS_HH
#define VFS_UTILS_HH

#include <EASTL/string.h>
#include "fs/vfs/ops.hh"
#include "fs/vfs/file/file.hh"

// 将flags转换为可读的字符串表示
eastl::string flags_to_string(uint flags);

// 路径规范化函数：处理 . 和 ..
eastl::string normalize_path(const eastl::string &path);

int vfs_openat(eastl::string absolute_path, fs::file* &file, uint flags, int mode);
int vfs_is_dir(eastl::string &absolute_path);
int vfs_path2filetype(eastl::string &absolute_path);
int create_and_write_file(const char *path, const char *data);
int vfs_is_file_exist(const char *path);
uint vfs_read_file(const char *path, uint64 buffer_addr, size_t offset, size_t size);
int vfs_write_file(const char *path, uint64 buffer_addr, size_t offset, size_t size);
int vfs_fstat(fs::file *f, fs::Kstat *st);
int vfs_path_stat(const char *path, fs::Kstat *st, bool follow_symlinks = true);
int vfs_getdents(fs::file *const file, struct linux_dirent64 *dirp, uint count);
int vfs_mkdir(const char *path, uint64_t mode);
int vfs_frename(const char *oldpath, const char *newpath);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_truncate(fs::file *f, size_t length);
int vfs_chmod(eastl::string pathname, mode_t mode);
// Change owner/group for a path. If follow_symlinks is true, operate on the target of symlink
// else operate on the link itself (lchown-like). Returns 0 on success, negative errno on error.
int vfs_chown(const eastl::string &pathname, int owner, int group, bool follow_symlinks);
// Get owner/group for a path. follow_symlinks controls final symlink resolution.
int vfs_owner_get(const eastl::string &pathname, uint32_t &uid, uint32_t &gid, bool follow_symlinks);
// Get file mode for a path. follow_symlinks controls final symlink resolution.
int vfs_mode_get(const eastl::string &pathname, uint32_t &mode, bool follow_symlinks);
// Set file mode for a path. follow_symlinks controls final symlink resolution.
int vfs_mode_set(const eastl::string &pathname, uint32_t mode, bool follow_symlinks);
int vfs_fallocate(fs::file *f,off_t offset, size_t length);
int vfs_free_file(struct fs::file *file);
int vfs_copy_file_range(int f_in,off_t offset_in, int f_out,off_t offset_out,size_t size,uint flags);
bool is_lock_conflict(const struct flock &existing_lock, const struct flock &new_lock);
bool check_file_lock_access(const struct flock &file_lock, off_t offset, size_t size, bool is_write);

#endif // VFS_UTILS_HH