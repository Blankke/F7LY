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
#include "proc_manager.hh"
#include "process_memory_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include "prlimit.hh"

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
        _sgid = 0;  // 保存的设置组ID
        _fsgid = 0; // 文件系统组ID

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
        
        // CPU亲和性初始化：默认可以在任何CPU上运行
        _cpu_mask.fill(); // 设置所有可用CPU位

        /****************************************************************************************
         * 内存管理
         ****************************************************************************************/
        _kstack = 0;          // 内核栈虚拟地址
        _trapframe = nullptr; // 用户态寄存器保存区
        
        // 阶段1：创建统一内存管理器
        _memory_manager = nullptr; // 延迟到init()中创建，避免在构造函数中panic

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
                _sigmask = 0;      // 信号屏蔽掩码
        _signal = 0;       // 待处理信号掩码
        sig_frame = nullptr;   // 信号处理栈帧
        
        // 初始化信号栈
        _alt_stack.ss_sp = nullptr;
        _alt_stack.ss_flags = proc::ipc::signal::SS_DISABLE;
        _alt_stack.ss_size = 0;
        _on_sigstack = false;

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
        
        // 注意：不在init中创建ProcessMemoryManager
        // ProcessMemoryManager的创建延迟到具体需要时（fork、user_init、execve等）
        _memory_manager = nullptr;
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

    // 阶段1新增：清理ProcessMemoryManager
    void Pcb::cleanup_memory_manager()
    {
        if (_memory_manager != nullptr)
        {
            // 直接调用 free_all_memory()，它内部会检查和减少引用计数
            _memory_manager->free_all_memory();
            
            // free_all_memory() 减少了引用计数，如果原来的引用计数<=1，则资源已被释放
            // 现在检查当前引用计数，如果<=0则删除对象
            if (_memory_manager->get_ref_count() <= 0)
            {
                delete _memory_manager;
            }
            _memory_manager = nullptr;
        }
    }

    // 设置新的内存管理器
    void Pcb::set_memory_manager(ProcessMemoryManager* mm)
    {
        // 先清理当前的内存管理器
        cleanup_memory_manager();
        
        // 设置新的内存管理器
        _memory_manager = mm;
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
        // printf("map_kstack: pcb: global_id: %d, kstack start: %p end: %p\n", _global_id, _kstack, _kstack + KSTACK_SIZE);
        // 检查内核栈地址是否已经初始化
        if (_kstack == 0)
            panic("pcb was not init");

        // 为内核栈分配多个物理页
        for (uint i = 0; i < KSTACK_PAGES; i++)
        {
            char *pa = (char *)mem::k_pmm.alloc_page();
            if (pa == 0)
                panic("pcb map kstack: no memory");

            // 清零分配的物理页
            mem::k_pmm.clear_page((void *)pa);

            uint64 va = _kstack + i * PGSIZE;

#ifdef RISCV
            // RISC-V架构：映射内核栈页面，设置可读可写权限
            if (!mem::k_vmm.map_pages(pt, va, PGSIZE, (uint64)pa,
                                      riscv::PteEnum::pte_readable_m |
                                          riscv::PteEnum::pte_writable_m))
                panic("kernel vm map failed");
#elif defined(LOONGARCH)
            // LoongArch架构：映射内核栈页面，设置相应的页表项权限
            if (!mem::k_vmm.map_pages(pt, va, PGSIZE, (uint64)pa,
                                      PTE_NX | PTE_P | PTE_W | PTE_MAT | PTE_D | PTE_PLV))
                panic("kernel vm map failed");
#endif
        }
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
     * 程序段管理方法实现 - 封装ProcessMemoryManager
     ****************************************************************************************/
    int Pcb::add_program_section(void *start, ulong size, const char *name)
    {
        if (_memory_manager)
        {
            return _memory_manager->add_program_section(start, size, name);
        }
        else
        {
            printfRed("add_program_section: _memory_manager is null\n");
            return -1;
        }
    }

    void Pcb::remove_program_section(int index)
    {
        if (_memory_manager)
        {
            _memory_manager->remove_program_section(index);
        }
        else
        {
            printfRed("remove_program_section: _memory_manager is null\n");
        }
    }

    void Pcb::clear_all_program_sections()
    {
        if (_memory_manager)
        {
            _memory_manager->clear_all_program_sections_data();
        }
        else
        {
            printfRed("clear_all_program_sections: _memory_manager is null\n");
        }
    }

    void Pcb::reset_memory_sections()
    {
        if (_memory_manager)
        {
            _memory_manager->reset_memory_sections();
        }
        else
        {
            printfRed("reset_memory_sections: _memory_manager is null\n");
        }
    }

    uint64 Pcb::get_total_program_memory() const
    {
        if (_memory_manager)
        {
            return _memory_manager->get_total_program_memory();
        }
        return 0;
    }

    void Pcb::copy_program_sections(const Pcb *src)
    {
        if (_memory_manager && src->_memory_manager)
        {
            _memory_manager->copy_program_sections(*src->_memory_manager);
        }
        else
        {
            printfRed("copy_program_sections: _memory_manager is null\n");
        }
    }

    /****************************************************************************************
     * 堆内存管理方法实现 - 封装ProcessMemoryManager
     ****************************************************************************************/
    void Pcb::init_heap(uint64 start_addr)
    {
        if (_memory_manager)
        {
            _memory_manager->init_heap(start_addr);
        }
        else
        {
            printfRed("init_heap: _memory_manager is null\n");
        }
    }

    uint64 Pcb::grow_heap(uint64 new_end)
    {
        if (_memory_manager)
        {
            return _memory_manager->grow_heap(new_end);
        }
        else
        {
            printfRed("grow_heap: _memory_manager is null\n");
            return 0;
        }
    }

    uint64 Pcb::shrink_heap(uint64 new_end)
    {
        if (_memory_manager)
        {
            return _memory_manager->shrink_heap(new_end);
        }
        else
        {
            printfRed("shrink_heap: _memory_manager is null\n");
            return 0;
        }
    }

    /****************************************************************************************
     * 内存大小计算方法实现 - 封装ProcessMemoryManager
     ****************************************************************************************/
    void Pcb::update_total_memory_size()
    {
        if (_memory_manager)
        {
            _memory_manager->update_total_memory_size();
        }
    }

    uint64 Pcb::calculate_total_memory_size() const
    {
        if (_memory_manager)
        {
            return _memory_manager->calculate_total_memory_size();
        }
        return 0;
    }

    
    bool Pcb::verify_memory_consistency()
    {
        if (_memory_manager)
        {
            return _memory_manager->verify_memory_consistency();
        }
        return true; // 没有内存管理器时认为是一致的
    }

    

    void Pcb::emergency_memory_cleanup()
    {
        if (_memory_manager)
        {
            _memory_manager->emergency_cleanup();
        }
    }

    bool Pcb::check_memory_leaks() const
    {
        if (_memory_manager)
        {
            return _memory_manager->check_memory_leaks();
        }
        return false;
    }

    void Pcb::print_detailed_memory_info() const
    {
        if (_memory_manager)
        {
            _memory_manager->print_memory_usage();
        }
        else
        {
            printfCyan("=== PCB Memory Information ===\n");
            printfCyan("Process: %s (PID: %d)\n", _name, _pid);
            printfCyan("ProcessMemoryManager: not present\n");
            printfCyan("=== End PCB Memory Information ===\n");
        }
    }

}
