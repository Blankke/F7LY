#include <EASTL/string.h>
#include "fs/vfs/ops.hh"
#include "fs/vfs/file/file.hh"
int vfs_openat(eastl::string absolute_path, fs::file *file, uint flags);
int vfs_is_dir(eastl::string &absolute_path);