#include "fs/vfs/file/virtual_file.hh"
// #include "fs/lwext4/ext4_errno.hh"
// #include "fs/lwext4/ext4.hh"
// #include "mem/userspace_stream.hh"
#include "proc_manager.hh"
#include "proc/meminfo.hh"
#include "proc/cpuinfo.hh"
#include "printer.hh"
#include "proc/proc.hh"
#define min(a, b) ((a) < (b) ? (a) : (b))

namespace fs
{
    // ======================== VirtualContentProvider 基类实现 ========================
    
    eastl::string VirtualContentProvider::read_symlink_target()
    {
        panic("虚类占位实现");
        // 默认实现：不支持符号链接
        return "";
    }

    // ======================== 具体内容提供者的实现 ========================
    
    eastl::string ProcSelfExeProvider::generate_content()
    {
        // /proc/self/exe 返回当前进程的可执行文件路径
        eastl::string exe_path = proc::k_pm.get_cur_pcb()->_cwd_name + "busybox";
        return exe_path;
    }

    eastl::string ProcMeminfoProvider::generate_content()
    {
        // panic("TODO");
        // return 0;
        return get_meminfo();
    }

    eastl::string ProcCpuinfoProvider::generate_content()
    {
        // panic("TODO");
        // return 0;
        return get_cpuinfo();
    }

    eastl::string ProcVersionProvider::generate_content()
    {
        return "Linux version 5.15.0-F7LY (F7LY) (gcc version 11.2.0) #1 SMP PREEMPT\n";
    }

    eastl::string ProcMountsProvider::generate_content()
    {
        eastl::string result;
        result += "/dev/sda1 / ext4 rw,relatime 0 0\n";
        result += "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n";
        result += "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n";
        return result;
    }

    eastl::string ProcSelfFdProvider::generate_content()
    {
        if (_fd_num < 0) {
            return "";
        }
        // 返回文件描述符对应的文件路径
        // 这里需要根据实际的文件描述符表来获取路径
        // 简化实现，返回一个占位符
        return "/dev/pts/0";
    }

    eastl::string ProcSelfFdProvider::read_symlink_target()
    {
        // panic("ProcSelfFdProvider::read_symlink_target: not implemented");
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        fs::file *file = pcb->get_open_file(_fd_num);
        return file ? file->_path_name : "";
    }

    eastl::string EtcPasswdProvider::generate_content()
    {
        eastl::string result;
        result += "root:x:0:0:root:/root:/bin/sh\n";
        result += "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n";
        result += "bin:x:2:2:bin:/bin:/usr/sbin/nologin\n";
        result += "sys:x:3:3:sys:/dev:/usr/sbin/nologin\n";
        result += "sync:x:4:65534:sync:/bin:/bin/sync\n";
        return result;
    }

    eastl::string DevBlockProvider::generate_content()
    {
        // 块设备文件通常不包含文本内容，但可以返回设备信息
        eastl::string result;
        result += "Block device ";
        result += "8";
        result += ":";
        result += "0";
        result += "\n";
        return result;
    }

    eastl::string DevLoopProvider::generate_content()
    {
        // Loop设备是一种特殊的块设备，可以将文件挂载为块设备
        eastl::string result;
        result += "Loop device ready for mounting\n";
        return result;
    }

    // ======================== virtual_file 实现 ========================

    void virtual_file::ensure_content_cached()
    {
        if (!_content_cached) {
            if (_content_provider->is_dynamic() || !_content_cached) {
                _cached_content = _content_provider->generate_content();
            }
            _content_cached = true;
            // 更新文件统计信息
            _stat.size = _cached_content.size();
        }
    }

    bool virtual_file::is_virtual_path(const eastl::string& path)
    {
        return path.find("/proc/") == 0;
    }

    long virtual_file::read(uint64 buf, size_t len, long off, bool upgrade)
    {
        // printfGreen("virtual_file::read called with buf: %p, len: %u, off: %d, upgrade: %d\n", (void *)buf, len, off, upgrade);
        
        if (_attrs.u_read != 1) {
            printfRed("virtual_file:: not allowed to read!");
            return -1;
        }

        // 对于动态内容，每次都重新生成
        if (_content_provider->is_dynamic()) {
            _cached_content = _content_provider->generate_content();
            _stat.size = _cached_content.size();
        } else {
            // 确保内容已缓存
            ensure_content_cached();
        }

        // 处理偏移量参数
        if (off < 0) {
            off = _file_ptr;
        }

        // 检查偏移量是否有效
        if (off >= (long)_cached_content.size()) {
            return 0; // EOF
        }

        // 计算实际要读取的字节数
        size_t available = _cached_content.size() - off;
        size_t to_read = min(len, available);

        // 复制数据到用户缓冲区
        const char* src_data = _cached_content.c_str() + off;
        
        // 这里应该使用适当的内存复制函数，类似于 copy_to_user
        // 临时使用简单的内存复制
        char* dst_buf = (char*)buf;
        for (size_t i = 0; i < to_read; i++) {
            dst_buf[i] = src_data[i];
        }

        // 如果upgrade为true，更新文件指针
        if (upgrade) {
            _file_ptr = off + to_read;
        }

        return to_read;
    }


    eastl::string virtual_file::read_symlink_target()
    {
        if (_content_provider) {
            return _content_provider->read_symlink_target();
        }
        return "";
    }
    long virtual_file::write(uint64 buf, size_t len, long off, bool upgrade)
    {
        if (!_content_provider->is_writable()) {
            printfRed("virtual_file::write: this virtual file is read-only");
            return -1;
        }

        if (_attrs.u_write != 1) {
            printfRed("virtual_file:: not allowed to write!");
            return -1;
        }

        // 处理偏移量参数
        if (off < 0) {
            off = _file_ptr;
        }

        // 委托给内容提供者处理写入
        long result = _content_provider->handle_write(buf, len, off);

        // 如果upgrade为true且写入成功，更新文件指针
        if (result > 0 && upgrade) {
            _file_ptr = off + result;
        }

        return result;
    }

    bool virtual_file::read_ready()
    {
        // 虚拟文件总是可读的
        return true;
    }

    bool virtual_file::write_ready()
    {
        // 根据内容提供者的能力决定是否可写
        return _content_provider->is_writable();
    }

    off_t virtual_file::lseek(off_t offset, int whence)
    {
        // 对于动态内容，确保获得最新的文件大小
        if (_content_provider->is_dynamic()) {
            _cached_content = _content_provider->generate_content();
            _stat.size = _cached_content.size();
        } else {
            // 确保内容已缓存以获得正确的文件大小
            ensure_content_cached();
        }
        
        off_t new_off;
        switch (whence) {
            case SEEK_SET:
                if (offset < 0) {
                    return -EINVAL;
                }
                _file_ptr = offset;
                break;
            case SEEK_CUR:
                new_off = _file_ptr + offset;
                if (new_off < 0) {
                    return -EINVAL;
                }
                _file_ptr = new_off;
                break;
            case SEEK_END:
                new_off = _cached_content.size() + offset;
                if (new_off < 0) {
                    return -EINVAL;
                }
                _file_ptr = new_off;
                break;
            default:
                printfRed("virtual_file::lseek: invalid whence %d", whence);
                return -EINVAL;
        }
        
        return _file_ptr;
    }

    size_t virtual_file::read_sub_dir(mem::UserspaceStream &dst)
    {
        // 虚拟文件通常不是目录，不支持读取子目录
        panic("virtual_file::read_sub_dir: virtual files are not directories");
        return 0;
    }
    
    // 实现 /proc/sys/fs/pipe-user-pages-soft 的内容生成
    eastl::string ProcSysFsPipeUserPagesSoftProvider::generate_content()
    {
        auto int_to_string = [](uint num) -> eastl::string
        {
            if (num == 0)
                return "0";

            char buffer[16];
            int pos = 15;
            buffer[pos] = '\0';

            while (num > 0)
            {
                buffer[--pos] = '0' + (num % 10);
                num /= 10;
            }

            return eastl::string(&buffer[pos]);
        };
        return int_to_string(max_pipe_size);
    }
    
    // 实现 /proc/self/stat 的内容生成
    // 参考Linux的/proc/[pid]/stat格式
    eastl::string ProcSelfStatProvider::generate_content()
    {
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        if (!pcb) {
            return "0 (unknown) R 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
        }
        
        // 辅助函数：转换整数为字符串
        auto int_to_string = [](long num) -> eastl::string {
            if (num == 0) return "0";
            
            char buffer[32];
            int pos = 31;
            buffer[pos] = '\0';
            bool negative = num < 0;
            
            if (negative) {
                num = -num;
            }
            
            while (num > 0) {
                buffer[--pos] = '0' + (num % 10);
                num /= 10;
            }
            
            if (negative) {
                buffer[--pos] = '-';
            }
            
            return eastl::string(&buffer[pos]);
        };
        
        eastl::string result;
        
        // pid
        result += int_to_string(pcb->_pid) + " ";
        
        // comm (进程名，带括号)
        result += "(" + eastl::string(pcb->_name) + ") ";
        
        // state (进程状态: R=running, S=sleeping, Z=zombie, etc.)
        result += "R ";  // 假设进程处于运行状态
        
        // ppid (父进程ID)
        result += int_to_string(pcb->_ppid) + " ";

        // pgrp (进程组ID)
        result += int_to_string(pcb->_pid) + " ";
        
        // session
        result += int_to_string(pcb->_pid) + " ";
        
        // tty_nr (控制终端)
        result += "0 ";
        
        // tpgid (控制终端的前台进程组)
        result += "0 ";
        
        // flags
        result += "0 ";
        
        // minflt (次缺页错误数)
        result += "0 ";
        
        // cminflt (子进程次缺页错误数)
        result += "0 ";
        
        // majflt (主缺页错误数)
        result += "0 ";
        
        // cmajflt (子进程主缺页错误数)
        result += "0 ";
        
        // utime (用户态CPU时间)
        result += int_to_string(pcb->_utime) + " ";
        
        // stime (内核态CPU时间)
        result += int_to_string(pcb->_stime) + " ";
        
        // cutime (子进程用户态CPU时间)
        result += "0 ";
        
        // cstime (子进程内核态CPU时间)
        result += "0 ";
        
        // priority (进程优先级)
        result += "0 ";
        
        // nice (nice值)
        result += "0 ";
        
        // num_threads (线程数)
        result += "1 ";
        
        // itrealvalue (SIGALRM倒计时值)
        result += "0 ";
        
        // starttime (启动时间，自系统启动后的节拍数)
        result += int_to_string(pcb->_start_time) + " ";
        
        // vsize (虚拟内存大小，字节)
        result += "4194304 ";  // 假设4MB虚拟内存
        
        // rss (常驻内存大小，页)
        result += "1024 ";  // 假设1024页
        
        // rsslim (常驻内存限制)
        result += "4294967295 ";  // 无限制
        
        // startcode (代码段起始地址)
        result += "0 ";
        
        // endcode (代码段结束地址)
        result += "0 ";
        
        // startstack (堆栈起始地址)
        result += "0 ";
        
        // kstkesp (ESP寄存器值)
        result += "0 ";
        
        // kstkeip (EIP寄存器值)
        result += "0 ";
        
        // signal (待处理信号位图)
        result += "0 ";
        
        // blocked (阻塞信号位图)
        result += "0 ";
        
        // sigignore (忽略信号位图)
        result += "0 ";
        
        // sigcatch (捕获信号位图)
        result += "0 ";
        
        // wchan (进程休眠内核函数地址)
        result += "0 ";
        
        // 添加换行符结束
        result += "\n";
        
        return result;
    }

} // namespace fs
