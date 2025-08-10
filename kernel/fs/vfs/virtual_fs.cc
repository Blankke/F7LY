#include "virtual_fs.hh"
#include "fs/vfs/file/virtual_file.hh"
#include "fs/vfs/vfs_utils.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
namespace fs
{
    // 构造函数
    VirtualFileSystem::VirtualFileSystem()
    {
    }

    // 析构函数
    VirtualFileSystem::~VirtualFileSystem()
    {
        destroy_tree(root);
    }

    // 销毁树结构
    void VirtualFileSystem::destroy_tree(vfile_tree_node *node)
    {
        if (node)
        {
            delete node; // node的析构函数会递归删除所有子节点
        }
    }

    // 根据路径查找节点
    vfile_tree_node *VirtualFileSystem::find_node_by_path(const eastl::string &path) const
    {
        if (path.empty() || path == "/")
        {
            return root;
        }

        eastl::vector<eastl::string> path_parts = path_split(path);
        vfile_tree_node *current = root;

        for (const auto &part : path_parts)
        {
            current = current->find_child(part);
            if (!current)
            {
                return nullptr;
            }
        }

        return current;
    }

    // 创建路径上的所有节点
    vfile_tree_node *VirtualFileSystem::create_path_nodes(const eastl::string &path)
    {
        if (path.empty() || path == "/")
        {
            return root;
        }

        eastl::vector<eastl::string> path_parts = path_split(path);
        vfile_tree_node *current = root;

        for (size_t i = 0; i < path_parts.size(); i++)
        {
            const auto &part = path_parts[i];
            vfile_tree_node *child = current->find_child(part);
            if (!child)
            {
                // 创建新节点
                child = new vfile_tree_node(part);

                // 如果不是最后一个部分，则设置为目录类型
                // 只有最后一个部分会在 add_virtual_file 中设置具体的文件类型
                if (i < path_parts.size() - 1)
                {
                    child->file_type = fs::FileTypes::FT_DIRECT; // 目录类型
                    child->provider = nullptr;                   // 目录不需要provider
                }

                if (!current->add_child(child))
                {
                    delete child;
                    return nullptr; // 添加失败，可能超过了最大子节点数
                }
            }
            else
            {
                // 如果节点已存在且不是最后一个部分，确保它是目录类型
                if (i < path_parts.size() - 1 && child->file_type == 0)
                {
                    child->file_type = fs::FileTypes::FT_DIRECT; // 确保中间节点是目录类型
                }
            }
            current = child;
        }

        return current;
    }

    // 添加虚拟文件
    bool VirtualFileSystem::add_virtual_file(const eastl::string &path, int file_type,
                                             eastl::unique_ptr<VirtualContentProvider> provider)
    {
        // printf("Adding virtual file: %s\n", path.c_str());
        vfile_tree_node *node = create_path_nodes(path);
        if (!node)
        {
            return false;
        }

        node->file_type = file_type;
        node->provider = eastl::move(provider);
        return true;
    }

    // 删除虚拟文件
    bool VirtualFileSystem::remove_virtual_file(const eastl::string &path)
    {
        if (path.empty() || path == "/")
        {
            return false; // 不能删除根节点
        }

        // 找到父节点和要删除的节点名称
        size_t last_slash = path.rfind('/');
        if (last_slash == eastl::string::npos)
        {
            return false;
        }

        eastl::string parent_path = path.substr(0, last_slash);
        eastl::string node_name = path.substr(last_slash + 1);

        if (parent_path.empty())
        {
            parent_path = "/";
        }

        vfile_tree_node *parent = find_node_by_path(parent_path);
        if (!parent)
        {
            return false;
        }

        return parent->remove_child(node_name);
    }

    // 检查路径是否为虚拟路径
    bool VirtualFileSystem::is_virtual_path(const eastl::string &path) const
    {
        vfile_tree_node *node = find_node_by_path(path);
        return node && node->provider != nullptr;
    }

    // 获取虚拟节点
    vfile_tree_node *VirtualFileSystem::get_virtual_node(const eastl::string &path) const
    {
        return find_node_by_path(path);
    }

    // 列出目录下的虚拟文件
    void VirtualFileSystem::list_virtual_files(const eastl::string &dir_path,
                                               eastl::vector<eastl::string> &file_list) const
    {
        vfile_tree_node *dir_node = find_node_by_path(dir_path);
        if (!dir_node)
        {
            return;
        }

        file_list.clear();
        for (int i = 0; i < dir_node->children_count; i++)
        {
            if (dir_node->children[i])
            {
                file_list.push_back(dir_node->children[i]->name);
            }
        }
    }

    // 打印树结构（调试用）
    void VirtualFileSystem::print_tree(vfile_tree_node *node, int depth, const eastl::string &prefix) const
    {
        if (!node)
        {
            node = root;
            printf("/\n");
        }

        for (int i = 0; i < node->children_count; i++)
        {
            if (node->children[i])
            {
                // 判断是否是最后一个子节点
                bool is_last = (i == node->children_count - 1);

                // 打印当前节点的前缀和名称
                printf("%s", prefix.c_str());
                printf(is_last ? "└── " : "├── ");
                printf("%s", node->children[i]->name.c_str());

                // 显示文件类型
                if (node->children[i]->file_type == fs::FileTypes::FT_DIRECT)
                {
                    printf(" [DIR]");
                }
                else if (node->children[i]->file_type == fs::FileTypes::FT_NORMAL)
                {
                    printf(" [FILE]");
                }
                else if (node->children[i]->file_type == fs::FileTypes::FT_SYMLINK)
                {
                    printf(" [LINK]");
                }
                else if (node->children[i]->file_type == fs::FileTypes::FT_DEVICE)
                {
                    printf(" [DEV]");
                }
                else if (node->children[i]->file_type == fs::FileTypes::FT_PIPE)
                {
                    printf(" [PIPE]");
                }

                // 显示是否有provider
                if (node->children[i]->provider)
                {
                    printf(" [provider]");
                }
                printf("\n");

                // 递归打印子节点
                eastl::string new_prefix = prefix + (is_last ? "    " : "│   ");
                print_tree(node->children[i], depth + 1, new_prefix);
            }
        }
    }
    bool VirtualFileSystem::is_filepath_virtual(const eastl::string &path) const
    {
        // 使用新的树形结构方法
        return is_virtual_path(path);
    }

    void VirtualFileSystem::dir_init()
    {
        printf("Initializing virtual file system tree structure\n");
        root = new vfile_tree_node("");             // 根节点名称为空
        root->file_type = fs::FileTypes::FT_DIRECT; // 根节点是目录
        // 使用新的树形结构初始化虚拟文件
        // /proc/self/exe
        add_virtual_file("/proc/self/exe", fs::FileTypes::FT_SYMLINK,
                         eastl::make_unique<ProcSelfExeProvider>());

        // /proc/meminfo
        add_virtual_file("/proc/meminfo", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcMeminfoProvider>());

        // /proc/cpuinfo
        add_virtual_file("/proc/cpuinfo", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcCpuinfoProvider>());

        // /proc/version
        add_virtual_file("/proc/version", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcVersionProvider>());

        // /proc/mounts
        add_virtual_file("/proc/mounts", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcMountsProvider>());

        // /proc/sys/kernel/pid_max
        add_virtual_file("/proc/sys/kernel/pid_max", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSysKernelPidMaxProvider>());

        // 注意：/proc/self/cmdline, /proc/stat, /proc/uptime 等需要相应的 Provider 实现
        // 这里先创建节点，但 provider 为 nullptr，可以后续添加
        add_virtual_file("/proc/self/cmdline", fs::FileTypes::FT_NORMAL, nullptr);
        add_virtual_file("/proc/stat", fs::FileTypes::FT_NORMAL, nullptr);
        add_virtual_file("/proc/uptime", fs::FileTypes::FT_NORMAL, nullptr);

        // 添加 /proc/self/stat 文件及其提供者 (使用新的统一provider)
        add_virtual_file("/proc/self/stat", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcPidStatProvider>(-1)); // -1表示当前进程

        // 添加 /proc/self/maps 文件及其提供者
        add_virtual_file("/proc/self/maps", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSelfMapsProvider>());

        // 添加 /proc/self/pagemap 文件及其提供者
        add_virtual_file("/proc/self/pagemap", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSelfPagemapProvider>());

        // 添加 /proc/self/status 文件及其提供者
        add_virtual_file("/proc/self/status", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSelfStatusProvider>());

        // ======================== Loop 设备节点 ========================
        // 添加 /dev/loop-control 控制设备
        add_virtual_file("/dev/loop-control", fs::FileTypes::FT_DEVICE,
                         eastl::make_unique<DevLoopControlProvider>());

        // 添加预定义的 loop 设备节点 (/dev/loop0 - /dev/loop7)
        for (int i = 0; i < 8; i++)
        {
            char loop_name[16] = "/dev/loop";
            loop_name[9] = '0' + i;
            loop_name[10] = '\0';
            eastl::string loop_path(loop_name);
            add_virtual_file(loop_path, fs::FileTypes::FT_DEVICE,
                             eastl::make_unique<DevLoopProvider>(i));
        }

        // 添加 /proc/interrupts 文件及其提供者
        add_virtual_file("/proc/interrupts", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcInterruptsProvider>());

        // /etc/passwd
        add_virtual_file("/etc/passwd", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<EtcPasswdProvider>());

        // /proc/sys/fs/pipe-user-pages-soft
        add_virtual_file("/proc/sys/fs/pipe-user-pages-soft", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSysFsPipeUserPagesSoftProvider>());

        // /dev/loop
        add_virtual_file("/dev/loop-control", fs::FileTypes::FT_DEVICE,
                         eastl::make_unique<DevLoopControlProvider>());

        add_virtual_file("/dev/loop0", fs::FileTypes::FT_DEVICE,
                         eastl::make_unique<DevLoopProvider>(0));

        // /dev/block/8:0 (块设备文件)
        add_virtual_file("/dev/block/8:0", fs::FileTypes::FT_DEVICE,
                         eastl::make_unique<DevBlockProvider>(8, 0));

        // /dev/zero (零设备)
        add_virtual_file("/dev/zero", fs::FileTypes::FT_DEVICE,
                         eastl::make_unique<DevZeroProvider>());

        // /dev/null (空设备)
        add_virtual_file("/dev/null", fs::FileTypes::FT_DEVICE,
                         eastl::make_unique<DevNullProvider>());

        // /proc/sys/kernel/shmmax (共享内存最大值)
        add_virtual_file("/proc/sys/kernel/shmmax", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSysKernelShmmaxProvider>());

        // /proc/sys/kernel/shmmni (共享内存最大值)
        add_virtual_file("/proc/sys/kernel/shmmni", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSysKernelShmmniProvider>());

        // /proc/sys/kernel/shmall (共享内存总大小)
        add_virtual_file("/proc/sys/kernel/shmall", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSysKernelShmallProvider>());

        // /proc/sys/kernel/tainted (内核污染状态)
        add_virtual_file("/proc/sys/kernel/tainted", fs::FileTypes::FT_NORMAL,
                         eastl::make_unique<ProcSysKernelTaintedProvider>());
                         
        // 打印树结构（调试用）
        // printf("Virtual file system tree:\n");
        // print_tree(root, 0, "");
    }

    vfile_msg VirtualFileSystem::get_vfile_msg(const eastl::string &absolute_path) const
    {
        vfile_msg result;
        result.is_virtual = false;
        result.provider = nullptr;

        // 首先尝试从树形结构中查找
        vfile_tree_node *node = find_node_by_path(absolute_path);
        if (node)
        {
            result.is_virtual = true;
            result.file_type = node->file_type;
            result.provider = node->provider ? node->provider->clone() : nullptr;
            return result; // 找到有效的provider，直接返回
            // 注意：这里我们需要克隆provider，因为原provider在树中
        }

        return result;
    }

    int VirtualFileSystem::openat(eastl::string absolute_path, fs::file *&file, uint flags, int mode)
    {
        int err;
        vfile_tree_node *node = find_node_by_path(absolute_path);
        
        // 检查是否是 /proc/<pid>/stat 格式的路径
        if (!node && is_proc_pid_stat_path(absolute_path)) {
            int pid = extract_pid_from_path(absolute_path);
            if (pid > 0) {
                // 验证进程是否存在
                proc::Pcb* target_pcb = proc::k_pm.find_proc_by_pid(pid);
                if (target_pcb) {
                    printfCyan("[open] creating dynamic /proc/%d/stat file\n", pid);
                    // 动态创建 /proc/<pid>/stat 文件
                    fs::FileAttrs attrs;
                    attrs.filetype = fs::FileTypes::FT_NORMAL;
                    attrs._value = 0644;
                    file = new virtual_file(attrs, absolute_path, 
                                          eastl::make_unique<ProcPidStatProvider>(pid));
                    return 0;
                }
            }
        }
        
        if (node)
        {
            printfCyan("[open] using virtual file system for path: %s\n", absolute_path.c_str());
            err = vfile_openat(absolute_path, file, flags);
        }
        else
        {
            err = vfs_openat(absolute_path.c_str(), file, flags, mode);
        }
        return err;
    }

    int VirtualFileSystem::vfile_openat(eastl::string absolute_path, fs::file *&file, uint flags)
    {
        vfile_msg vf_msg = get_vfile_msg(absolute_path);

        fs::FileAttrs attrs;
        attrs.filetype = (fs::FileTypes)vf_msg.file_type;
        attrs._value = 0644;
        file = new virtual_file(attrs, absolute_path, eastl::move(vf_msg.provider));
        return 0;
    }
    eastl::vector<eastl::string> VirtualFileSystem::path_split(const eastl::string &path) const
    {
        eastl::vector<eastl::string> parts;
        size_t start = 0;
        size_t end = path.find('/');

        while (end != eastl::string::npos)
        {
            if (end > start)
            { // 忽略空部分
                parts.push_back(path.substr(start, end - start));
            }
            start = end + 1; // 跳过 '/'
            end = path.find('/', start);
        }

        if (start < path.length())
        { // 添加最后一部分
            parts.push_back(path.substr(start));
        }

        return parts;
    }
    
    // 检查路径是否符合 /proc/<pid>/stat 格式
    bool VirtualFileSystem::is_proc_pid_stat_path(const eastl::string& path) const
    {
        // 检查是否以 /proc/ 开头并以 /stat 结尾
        if (path.size() < 11) return false; // 最短: /proc/1/stat (11个字符)
        if (path.substr(0, 6) != "/proc/") return false;
        if (path.substr(path.size() - 5) != "/stat") return false;
        
        // 提取中间的PID部分并验证是否为数字
        eastl::string pid_str = path.substr(6, path.size() - 11);
        if (pid_str.empty()) return false;
        
        // 检查是否全为数字
        for (size_t i = 0; i < pid_str.size(); i++) {
            if (pid_str[i] < '0' || pid_str[i] > '9') {
                return false;
            }
        }
        
        return true;
    }
    
    // 从路径中提取PID
    int VirtualFileSystem::extract_pid_from_path(const eastl::string& path) const
    {
        if (!is_proc_pid_stat_path(path)) return -1;
        
        eastl::string pid_str = path.substr(6, path.size() - 11);
        int pid = 0;
        
        // 手动转换字符串为整数
        for (size_t i = 0; i < pid_str.size(); i++) {
            if (pid_str[i] >= '0' && pid_str[i] <= '9') {
                pid = pid * 10 + (pid_str[i] - '0');
            } else {
                return -1; // 无效字符
            }
        }
        
        return pid;
    }

    int VirtualFileSystem::fstat(fs::file *f, fs::Kstat *st)
    {
        if (f->is_virtual)
        {
            // 如果是虚拟文件，使用虚拟文件系统的fstat处理
            return vfile_fstat(f, st);
        }
        else
        {
            // 调用常规的fstat处理
            printfCyan("[VirtualFileSystem] ::fstat: calling vfs_fstat for non-virtual file\n");
            return vfs_fstat(f, st);
        }
    }

    int VirtualFileSystem::vfile_fstat(fs::file *f, fs::Kstat *st)
    {
        // 获取虚拟文件的路径，用于判断具体的设备类型
        const eastl::string& path = f->_path_name;
        
        // 为不同的设备设置不同的stat信息
        if (path == "/dev/null") {
            // /dev/null 字符设备的标准属性
            st->dev = 0x5;               // Device: 5h/5d
            st->ino = 3;                 // Inode: 3 (标准/dev/null的inode)
            st->mode = 0666 | S_IFCHR;   // character special file, mode: 0666
            st->nlink = 1;               // Links: 1
            st->uid = 0;                 // Uid: 0 (root)
            st->gid = 0;                 // Gid: 0 (root)
            st->rdev = (1 << 8) | 3;     // Device type: 1,3 (标准/dev/null的设备号)
            st->size = 0;                // Size: 0
            st->blksize = 4096;          // IO Block: 4096
            st->blocks = 0;              // Blocks: 0
        }
        else if (path == "/dev/zero") {
            // /dev/zero 字符设备的标准属性
            st->dev = 0x5;               // Device: 5h/5d
            st->ino = 5;                 // Inode: 5 (标准/dev/zero的inode)
            st->mode = 0666 | S_IFCHR;   // character special file, mode: 0666
            st->nlink = 1;               // Links: 1
            st->uid = 0;                 // Uid: 0 (root)
            st->gid = 0;                 // Gid: 0 (root)
            st->rdev = (1 << 8) | 5;     // Device type: 1,5 (标准/dev/zero的设备号)
            st->size = 0;                // Size: 0
            st->blksize = 4096;          // IO Block: 4096
            st->blocks = 0;              // Blocks: 0
        }
        else if (path.find("/dev/loop") == 0) {
            // 原有的loop设备处理逻辑
            st->dev = 0x5;               // Device: 5h/5d
            st->ino = 124;               // Inode: 124
            st->mode = 0660 | S_IFBLK;   // block special file, mode: 0660 + block device
            st->nlink = 1;               // Links: 1
            st->uid = 0;                 // Uid: 0 (root)
            st->gid = 6;                 // Gid: 6 (disk)
            st->rdev = 7;                // Device type: 7,0
            st->size = 0;                // Size: 0
            st->blksize = 4096;          // IO Block: 4096
            st->blocks = 0;              // Blocks: 0
        }
        else {
            // 其他虚拟文件的默认处理
            st->dev = 0x1;               // Device: 1h/1d
            st->ino = 1;                 // Inode: 1
            st->mode = 0644 | S_IFREG;   // regular file, mode: 0644
            st->nlink = 1;               // Links: 1
            st->uid = 0;                 // Uid: 0 (root)
            st->gid = 0;                 // Gid: 0 (root)
            st->rdev = 0;                // Device type: 0
            st->size = 0;                // Size: 0 (动态文件大小在读取时确定)
            st->blksize = 4096;          // IO Block: 4096
            st->blocks = 0;              // Blocks: 0
        }

        // 设置时间戳（所有虚拟文件使用相同的时间戳）
        st->st_atime_sec = 1753278126; // Access: 2025-07-23 19:02:06
        st->st_atime_nsec = 192843346; // Access nsec
        st->st_ctime_sec = 1753278126; // Change: 2025-07-23 19:02:06
        st->st_ctime_nsec = 176778624; // Change nsec
        st->st_mtime_sec = 1753278126; // Modify: 2025-07-23 19:02:06
        st->st_mtime_nsec = 176778624; // Modify nsec
        
        return 0;
    }

    VirtualFileSystem k_vfs;
}