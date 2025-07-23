#pragma once

#include "fs/vfs/file/file.hh"
#include <EASTL/string.h>
#include <EASTL/unique_ptr.h>

namespace mem
{
    class UserspaceStream;
}

namespace fs
{
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

        /// @brief 向虚拟文件写入数据（仅支持可写的虚拟文件）
        /// @param buf 要写入的数据缓冲区的地址
        /// @param len 要写入的数据长度（以字节为单位）
        /// @param off off=-1 表示不指定偏移使用文件内部偏移量
        /// @param upgrade 如果 upgrade 为 true，写完后文件指针自动后移
        /// @return 实际写入的字节数，若发生错误则返回负值表示错误码
        virtual long write(uint64 buf, size_t len, long off = -1, bool upgrade = true) override;
        
        virtual bool read_ready() override;
        virtual bool write_ready() override;
        virtual off_t lseek(off_t offset, int whence) override;
        virtual eastl::string read_symlink_target() override;

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
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfExeProvider>();
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

    // /etc/passwd 内容提供者
    class EtcPasswdProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<EtcPasswdProvider>();
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
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<DevLoopProvider>(_loop_number);
        }
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
    
    // /proc/self/stat 内容提供者
    class ProcSelfStatProvider : public VirtualContentProvider
    {
    public:
        virtual eastl::string generate_content() override;
        virtual bool is_dynamic() const override { return true; } // 进程状态需要实时更新
        virtual eastl::unique_ptr<VirtualContentProvider> clone() const override {
            return eastl::make_unique<ProcSelfStatProvider>();
        }
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

}
