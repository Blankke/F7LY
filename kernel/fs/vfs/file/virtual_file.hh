#pragma once

#include "fs/vfs/file/file.hh"
#include <EASTL/string.h>
#include <EASTL/unique_ptr.h>
#include "proc/proc.hh"

namespace mem
{
    class UserspaceStream;
}

namespace fs
{
    // 虚拟文件提供者类型枚举
    enum class VirtualProviderType
    {
        GENERIC,
        DEV_ZERO,
        DEV_NULL,
        DEV_URANDOM,
        PROC_SELF_EXE,
        PROC_MEMINFO,
        // 可以根据需要添加更多类型
    };

    // 虚拟文件内容提供者的抽象基类
    class VirtualContentProvider
    {
    public:
        virtual ~VirtualContentProvider() = default;
        
        // 生成虚拟文件内容
        virtual eastl::string generate_content() = 0;
        virtual eastl::string read_symlink_target();

        // 克隆方法，用于创建当前provider的副本
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const = 0;

        // 是否支持写入
        virtual bool is_writable() const { return false; }
        
        // 是否需要动态更新内容（不缓存）
        virtual bool is_dynamic() const { return false; }
        
        // 处理写入操作（对于支持写入的虚拟文件）
        virtual long handle_write(uint64 buf, size_t len, long off) { return -1; }

        // 获取提供者类型
        virtual VirtualProviderType get_provider_type() const { return VirtualProviderType::GENERIC; }

        virtual bool is_readable() const { return false; }
        virtual bool get_time_namespace_snapshot(time_namespace_snapshot &snapshot) const
        {
            (void)snapshot;
            return false;
        }

        // @brief 这个设计很狗屎，handle_read本来应该跟 generate_content类似的，但是content这玩意儿首先它常驻内存，比较狗屎。
        // content设计之初只是为了一些非常简单的，内容很少的虚拟文件，甚至返回值都是eastl::string。
        // 现在增加handle_read是为了真正的实现虚拟文件的读取操作，比如/dev/loopX
        //
        // 总之，content这个设计太屎山了，一时难以重构，引入read过渡一下。
        virtual long handle_read(uint64 buf, size_t len, long off) { return -1; }
        virtual bool has_read_size() const { return false; }
        virtual uint64 read_size() const { return 0; }
    };

    class virtual_file : public file
    {
    private:
        eastl::unique_ptr<VirtualContentProvider> _content_provider;
        eastl::string _cached_content;  // 缓存虚拟文件内容
        bool _content_cached;           // 是否已缓存内容
        
        // 确保内容已缓存
        void ensure_content_cached();

    public:
        virtual_file() = default;
        
        // 使用内容提供者构造虚拟文件
        virtual_file(FileAttrs attrs, eastl::string path, eastl::unique_ptr<VirtualContentProvider> provider) 
            : file(attrs, path), _content_provider(eastl::move(provider)), _content_cached(false)
        {
            dup();
            new(&_stat) Kstat(attrs.filetype);
            is_virtual = true;
        }
        
        ~virtual_file() = default;

        /// @brief 从虚拟文件中读取数据到指定缓冲区
        /// @param buf 目标缓冲区的地址，用于存放读取到的数据
        /// @param len 需要读取的数据长度（字节数）
        /// @param off off=-1 表示不指定偏移使用文件内部偏移量
        /// @param upgrade 如果 upgrade 为 true，文件指针自动后移
        /// @return 实际读取的字节数，若发生错误则返回负值表示错误码
        virtual long read(uint64 buf, size_t len, long off = -1, bool upgrade = true) override;
        virtual long read_to_user(mem::PageTable &pt, uint64 user_buf, size_t len, long off = -1, bool upgrade = true) override;

        /// @brief 向虚拟文件写入数据（仅支持可写的虚拟文件）
        /// @param buf 要写入的数据缓冲区的地址
        /// @param len 要写入的数据长度（以字节为单位）
        /// @param off off=-1 表示不指定偏移使用文件内部偏移量
        /// @param upgrade 如果 upgrade 为 true，写完后文件指针自动后移
        /// @return 实际写入的字节数，若发生错误则返回负值表示错误码
        virtual long write(uint64 buf, size_t len, long off = -1, bool upgrade = true) override;
        virtual long write_from_user(mem::PageTable &pt, uint64 user_buf, size_t len, long off = -1, bool upgrade = true) override;
        
        virtual bool read_ready() override;
        virtual bool write_ready() override;
        virtual off_t lseek(off_t offset, int whence) override;
        virtual eastl::string read_symlink_target() override;
        virtual bool get_time_namespace_snapshot(time_namespace_snapshot &snapshot) const override;

        using ubuf = mem::UserspaceStream;
        virtual size_t read_sub_dir(ubuf &dst) override;
        
        // 清除缓存内容（用于需要动态更新的虚拟文件）
        void clear_cache() { _content_cached = false; _cached_content.clear(); }
        
        // 检查路径是否为虚拟文件路径
        static bool is_virtual_path(const eastl::string& path);
    };

    // ======================== 具体的内容提供者实现 ========================
    
    // /proc/self/exe 内容提供者
    class ProcSelfExeProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::string read_symlink_target() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfExeProvider>();
        }
    };

    // 固定内容文件提供者，用于 sysfs/procfs 中简单常量节点。
    class StaticContentProvider : public VirtualContentProvider
    {
    private:
        eastl::string _content;

    public:
        explicit StaticContentProvider(const eastl::string &content) : _content(content) {}
        virtual eastl::string generate_content() override { return _content; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<StaticContentProvider>(_content);
        }
    };

    // /proc/meminfo 内容提供者
    class ProcMeminfoProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 内存信息需要实时更新
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcMeminfoProvider>();
        }
    };

    // /proc/cpuinfo 内容提供者
    class ProcCpuinfoProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcCpuinfoProvider>();
        }
    };

    // /proc/version 内容提供者
    class ProcVersionProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcVersionProvider>();
        }
    };

    class KernelConfigProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<KernelConfigProvider>();
        }
    };

    // /proc/mounts 内容提供者
    class ProcMountsProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 挂载信息可能变化
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcMountsProvider>();
        }
    };

    // /proc/self/fd/X 内容提供者
    class ProcSelfFdProvider : public VirtualContentProvider
    {
    private:
        int _fd_num;
    public:
        ProcSelfFdProvider(int fd_num) : _fd_num(fd_num) {}
        virtual eastl::string generate_content() override;
        virtual eastl::string read_symlink_target() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfFdProvider>(_fd_num);
        }
    };

    // /proc/<pid>/fdinfo/<fd> 内容提供者
    class ProcPidFdinfoProvider : public VirtualContentProvider
    {
    private:
        int _pid;
        int _fd_num;

    public:
        ProcPidFdinfoProvider(int pid, int fd_num) : _pid(pid), _fd_num(fd_num) {}
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcPidFdinfoProvider>(_pid, _fd_num);
        }
    };

    class ProcSelfTimeNamespaceProvider : public VirtualContentProvider
    {
    private:
        time_namespace_snapshot _snapshot;
        bool _for_children = false;

    public:
        ProcSelfTimeNamespaceProvider(bool for_children = false,
                                      time_namespace_snapshot snapshot = {})
            : _snapshot(snapshot), _for_children(for_children) {}
        virtual eastl::string generate_content() override;
        virtual bool get_time_namespace_snapshot(time_namespace_snapshot &snapshot) const override
        {
            snapshot = _snapshot;
            return true;
        }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfTimeNamespaceProvider>(_for_children, _snapshot);
        }
    };

    class ProcSelfTimensOffsetsProvider : public VirtualContentProvider
    {
    private:
        eastl::string _write_buffer;

    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; }
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfTimensOffsetsProvider>();
        }
    };

    // /etc/passwd 内容提供者
    class EtcPasswdProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<EtcPasswdProvider>();
        }
    };

    // /etc/group 内容提供者
    class EtcGroupProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<EtcGroupProvider>();
        }
    };

    // /etc/hosts 内容提供者
    class EtcHostsProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<EtcHostsProvider>();
        }
    };

    // /etc/protocols 内容提供者
    class EtcProtocolsProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<EtcProtocolsProvider>();
        }
    };

    // /dev/block/X:Y 内容提供者
    class DevBlockProvider : public VirtualContentProvider
    {
    private:
        int _major;
        int _minor;
    public:
        DevBlockProvider(int major, int minor) : _major(major), _minor(minor) {}
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; }
        virtual bool is_readable() const override { return true; }
        virtual long handle_read(uint64 buf, size_t len, long off) override;
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual bool has_read_size() const override { return true; }
        virtual uint64 read_size() const override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<DevBlockProvider>(_major, _minor);
        }
    };

    // /dev/loop 设备提供者（具体的 loop 设备，如 loop0, loop1 等）
    class DevLoopProvider : public VirtualContentProvider
    {
    private:
        int _loop_number;
        
    public:
        DevLoopProvider(int loop_number) : _loop_number(loop_number) {}
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<DevLoopProvider>(_loop_number);
        }
        virtual bool is_readable() const override { return true; }
        virtual long handle_read(uint64 buf, size_t len, long off) override;
        virtual bool has_read_size() const override { return true; }
        virtual uint64 read_size() const override;
    };

    // /dev/loop-control 控制设备提供者
    class DevLoopControlProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; } // 支持 ioctl 操作
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<DevLoopControlProvider>();
        }
    };
    
    // /proc/sys/fs/pipe-user-pages-soft 内容提供者
    class ProcSysFsPipeUserPagesSoftProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; } // 允许写入
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysFsPipeUserPagesSoftProvider>();
        }
    };

    class ProcSysFsPipeMaxSizeProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysFsPipeMaxSizeProvider>();
        }
    };

    class ProcSysFsLeaseBreakTimeProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysFsLeaseBreakTimeProvider>();
        }
    };

    class ProcSysFsInotifyMaxQueuedEventsProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysFsInotifyMaxQueuedEventsProvider>();
        }
    };

    class ProcSysFsInotifyMaxUserInstancesProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysFsInotifyMaxUserInstancesProvider>();
        }
    };

    class ProcSysNetIpv4TagProvider : public VirtualContentProvider
    {
    private:
        bool _is_default = false;

    public:
        explicit ProcSysNetIpv4TagProvider(bool is_default) : _is_default(is_default) {}
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; }
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysNetIpv4TagProvider>(_is_default);
        }
    };

    // /proc/sys/kernel/pid_max 内容提供者
    class ProcSysKernelPidMaxProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return false; } // 静态内容
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysKernelPidMaxProvider>();
        }
    };

    class ProcSysKernelRandomEntropyAvailProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysKernelRandomEntropyAvailProvider>();
        }
    };

    // 通用的 /proc/<pid>/stat 内容提供者
    class ProcPidStatProvider : public VirtualContentProvider
    {
    private:
        int target_pid; // 目标进程PID，-1表示使用当前进程(self)
        
    public:
        ProcPidStatProvider(int pid = -1) : target_pid(pid) {}
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 进程状态需要实时更新
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcPidStatProvider>(target_pid);
        }
        
        // 生成标准Linux /proc/[pid]/stat格式的内容
        eastl::string generate_stat_content(proc::Pcb* pcb);
    };

    // /proc/interrupts 内容提供者
    class ProcInterruptsProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 中断统计需要实时更新
        virtual bool is_writable() const override { return false; } // 只读文件
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcInterruptsProvider>();
        }
    };

    // /dev/zero 内容提供者
    class DevZeroProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 每次读取都生成新内容
        virtual bool is_writable() const override { return true; } // 支持写入（丢弃所有数据）
        virtual long handle_write(uint64 buf, size_t len, long off) override; // 处理写入操作
        virtual VirtualProviderType get_provider_type() const override { return VirtualProviderType::DEV_ZERO; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<DevZeroProvider>();
        }
    };

    // /dev/null 内容提供者
    class DevNullProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 每次读取都生成新内容
        virtual bool is_writable() const override { return true; } // 支持写入（丢弃所有数据）
        virtual long handle_write(uint64 buf, size_t len, long off) override; // 处理写入操作
        virtual VirtualProviderType get_provider_type() const override { return VirtualProviderType::DEV_NULL; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<DevNullProvider>();
        }
    };

    // /dev/urandom 内容提供者
    class DevUrandomProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; }
        virtual bool is_writable() const override { return true; } // Linux 允许写入熵池；这里按丢弃处理
        virtual bool is_readable() const override { return true; }
        virtual long handle_read(uint64 buf, size_t len, long off) override;
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual VirtualProviderType get_provider_type() const override { return VirtualProviderType::DEV_URANDOM; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<DevUrandomProvider>();
        }
    };

    // /proc/self/maps 内容提供者
    class ProcSelfMapsProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 内存映射信息需要实时更新
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfMapsProvider>();
        }
    };

    // /proc/self/pagemap 内容提供者  
    class ProcSelfPagemapProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_readable() const override { return true; }
        virtual long handle_read(uint64 buf, size_t len, long off) override;
        virtual bool is_dynamic() const override { return true; } // 页面映射信息需要实时更新
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfPagemapProvider>();
        }
    };

    // /proc/self/status 内容提供者
    class ProcSelfStatusProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 进程状态信息需要实时更新
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfStatusProvider>();
        }
    };
    // /proc/sys/kernel/shmmax
    class ProcSysKernelShmmaxProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return true; }
        virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysKernelShmmaxProvider>();
        }
    };
    // /proc/sysvipc/shm
    class ProcSysvipcShmProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysvipcShmProvider>();
        }
    };
        // /proc/sys/kernel/shmmni
    class ProcSysKernelShmmniProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return false; } // 允许写入
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysKernelShmmniProvider>();
        }
    };
    // /proc/sys/kernel/shmall
    class ProcSysKernelShmallProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return false; } // 允许写入
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysKernelShmallProvider>();
        }
    };
    
    // /proc/sys/kernel/tainted
    class ProcSysKernelTaintedProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_writable() const override { return false; } // 只读
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSysKernelTaintedProvider>();
        }
    };

    // /etc/ld.so.preload 内容提供者（通常为空，表示不预加载任何库）
    class EtcLdSoPreloadProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
    virtual bool is_dynamic() const override { return false; }
    virtual bool is_writable() const override { return true; }
    virtual long handle_write(uint64 buf, size_t len, long off) override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<EtcLdSoPreloadProvider>();
        }
    };

    // /etc/ld.so.cache 内容提供者（提供一个空或最小可接受的内容，动态链接器将回退到目录扫描）
    class EtcLdSoCacheProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return false; }
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<EtcLdSoCacheProvider>();
        }
    };

    // /proc/stat 系统统计信息提供者
    class ProcStatProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 系统统计需要实时更新
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcStatProvider>();
        }
    };
}
