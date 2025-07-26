/**
 * @file proc.cc
 * @brief 进程控制块(PCB)实现文件
 *
 * 实现进程控制块的构造、初始化、资源清理等核心功能。
 * PCB结构参照Linux内核设计，包含进程标识、状态管理、内存管理、
 * 文件系统、信号处理、资源限制等各个方面。
 *
 * 主要功能：
 * - 进程控制块的初始化和清理
 * - 内核栈的映射和管理
 * - 文件描述符表的管理
 * - 信号处理结构的管理
 * - 进程优先级管理
 * - 上下文信息打印（调试用）
 */

#include "proc.hh"
#include "context.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"

namespace proc
{
    Pcb k_proc_pool[num_process]; // 全局进程池，存储所有进程的PCB

    Pcb::Pcb()
    {
        /****************************************************************************************
         * 基本进程标识和状态管理
         ****************************************************************************************/
        _global_id = 0;                  // 全局ID，在进程池中的唯一标识
        _pid = 0;                        // 进程ID
        _tid = 0;                        // 线程ID，单线程进程中等于PID
        _parent = nullptr;               // 父进程指针
        memset(_name, 0, sizeof(_name)); // 进程名称
        exe.clear();                     // 可执行文件路径

        // Linux标准进程标识符
        _ppid = 0;  // 父进程PID
        _pgid = 0;  // 进程组ID
        _tgid = 0;  // 线程组ID
        _sid = 0;   // 会话ID
        _uid = 0;   // 真实用户ID
        _euid = 0;  // 有效用户ID
        _suid = 0;  // 保存的设置用户ID
        _fsuid = 0; // 文件系统用户ID
        _gid = 0;   // 真实组ID
        _egid = 0;  // 有效组ID

        /****************************************************************************************
         * 进程状态和调度信息
         ****************************************************************************************/
        _state = UNUSED; // 进程状态初始化为未使用
        _chan = nullptr; // 睡眠等待通道
        _killed = 0;     // 进程终止标志
        _xstate = 0;     // 进程退出状态码

        // 调度相关字段
        _slot = 0;                     // 时间片剩余量
        _priority = default_proc_prio; // 默认进程优先级

        /****************************************************************************************
         * 内存管理
         ****************************************************************************************/
        _kstack = 0;          // 内核栈虚拟地址
        _sz = 0;              // 进程占用的总内存空间大小
        _shared_vm = false;   // 是否与父进程共享虚拟内存
        _trapframe = nullptr; // 用户态寄存器保存区

        // 堆内存管理初始化
        _heap_start = 0; // 堆的起始地址
        _heap_end = 0;   // 堆的结束地址

        // 虚拟内存区域管理
        _vma = nullptr; // VMA管理结构指针

        /****************************************************************************************
         * 文件系统和I/O管理
         ****************************************************************************************/
        _cwd = nullptr;    // 当前工作目录的dentry指针
        _cwd_name.clear(); // 当前工作目录路径字符串
        _ofile = nullptr;  // 打开文件描述符表
        _umask = 0022;     // 默认umask值 (octal 022)

        /****************************************************************************************
         * 线程和同步原语
         ****************************************************************************************/
        _futex_addr = nullptr;  // futex等待地址
        _clear_tid_addr = 0;    // 线程退出时清除的地址
        _robust_list = nullptr; // 健壮futex链表头

        /****************************************************************************************
         * 信号处理
         ****************************************************************************************/
        _sigactions = nullptr; // 信号处理函数表
        _sigmask = 0;          // 信号屏蔽掩码
        _signal = 0;           // 待处理信号掩码
        sig_frame = nullptr;   // 信号处理栈帧

        /****************************************************************************************
         * 资源限制
         ****************************************************************************************/
        // 初始化所有资源限制为0，在init()中设置具体值
        for (uint i = 0; i < ResourceLimitId::RLIM_NLIMITS; i++)
        {
            _rlim_vec[i].rlim_cur = 0;
            _rlim_vec[i].rlim_max = 0;
        }

        /****************************************************************************************
         * 时间统计和会计信息
         ****************************************************************************************/
        _start_tick = 0;        // 进程开始运行时的时钟节拍数
        _user_ticks = 0;        // 用户态累计运行时钟节拍数
        _last_user_tick = 0;    // 上次进入用户态的时钟节拍数
        _kernel_entry_tick = 0; // 进入内核态的时钟节拍数

        // 详细时间统计
        _stime = 0;          // 系统态时间(内核态运行时间)
        _cutime = 0;         // 子进程用户态时间累计
        _cstime = 0;         // 子进程系统态时间累计
        _start_time = 0;     // 进程启动时间(绝对时间戳)
        _start_boottime = 0; // 自系统启动以来的启动时间

        /****************************************************************************************
         * 程序段描述(调试和分析用)
         ****************************************************************************************/
        _prog_section_cnt = 0; // 已记录的程序段数量
        for (int i = 0; i < max_program_section_num; i++)
        {
            _prog_sections[i]._sec_start = nullptr;
            _prog_sections[i]._sec_size = 0;
            _prog_sections[i]._debug_name = nullptr;
        }

        /****************************************************************************************
         * 资源限制初始化
         ****************************************************************************************/
        // 设置栈大小限制 (TODO: 需要根据实际需求设置)
        _rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_cur = 0;
        _rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_max = 0;

        // 设置打开文件数量限制
        _rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_cur = max_open_files;
        _rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_max = max_open_files;
        
        // 设置文件大小限制 (默认无限制)
        _rlim_vec[ResourceLimitId::RLIMIT_FSIZE].rlim_cur = ResourceLimitId::RLIM_INFINITY;
        _rlim_vec[ResourceLimitId::RLIMIT_FSIZE].rlim_max = ResourceLimitId::RLIM_INFINITY;
    }

    void Pcb::init(const char *lock_name, uint gid)
    {
        // 初始化进程控制块的锁
        _lock.init(lock_name);

        // 设置进程状态和基本信息
        _state = ProcState::UNUSED;
        _global_id = gid;
        _kstack = mem::VirtualMemoryManager::kstack_vm_from_global_id(_global_id);
    }

    void Pcb::cleanup_sighand()
    {
        if (_sigactions != nullptr)
        {
            // 减少信号处理结构的引用计数
            _sigactions->refcnt--;

            // 如果引用计数降到0或以下，释放所有资源
            if (_sigactions->refcnt <= 0)
            {
                // 遍历所有信号，释放对应的处理函数
                for (int i = 0; i <= ipc::signal::SIGRTMAX; ++i)
                {
                    if (_sigactions->actions[i] != nullptr)
                    {
                        delete _sigactions->actions[i];
                        _sigactions->actions[i] = nullptr;
                    }
                }
                // 释放信号处理结构本身
                delete _sigactions;
            }
            // 清空当前进程的信号处理指针
            _sigactions = nullptr;
        }
    }

    void Pcb::cleanup_ofile()
    {
        if (_ofile != nullptr)
        {
            // 减少打开文件表的引用计数
            _ofile->_shared_ref_cnt--;

            // 如果引用计数降到0或以下，关闭所有打开的文件
            if (_ofile->_shared_ref_cnt <= 0)
            {
                // 遍历所有文件描述符，关闭打开的文件
                for (uint64 i = 0; i < max_open_files; ++i)
                {
                    if (_ofile->_ofile_ptr[i] != nullptr)
                    {
                        // 释放文件对象资源
                        _ofile->_ofile_ptr[i]->free_file();
                        _ofile->_ofile_ptr[i] = nullptr;
                    }
                }
                // 释放打开文件表结构本身
                delete _ofile;
            }
            // 清空当前进程的文件表指针
            _ofile = nullptr;
        }
    }

    void Pcb::map_kstack(mem::PageTable &pt)
    {
        // 检查内核栈地址是否已经初始化
        if (_kstack == 0)
            panic("pcb was not init");

        // 分配一个物理页作为内核栈
        char *pa = (char *)mem::k_pmm.alloc_page();
        if (pa == 0)
            panic("pcb map kstack: no memory");

        // 清零分配的物理页
        mem::k_pmm.clear_page((void *)pa);

#ifdef RISCV
        // RISC-V架构：映射内核栈页面，设置可读可写权限
        // printfBlue("map kstack: %p, end: %p\n", _kstack, _kstack + PGSIZE-1);
        if (!mem::k_vmm.map_pages(pt, _kstack, PGSIZE, (uint64)pa,
                                  riscv::PteEnum::pte_readable_m |
                                      riscv::PteEnum::pte_writable_m))
            panic("kernel vm map failed");
#elif defined(LOONGARCH)
        // LoongArch架构：映射内核栈页面，设置相应的页表项权限
        // TODO: 未测试正确性 (参考自华科实现)
        if (!mem::k_vmm.map_pages(pt, _kstack, PGSIZE, (uint64)pa,
                                  PTE_NX | PTE_P | PTE_W | PTE_MAT | PTE_D | PTE_PLV))
            panic("kernel vm map failed");
#endif
    }

    int Pcb::get_priority()
    {
        // 获取进程优先级时需要加锁，确保读取的一致性
        _lock.acquire();
        int priority = _priority;
        _lock.release();
        return priority;
    }

    /****************************************************************************************
     * 程序段管理方法实现
     ****************************************************************************************/
    int Pcb::add_program_section(void *start, ulong size, const char *name)
    {
        if (_prog_section_cnt >= max_program_section_num)
        {
            panic("add_program_section: too many program sections\n");
            return -1;
        }

        int index = _prog_section_cnt++;
        _prog_sections[index]._sec_start = start;
        _prog_sections[index]._sec_size = size;
        _prog_sections[index]._debug_name = name;

        // 更新总内存大小
        update_total_memory_size();

        verify_memory_consistency();

        return index;
    }

    void Pcb::remove_program_section(int index)
    {
        if (index < 0 || index >= _prog_section_cnt)
        {
            printfRed("remove_program_section: invalid index %d\n", index);
            return;
        }

        // 移动后续段到前面
        for (int i = index; i < _prog_section_cnt - 1; i++)
        {
            _prog_sections[i] = _prog_sections[i + 1];
        }

        _prog_section_cnt--;

        // 清理最后一个位置
        _prog_sections[_prog_section_cnt]._sec_start = nullptr;
        _prog_sections[_prog_section_cnt]._sec_size = 0;
        _prog_sections[_prog_section_cnt]._debug_name = nullptr;

        // 更新总内存大小
        update_total_memory_size();

        verify_memory_consistency();
    }

    void Pcb::clear_all_program_sections()
    {
        for (int i = 0; i < _prog_section_cnt; i++)
        {
            _prog_sections[i]._sec_start = nullptr;
            _prog_sections[i]._sec_size = 0;
            _prog_sections[i]._debug_name = nullptr;
        }
        _prog_section_cnt = 0;

        // 重新计算总内存大小：只包含堆空间
        update_total_memory_size();

        verify_memory_consistency();
    }

    void Pcb::reset_memory_sections()
    {
        // 清空所有程序段
        clear_all_program_sections();
        
        // 重置堆信息
        _heap_start = 0;
        _heap_end = 0;
        
        // 重置总内存大小
        _sz = 0;
    }

    uint64 Pcb::get_total_program_memory() const
    {
        uint64 total = 0;
        for (int i = 0; i < _prog_section_cnt; i++)
        {
            total += _prog_sections[i]._sec_size;
        }
        return total;
    }

    void Pcb::copy_program_sections(const Pcb *src)
    {
        _prog_section_cnt = src->_prog_section_cnt;
        for (int i = 0; i < _prog_section_cnt; i++)
        {
            _prog_sections[i] = src->_prog_sections[i];
        }

        // 更新总内存大小
        update_total_memory_size();
    }

    void Pcb::free_program_sections()
    {
        // 释放程序段占用的内存
        for (int i = 0; i < _prog_section_cnt; i++)
        {
            if (_prog_sections[i]._sec_start && _prog_sections[i]._sec_size > 0)
            {
                uint64 va_start = PGROUNDDOWN((uint64)_prog_sections[i]._sec_start);
                uint64 va_end = PGROUNDUP((uint64)_prog_sections[i]._sec_start + _prog_sections[i]._sec_size);

                // 释放该段的所有页面
                for (uint64 va = va_start; va < va_end; va += PGSIZE)
                {
                    mem::Pte pte = _pt.walk(va, 0);
                    if (pte.is_null())
                    {
                        panic("free_program_sections: PTE is null for va %p, this should not happen", (void *)va);
                    }
                    if (!pte.is_valid())
                    {
                        panic("free_program_sections: PTE is invalid for va %p, this should not happen", (void *)va);
                    }
                    mem::k_vmm.vmunmap(_pt, va, 1, 1);
                }
            }
        }
        clear_all_program_sections();
    }

    /****************************************************************************************
     * 堆内存管理方法实现
     ****************************************************************************************/
    void Pcb::init_heap(uint64 start_addr)
    {
        _heap_start = start_addr;
        _heap_end = start_addr;

        // 更新总内存大小
        update_total_memory_size();
    }

    uint64 Pcb::grow_heap(uint64 new_end)
    {
        if (new_end <= _heap_end)
        {
            return _heap_end;
        }

        // 分配新的堆内存，使用适当的页表项标志
#ifdef RISCV
        uint64 result = mem::k_vmm.vmalloc(_pt, _heap_end, new_end, PTE_W | PTE_R | PTE_U);
#elif defined(LOONGARCH)
        uint64 result = mem::k_vmm.vmalloc(_pt, _heap_end, new_end, PTE_P | PTE_W | PTE_PLV | PTE_MAT | PTE_D);
#endif

        if (result < new_end)
        {
            // 分配失败
            return _heap_end;
        }

        // 更新堆结束地址和总内存大小
        _heap_end = new_end;
        update_total_memory_size();

        return _heap_end;
    }

    uint64 Pcb::shrink_heap(uint64 new_end)
    {
        if (new_end >= _heap_end || new_end < _heap_start)
        {
            return _heap_end;
        }

        // 释放多余的堆内存
        uint64 va_start = PGROUNDUP(new_end);
        uint64 va_end = PGROUNDUP(_heap_end);

        for (uint64 va = va_start; va < va_end; va += PGSIZE)
        {
            mem::Pte pte = _pt.walk(va, 0);
            if (!pte.is_null() && pte.is_valid())
            {
                mem::k_vmm.vmunmap(_pt, va, 1, 1);
            }
        }

        _heap_end = new_end;

        // 更新总内存大小
        update_total_memory_size();

        return _heap_end;
    }

    /****************************************************************************************
     * 内存大小计算方法实现
     ****************************************************************************************/
    void Pcb::update_total_memory_size()
    {
        _sz = calculate_total_memory_size();
    }

    uint64 Pcb::calculate_total_memory_size() const
    {
        uint64 total = 0;

        // 计算所有程序段的大小
        for (int i = 0; i < _prog_section_cnt; i++)
        {
            total += _prog_sections[i]._sec_size;
        }

        // 加上堆的大小
        total += get_heap_size();

        return total;
    }

    /****************************************************************************************
     * 调试和信息打印方法实现
     ****************************************************************************************/
    void Pcb::print_memory_info() const
    {
        printfCyan("=== Process Memory Information ===\n");
        printfCyan("Process: %s (PID: %d)\n", _name, _pid);
        printfCyan("Total Memory Size: %lu bytes (%.2f KB)\n", _sz, (double)_sz / 1024.0);

        printfBlue("--- Program Sections ---\n");
        if (_prog_section_cnt == 0)
        {
            printfBlue("No program sections\n");
        }
        else
        {
            uint64 total_prog_memory = 0;
            for (int i = 0; i < _prog_section_cnt; i++)
            {
                printfBlue("  Section %d: %s\n", i, _prog_sections[i]._debug_name ? _prog_sections[i]._debug_name : "unnamed");
                printfBlue("    Start: %p, Size: %lu bytes (%.2f KB)\n",
                           _prog_sections[i]._sec_start,
                           _prog_sections[i]._sec_size,
                           (double)_prog_sections[i]._sec_size / 1024.0);
                total_prog_memory += _prog_sections[i]._sec_size;
            }
            printfBlue("  Total Program Memory: %lu bytes (%.2f KB)\n",
                       total_prog_memory, (double)total_prog_memory / 1024.0);
        }

        printfGreen("--- Heap Information ---\n");
        if (_heap_start == _heap_end)
        {
            printfGreen("Heap not initialized or empty\n");
        }
        else
        {
            uint64 heap_size = get_heap_size();
            printfGreen("  Heap Start: %p\n", (void *)_heap_start);
            printfGreen("  Heap End: %p\n", (void *)_heap_end);
            printfGreen("  Heap Size: %lu bytes (%.2f KB)\n", heap_size, (double)heap_size / 1024.0);
        }
        
        printfMagenta("--- VMA Information (not counted in _sz) ---\n");
        if (_vma == nullptr) {
            printfMagenta("No VMA structure\n");
        } else {
            uint64 total_vma_memory = 0;
            int active_vmas = 0;
            for (int i = 0; i < NVMA; i++) {
                if (_vma->_vm[i].used) {
                    printfMagenta("  VMA %d:\n", i);
                    printfMagenta("    Address: %p, Length: %lu bytes (%.2f KB)\n", 
                                 (void*)_vma->_vm[i].addr, 
                                 _vma->_vm[i].len,
                                 (double)_vma->_vm[i].len / 1024.0);
                    printfMagenta("    Prot: %d, Flags: %d\n", _vma->_vm[i].prot, _vma->_vm[i].flags);
                    if (_vma->_vm[i].vfile) {
                        printfMagenta("    File: mapped\n");
                    } else {
                        printfMagenta("    File: anonymous\n");
                    }
                    total_vma_memory += _vma->_vm[i].len;
                    active_vmas++;
                }
            }
            if (active_vmas == 0) {
                printfMagenta("No active VMAs\n");
            } else {
                printfMagenta("  Total VMA Memory: %lu bytes (%.2f KB)\n", 
                             total_vma_memory, (double)total_vma_memory / 1024.0);
                printfMagenta("  Active VMAs: %d\n", active_vmas);
            }
        }

        printfYellow("--- Memory Summary ---\n");
        uint64 prog_total = get_total_program_memory();
        uint64 heap_total = get_heap_size();
        printfYellow("  Program Sections: %lu bytes (%.2f KB)\n", prog_total, (double)prog_total / 1024.0);
        printfYellow("  Heap: %lu bytes (%.2f KB)\n", heap_total, (double)heap_total / 1024.0);
        printfYellow("  Total (_sz): %lu bytes (%.2f KB)\n", _sz, (double)_sz / 1024.0);
        printfYellow("  Calculated Total: %lu bytes (%.2f KB)\n",
                     prog_total + heap_total, (double)(prog_total + heap_total) / 1024.0);

        if (_sz != (prog_total + heap_total))
        {
            printfRed("  WARNING: _sz does not match calculated total!\n");
        }

        printfCyan("=== End Memory Information ===\n");
    }

    bool Pcb::verify_memory_consistency() const
    {
        uint64 calculated_total = calculate_total_memory_size();
        bool consistent = (_sz == calculated_total);

        if (!consistent)
        {
            printfRed("Memory inconsistency detected in process %s (PID: %d)\n", _name, _pid);
            printfRed("  _sz: %lu, calculated: %lu\n", _sz, calculated_total);
            printfRed("  Note: VMA regions are managed separately and not counted in _sz\n");
            panic("proc verify_memory_consistency failed\n");
        }

        return consistent;
    }

}
