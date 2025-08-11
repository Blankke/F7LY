#include "fs/vfs/file/virtual_file.hh"
// #include "fs/lwext4/ext4_errno.hh"
// #include "fs/lwext4/ext4.hh"
// #include "mem/userspace_stream.hh"
#include "proc_manager.hh"
#include "proc/meminfo.hh"
#include "proc/cpuinfo.hh"
#include "printer.hh"
#include "proc/proc.hh"
#include "trap/interrupt_stats.hh"
// #include "mem/mem_layout.hh"
#include "fs/vfs/file/normal_file.hh"
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
        result += "nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n"; //这行不能删，挺多ltp要用这一行。前面几行应该随意一点。
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
        // 具体的 loop 设备节点，这里返回设备信息
        eastl::string result;
        result += "Loop device #";
        
        // 手动转换数字到字符串
        char num_str[16];
        int temp = _loop_number;
        int pos = 0;
        if (temp == 0) {
            num_str[pos++] = '0';
        } else {
            char temp_str[16];
            int temp_pos = 0;
            while (temp > 0) {
                temp_str[temp_pos++] = '0' + (temp % 10);
                temp /= 10;
            }
            // 反转字符串
            for (int i = temp_pos - 1; i >= 0; i--) {
                num_str[pos++] = temp_str[i];
            }
        }
        num_str[pos] = '\0';
        
        result += num_str;
        result += "\n";
        result += "Device path: /dev/loop";
        result += num_str;
        result += "\n";
        return result;
    }

    eastl::string DevLoopControlProvider::generate_content()
    {
        // Loop 控制设备，用于管理 loop 设备的创建和删除
        eastl::string result;
        result += "Loop control device\n";
        result += "Use ioctl() to manage loop devices\n";
        result += "Supported operations: LOOP_CTL_GET_FREE, LOOP_CTL_ADD, LOOP_CTL_REMOVE\n";
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

        // 特殊处理 /dev/zero 设备
        if (_content_provider && _content_provider->get_provider_type() == VirtualProviderType::DEV_ZERO) {
            // 处理偏移量参数
            if (off < 0) {
                off = _file_ptr;
            }

            // /dev/zero 可以读取任意数量的零字节
            char* dst_buf = (char*)buf;
            for (size_t i = 0; i < len; i++) {
                dst_buf[i] = 0;
            }

            // 如果upgrade为true，更新文件指针
            if (upgrade) {
                _file_ptr = off + len;
            }

            return len;
        }

        // 特殊处理 /dev/null 设备
        if (_content_provider && _content_provider->get_provider_type() == VirtualProviderType::DEV_NULL) {
            // /dev/null 读取时总是返回 0 字节（EOF）
            // 处理偏移量参数
            if (off < 0) {
                off = _file_ptr;
            }

            // 如果upgrade为true，更新文件指针（虽然对于/dev/null来说意义不大）
            if (upgrade) {
                _file_ptr = off;
            }

            return 0; // 总是返回 EOF
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
        if (off >= (long)_cached_content.size()) 
        {
            printfRed("virtual_file::read: off=%d is out of bounds, size=%u\n", off, _cached_content.size());
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
        if (!_content_provider->is_writable())
        {
            printfRed("virtual_file::write: this virtual file is read-only");
            return -EBADF;
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

    // 实现 /proc/sys/kernel/pid_max 的内容生成
    eastl::string ProcSysKernelPidMaxProvider::generate_content()
    {
        // 将常量转换为字符串
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
        
        return int_to_string(proc::pid_max) + "\n";
    }

    // 实现通用的 /proc/<pid>/stat 的内容生成
    eastl::string ProcPidStatProvider::generate_content()
    {
        proc::Pcb *target_pcb = nullptr;
        
        if (target_pid == -1) {
            // /proc/self/stat - 使用当前进程
            target_pcb = proc::k_pm.get_cur_pcb();
        } else {
            // /proc/<pid>/stat - 查找指定PID的进程
            target_pcb = proc::k_pm.find_proc_by_pid(target_pid);
        }
        
        if (!target_pcb) {
            // 如果找不到进程，返回默认值或错误格式
            if (target_pid == 1) {
                // init进程的特殊处理
                return "1 (init) S 0 1 1 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
            }
            return "0 (unknown) R 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
        }
        
        return generate_stat_content(target_pcb);
    }
    
    // 生成标准Linux /proc/[pid]/stat格式的内容
    eastl::string ProcPidStatProvider::generate_stat_content(proc::Pcb* pcb)
    {
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
        
        // 1. pid (进程ID)
        result += int_to_string(pcb->_pid) + " ";
        
        // 2. comm (进程名，带括号)
        result += "(" + eastl::string(pcb->_name) + ") ";
        
        // 3. state (进程状态: R=running, S=sleeping, Z=zombie, D=disk sleep等)
        char state_char = 'R'; // 默认运行状态
        switch (pcb->_state) {
            case proc::ProcState::RUNNING:
            case proc::ProcState::RUNNABLE:
                state_char = 'R';
                break;
            case proc::ProcState::SLEEPING:
                state_char = 'S';
                break;
            case proc::ProcState::ZOMBIE:
                state_char = 'Z';
                break;
            default:
                state_char = 'R';
                break;
        }
        result += state_char;
        result += " ";
        
        // 4. ppid (父进程ID)
        result += int_to_string(pcb->get_ppid()) + " ";

        // 5. pgrp (进程组ID)
        result += int_to_string(pcb->get_pgid()) + " ";
        
        // 6. session (会话ID)
        result += int_to_string(pcb->get_sid()) + " ";
        
        // 7. tty_nr (控制终端)
        result += "0 ";
        
        // 8. tpgid (控制终端的前台进程组)
        result += "-1 ";
        
        // 9. flags (进程标志)
        result += "4194304 ";
        
        // 10. minflt (次缺页错误数)
        result += "0 ";
        
        // 11. cminflt (子进程次缺页错误数)
        result += "0 ";
        
        // 12. majflt (主缺页错误数)
        result += "0 ";
        
        // 13. cmajflt (子进程主缺页错误数)
        result += "0 ";
        
        // 14. utime (用户态CPU时间，以时钟滴答为单位)
        result += int_to_string(pcb->_utime) + " ";
        
        // 15. stime (内核态CPU时间，以时钟滴答为单位)
        result += int_to_string(pcb->_stime) + " ";
        
        // 16. cutime (子进程用户态CPU时间)
        result += int_to_string(pcb->_cutime) + " ";
        
        // 17. cstime (子进程内核态CPU时间)
        result += int_to_string(pcb->_cstime) + " ";
        
        // 18. priority (进程优先级)
        result += int_to_string(20) + " ";  // 默认优先级20
        
        // 19. nice (nice值)
        result += int_to_string(0) + " ";   // 默认nice值0
        
        // 20. num_threads (线程数)
        result += "1 ";
        
        // 21. itrealvalue (SIGALRM倒计时值，已弃用)
        result += "0 ";
        
        // 22. starttime (启动时间，自系统启动后的节拍数)
        result += int_to_string(pcb->_start_time) + " ";
        
        // 23. vsize (虚拟内存大小，字节)
        uint64 vsize = 0;
        if (pcb->get_memory_manager()) {
            // 使用实际的内存管理器方法获取虚拟内存大小
            vsize = pcb->get_memory_manager()->get_total_memory_usage();
            uint64 vma_usage = pcb->get_memory_manager()->get_vma_memory_usage();
            vsize += vma_usage;
        }
        if (vsize == 0) {
            vsize = 4194304; // 默认4MB
        }
        result += int_to_string(vsize) + " ";
        
        // 24. rss (常驻内存大小，页)
        uint64 rss = 0;
        if (pcb->get_memory_manager()) {
            // 简单估算RSS：使用程序段大小作为近似值
            uint64 total_mem = pcb->get_memory_manager()->get_total_memory_usage();
            rss = (total_mem + PGSIZE - 1) / PGSIZE; // 转换为页数
        }
        if (rss == 0) {
            rss = 1024; // 默认1024页
        }
        result += int_to_string(rss) + " ";
        
        // 25. rsslim (常驻内存限制)
        result += "18446744073709551615 ";  // RLIM_INFINITY
        
        // 26. startcode (代码段起始地址)
        result += "134512640 ";
        
        // 27. endcode (代码段结束地址)
        result += "134529084 ";
        
        // 28. startstack (堆栈起始地址)
        result += "140737488347136 ";
        
        // 29. kstkesp (ESP寄存器值，已弃用)
        result += "0 ";
        
        // 30. kstkeip (EIP寄存器值，已弃用)
        result += "0 ";
        
        // 31. signal (待处理信号位图)
        result += int_to_string(pcb->_signal) + " ";
        
        // 32. blocked (阻塞信号位图)
        result += int_to_string(pcb->_sigmask) + " ";
        
        // 33. sigignore (忽略信号位图)
        result += "0 ";
        
        // 34. sigcatch (捕获信号位图)
        result += "0 ";
        
        // 35. wchan (进程休眠内核函数地址)
        result += "0 ";
        
        // 36. nswap (已交换页数，已弃用)
        result += "0 ";
        
        // 37. cnswap (子进程已交换页数，已弃用)
        result += "0 ";
        
        // 38. exit_signal (退出时发送给父进程的信号)
        result += "17 ";  // SIGCHLD
        
        // 39. processor (最后运行的处理器号)
        result += int_to_string(0) + " ";
        
        // 40. rt_priority (实时优先级)
        result += "0 ";
        
        // 41. policy (调度策略)
        result += "0 ";  // SCHED_NORMAL
        
        // 42. delayacct_blkio_ticks (块I/O延迟累计时间)
        result += "0 ";
        
        // 43. guest_time (虚拟化guest时间)
        result += "0 ";
        
        // 44. cguest_time (子进程虚拟化guest时间)
        result += "0 ";
        
        // 45. start_data (数据段起始地址)
        result += "134529084 ";
        
        // 46. end_data (数据段结束地址)
        result += "134531240 ";
        
        // 47. start_brk (堆起始地址)
        result += "134531240 ";
        
        // 48. arg_start (参数起始地址)
        result += "140737488346624 ";
        
        // 49. arg_end (参数结束地址)
        result += "140737488346633 ";
        
        // 50. env_start (环境变量起始地址)
        result += "140737488346633 ";
        
        // 51. env_end (环境变量结束地址)
        result += "140737488347136 ";
        
        // 52. exit_code (退出码)
        result += int_to_string(pcb->_xstate) + " ";
        
        // 添加换行符结束
        result += "\n";
        
        return result;
    }

    eastl::string ProcInterruptsProvider::generate_content()
    {
        // 生成 /proc/interrupts 的内容
        // 格式：中断号:        计数\n
        return intr_stats::k_intr_stats.get_interrupts_info();
    }

    // ======================== DevZeroProvider 实现 ========================
    
    eastl::string DevZeroProvider::generate_content()
    {
        // /dev/zero 不需要预先生成内容，因为它产生无限的零字节
        // 这个方法不应该被调用，因为我们会重写读取逻辑
        return "";
    }

    long DevZeroProvider::handle_write(uint64 buf, size_t len, long off)
    {
        // /dev/zero 设备丢弃所有写入的数据，总是返回写入的长度
        (void)buf;  // 忽略缓冲区内容
        (void)off;  // 忽略偏移量
        return len; // 假装写入了所有数据
    }

    // ======================== DevNullProvider 实现 ========================
    
    eastl::string DevNullProvider::generate_content()
    {
        // /dev/null 读取时总是返回空内容（EOF）
        return "";
    }

    long DevNullProvider::handle_write(uint64 buf, size_t len, long off)
    {
        // /dev/null 设备丢弃所有写入的数据，总是返回写入的长度
        (void)buf;  // 忽略缓冲区内容
        (void)off;  // 忽略偏移量
        return len; // 假装写入了所有数据
    }

    // ======================== ProcSelfMapsProvider 实现 ========================
    
    eastl::string ProcSelfMapsProvider::generate_content()
    {
        // TODO: 不懂mmap，等曹老师写
        printfRed("需要曹老师实现，下面写的格式不对，内容也不对，只是个示例\n");
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        if (!pcb || !pcb->get_vma()) {
            return "";
        }

        eastl::string result;
        
        // 遍历所有VMA区域
        for (int i = 0; i < proc::NVMA; i++) {
            proc::vma &vm = pcb->get_vma()->_vm[i];
            if (!vm.used) {
                continue;
            }

            // 格式：address perms offset dev inode pathname
            // 例如：00400000-0040c000 r-xp 00000000 08:01 1234567 /bin/cat
            
            // 地址范围 (start-end)
            char addr_buf[64];
            // sprintf(addr_buf, "%016lx-%016lx ", vm.addr, vm.addr + vm.len);
            result += addr_buf;
            
            // 权限 (rwxp/rwxs)
            char perms[5] = "----";
            if (vm.prot & 0x1) perms[0] = 'r';  // PROT_READ
            if (vm.prot & 0x2) perms[1] = 'w';  // PROT_WRITE  
            if (vm.prot & 0x4) perms[2] = 'x';  // PROT_EXEC
            if (vm.flags & 0x1) perms[3] = 's'; // MAP_SHARED
            else perms[3] = 'p';                // MAP_PRIVATE
            perms[4] = '\0';
            result += perms;
            result += " ";
            
            // 文件偏移
            char offset_buf[16];
            // sprintf(offset_buf, "%08x ", vm.offset);
            result += offset_buf;
            
            // 设备号 (major:minor)
            result += "00:00 ";
            
            // inode号
            result += "0 ";
            
            // 路径名
            if (vm.vfile && !vm.vfile->_path_name.empty()) {
                result += vm.vfile->_path_name;
            } else if (vm.addr <= 0x400000) {
                result += "[heap]";
            } else if (vm.addr >= 0x7fff0000) {
                result += "[stack]";
            }
            
            result += "\n";
        }
        
        return result;
    }

    // ======================== ProcSelfPagemapProvider 实现 ========================
    
    eastl::string ProcSelfPagemapProvider::generate_content()
    {
        // TODO: 等曹老师写
        printfRed("需要曹老师实现，只是个示例\n");

        // /proc/self/pagemap 是二进制文件，每个虚拟页面对应8字节
        // 由于这是一个文本实现，我们返回空内容或简化的表示
        // 在真实实现中，这应该生成二进制数据
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        if (!pcb) {
            return "";
        }
        
        eastl::string result;
        result += "# Pagemap entries (simplified text representation)\n";
        result += "# Format: virtual_addr -> physical_addr [flags]\n";
        
        //关于二进制数据，可以用二进制表示进一段内存中，转成char*再转eastl::string
        
        return result;
    }

    // ======================== ProcSelfStatusProvider 实现 ========================
    
    eastl::string ProcSelfStatusProvider::generate_content()
    {
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        if (!pcb) {
            return "";
        }

        eastl::string result;
        
        // Name: 进程名
        result += "Name:\t";
        result += eastl::string(pcb->_name);
        result += "\n";
        
        // State: 进程状态
        result += "State:\t";
        switch (pcb->_state) {
            case proc::ProcState::UNUSED: result += "unused"; break;
            case proc::ProcState::USED: result += "used"; break;
            case proc::ProcState::SLEEPING: result += "S (sleeping)"; break;
            case proc::ProcState::RUNNABLE: result += "R (running)"; break;
            case proc::ProcState::RUNNING: result += "R (running)"; break;
            case proc::ProcState::ZOMBIE: result += "Z (zombie)"; break;
            default: result += "unknown"; break;
        }
        result += "\n";
        
        // Tgid, Ngid, Pid, PPid
        auto int_to_string = [](int num) -> eastl::string {
            if (num == 0) return "0";
            
            char buffer[16];
            int pos = 15;
            buffer[pos] = '\0';
            
            while (num > 0) {
                buffer[--pos] = '0' + (num % 10);
                num /= 10;
            }
            
            return eastl::string(&buffer[pos]);
        };
        
        result += "Tgid:\t" + int_to_string(pcb->_tgid) + "\n";
        result += "Ngid:\t0\n";  // 命名空间组ID，暂时为0
        result += "Pid:\t" + int_to_string(pcb->_pid) + "\n";
        result += "PPid:\t" + int_to_string(pcb->_ppid) + "\n";
        
        // TracerPid
        result += "TracerPid:\t0\n";
        
        // Uid 信息
        result += "Uid:\t" + int_to_string(pcb->_uid) + "\t" + 
                  int_to_string(pcb->_euid) + "\t" + 
                  int_to_string(pcb->_suid) + "\t" + 
                  int_to_string(pcb->_fsuid) + "\n";
                  
        // Gid 信息  
        result += "Gid:\t" + int_to_string(pcb->_gid) + "\t" + 
                  int_to_string(pcb->_egid) + "\t0\t0\n";
        
        // FDSize: 文件描述符表大小
        result += "FDSize:\t" + int_to_string(proc::max_open_files) + "\n";
        
        // Groups: 附加组ID列表（暂时为空）
        result += "Groups:\t\n";
        
        // NStgid, NSpid, NSpgid, NSsid（命名空间相关，暂时等于对应的全局ID）
        result += "NStgid:\t" + int_to_string(pcb->_tgid) + "\n";
        result += "NSpid:\t" + int_to_string(pcb->_pid) + "\n";  
        result += "NSpgid:\t" + int_to_string(pcb->_pgid) + "\n";
        result += "NSsid:\t" + int_to_string(pcb->_sid) + "\n";
        
        // VmPeak, VmSize: 虚拟内存大小（KB）
        uint64 vm_size_kb = pcb->get_size() / 1024;
        result += "VmPeak:\t" + int_to_string(vm_size_kb) + " kB\n";
        result += "VmSize:\t" + int_to_string(vm_size_kb) + " kB\n";
        
        // VmLck, VmPin, VmHWM, VmRSS（暂时设为0或简化值）
        result += "VmLck:\t0 kB\n";
        result += "VmPin:\t0 kB\n"; 
        result += "VmHWM:\t" + int_to_string(vm_size_kb) + " kB\n";
        result += "VmRSS:\t" + int_to_string(vm_size_kb) + " kB\n";
        
        // VmData, VmStk, VmExe, VmLib（内存段信息，简化实现）
        result += "VmData:\t" + int_to_string(vm_size_kb / 4) + " kB\n";
        result += "VmStk:\t132 kB\n";
        result += "VmExe:\t" + int_to_string(vm_size_kb / 8) + " kB\n";
        result += "VmLib:\t0 kB\n";
        result += "VmPTE:\t" + int_to_string(vm_size_kb / 1024) + " kB\n";
        result += "VmSwap:\t0 kB\n";
        
        // Threads: 线程数
        result += "Threads:\t1\n";
        
        // SigQ: 排队信号数
        result += "SigQ:\t0/1024\n";
        
        // SigPnd, ShdPnd: 待处理信号掩码
        char sig_buf[32];
        // sprintf(sig_buf, "%016lx", pcb->_signal);
        result += "SigPnd:\t" + eastl::string(sig_buf) + "\n";
        result += "ShdPnd:\t0000000000000000\n";
        
        // SigBlk: 阻塞信号掩码
        // sprintf(sig_buf, "%016lx", pcb->_sigmask);
        result += "SigBlk:\t" + eastl::string(sig_buf) + "\n";
        result += "SigIgn:\t0000000000000000\n";
        result += "SigCgt:\t0000000000000000\n";
        
        // CapInh, CapPrm, CapEff, CapBnd, CapAmb: 能力集（暂时全为0）
        result += "CapInh:\t0000000000000000\n";
        result += "CapPrm:\t0000000000000000\n";
        result += "CapEff:\t0000000000000000\n";
        result += "CapBnd:\t0000000000000000\n";
        result += "CapAmb:\t0000000000000000\n";
        
        // NoNewPrivs: 禁止获取新特权
        result += "NoNewPrivs:\t0\n";
        
        // Seccomp: seccomp模式
        result += "Seccomp:\t0\n";
        
        // Speculation_Store_Bypass: 推测执行相关
        result += "Speculation_Store_Bypass:\tnot vulnerable\n";
        
        // Cpus_allowed: 允许运行的CPU掩码
        result += "Cpus_allowed:\tffffffff\n";
        result += "Cpus_allowed_list:\t0-31\n";
        
        // Mems_allowed: 允许的内存节点
        result += "Mems_allowed:\t1\n";
        result += "Mems_allowed_list:\t0\n";
        
        // voluntary_ctxt_switches, nonvoluntary_ctxt_switches: 上下文切换次数
        result += "voluntary_ctxt_switches:\t0\n";
        result += "nonvoluntary_ctxt_switches:\t0\n";
        
        return result;
    }

    // ======================== shmmax提供者实现 ========================
    eastl::string ProcSysKernelShmmaxProvider::generate_content()
    {
        // 返回系统共享内存段的最大大小，单位为字节
        // 这里假设最大值为 32MB
        const uint64 shmmax = 32 * 1024 * 1024; // 32MB
        char _buffer[32];
        snprintf(_buffer, sizeof(_buffer), "%lu\n", shmmax);
        eastl::string result(_buffer);
        return result;
    }
    eastl::string ProcSysKernelShmmniProvider::generate_content()
    {
        // 返回系统共享内存段的最小大小，单位为字节
        // 这里假设最大值为 32MB
        const uint64 shmmax = 4 * 1024; // 4KB
        char _buffer[32];
        snprintf(_buffer, sizeof(_buffer), "%lu\n", shmmax);
        eastl::string result(_buffer);
        return result;
    }
    eastl::string ProcSysKernelShmallProvider::generate_content()
    {
        // 返回系统共享内存段的总大小，单位为字节
        // 这里假设最大值为 32MB
        const uint64 shmall = 16384;
        char _buffer[32];
        snprintf(_buffer, sizeof(_buffer), "%lu\n", shmall);
        eastl::string result(_buffer);
        return result;
    }

    // ======================== tainted提供者实现 ========================
    eastl::string ProcSysKernelTaintedProvider::generate_content()
    {
        // 返回内核污染状态
        // 0 表示内核未被污染（无专有模块或其他问题）
        return "0\n";
    }
} // namespace fs
