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
#include "capability.hh"
#include "proc_manager.hh"
#include "process_memory_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include "prlimit.hh"
#include "shm_manager.hh"
#include "memlayout.hh"
#include "hal/tlb_shootdown.hh"

namespace proc
{
    uint max_open_files = fd_table_capacity;

    namespace
    {
#ifdef RISCV
        constexpr uint64 k_min_kernel_mm_ptr = KERNBASE;
#elif defined(LOONGARCH)
        constexpr uint64 k_min_kernel_mm_ptr = PHYSBASE;
#endif
        inline uint32 max_reasonable_file_refcnt()
        {
            return num_process * max_open_files;
        }

        inline bool is_kernel_mapped_range(uint64 addr, uint64 size)
        {
            if (addr < k_min_kernel_mm_ptr || size == 0)
            {
                return false;
            }

            uint64 end = addr + size - 1;
            if (end < addr)
            {
                return false;
            }

            return mem::k_pagetable.kwalk_addr(addr) != 0 &&
                   mem::k_pagetable.kwalk_addr(end) != 0;
        }

        inline bool is_probably_live_mm_object(ProcessMemoryManager *mm)
        {
            return mm != nullptr &&
                   is_kernel_mapped_range((uint64)mm, sizeof(ProcessMemoryManager));
        }

        inline bool is_probably_live_file_object(fs::file *file_obj)
        {
            if (file_obj == nullptr)
            {
                return false;
            }

            uint64 file_addr = (uint64)file_obj;
            if (!is_kernel_mapped_range(file_addr, sizeof(fs::file)))
            {
                return false;
            }

            // 先验证对象自身映射，再读取虚表与引用计数，避免坏指针直接触发页故障。
            uint64 vtable_addr = *(uint64 *)file_obj;
            if (!is_kernel_mapped_range(vtable_addr, sizeof(void *)))
            {
                return false;
            }

            uint32 refcnt = file_obj->refcnt;
            return refcnt > 0 && refcnt <= max_reasonable_file_refcnt();
        }
    }

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
        _cmdline.clear();                // NUL 分隔命令行

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
        memset(_supplementary_groups, 0, sizeof(_supplementary_groups));
        _supplementary_group_count = 0;
        k_capability.clear_all(*this);

        /****************************************************************************************
         * 进程状态和调度信息
         ****************************************************************************************/
        _state = UNUSED; // 进程状态初始化为未使用
        _chan.store(nullptr, eastl::memory_order_relaxed); // 睡眠等待通道
        _wait_channel_bucket = 0;
        _wait_channel_registered = false;
        _killed = 0;     // 进程终止标志
        _exiting = false; // 尚未进入退出清理流程
        _mm_cleanup_active = false;
        _deferred_reap = false;
        _xstate = 0;     // 进程退出状态码
        _parent_exit_signal = proc::ipc::signal::SIGCHLD;

        // 调度相关字段
        _slot = 0;                          // 时间片剩余量
        _priority = default_proc_prio;      // 默认 CPU 优先级
        _sched_policy = 0;                  // SCHED_OTHER
        _sched_priority = 0;
        _sched_reset_on_fork = false;
        _io_priority_override = default_proc_prio; // 默认块层优先级覆盖值
        _has_io_priority_override = false;  // 默认让块层优先级跟随 CPU nice
        
        // CPU亲和性初始化：默认可以在任何CPU上运行
        _cpu_mask.fill(); // 设置所有可用CPU位
        _last_cpu = 0;
        _running_cpu = -1;

        /****************************************************************************************
         * 内存管理
         ****************************************************************************************/
        _kstack = 0;          // 内核栈虚拟地址
        _trapframe = nullptr; // 用户态寄存器保存区
        _used_fpu = false;    // 默认按整数任务处理，第一次浮点指令异常后再启用 FPU 现场
        _used_lsx = false;    // LSX 与 FPR 共享低位，单独跟踪完整向量现场
        invalidate_trapframe_mapping_cache();
        
        // 阶段1：创建统一内存管理器
        _memory_manager = nullptr; // 延迟到init()中创建，避免在构造函数中panic

        /****************************************************************************************
         * 文件系统和I/O管理
         ****************************************************************************************/
        _cwd = nullptr;    // 当前工作目录的dentry指针
        _cwd_name.clear(); // 当前工作目录路径字符串
        _root_name = "/";
        _ofile = nullptr;  // 打开文件描述符表
        _umask = 0022;     // 默认umask值 (octal 022)
        _personality = 0;  // 默认 personality 为 PER_LINUX

        /****************************************************************************************
         * 线程和同步原语
         ****************************************************************************************/
        _futex_addr = nullptr;  // futex等待地址
        _futex_key.store(0, eastl::memory_order_relaxed); // futex匹配键
        _futex_private = false;
        _futex_bucket_index.store(0, eastl::memory_order_relaxed);
        _futex_timeout_deadline.store(0, eastl::memory_order_relaxed);
        _futex_wait_result = 0;
        _clear_tid_addr = 0;    // 线程退出时清除的地址
        _robust_list = nullptr; // 健壮futex链表头
        _robust_list_user_addr = 0; // 健壮futex链表头用户地址
        _vfork_parent = nullptr; // vfork 父进程等待通道

        /****************************************************************************************
         * 信号处理
         ****************************************************************************************/
        _sigactions = nullptr; // 信号处理函数表
        _sigmask = 0;          // 信号屏蔽掩码
        _signal = 0;           // 待处理信号掩码
        _siginfo_mask = 0;     // 默认没有附带 siginfo 的 pending signal
        memset(_queued_siginfo, 0, sizeof(_queued_siginfo));
        _sigsuspend_restore_pending = false;
        _sigsuspend_saved_sigmask = 0;
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
        _utime = 0;          // 用户态时间(用户态运行时间)
        _stime = 0;          // 系统态时间(内核态运行时间)
        _cutime = 0;         // 子进程用户态时间累计
        _cstime = 0;         // 子进程系统态时间累计
        _start_time = 0;     // 进程启动时间(绝对时间戳)
        _start_boottime = 0; // 自系统启动以来的启动时间
        _timens_current = {};
        _timens_children = {};
        _netns = {};
        _ipc_ns_id = k_initial_ipc_namespace_id;
        _mnt_ns_id = k_initial_mount_namespace_id;

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

        // 全局对象构造期无法依赖 Cpu 拓扑；进程池正式初始化发生在主核解析
        // DTB 之后，此处把默认亲和性收敛为本次启动真正可用的 CPU 集合。
        const uint64 possible_mask = Cpu::possible_cpu_mask();
        _cpu_mask = CpuMask{possible_mask != 0 ? possible_mask : 1};
        _last_cpu = 0;
        _running_cpu = -1;
        
        // 注意：不在init中创建ProcessMemoryManager
        // ProcessMemoryManager的创建延迟到具体需要时（fork、user_init、execve等）
        invalidate_trapframe_mapping_cache();
        _memory_manager = nullptr;
    }

    mem::PageTable *Pcb::get_pagetable()
    {
        return _memory_manager ? &_memory_manager->pagetable : nullptr;
    }

    const mem::PageTable *Pcb::get_pagetable() const
    {
        return _memory_manager ? &_memory_manager->pagetable : nullptr;
    }

    void Pcb::set_pagetable(const mem::PageTable &pt)
    {
        if (_memory_manager != nullptr)
        {
            invalidate_trapframe_mapping_cache();
            _memory_manager->pagetable = pt;
        }
    }

    bool Pcb::get_shared_vm() const
    {
        return _memory_manager ? _memory_manager->shared_vm : false;
    }

    void Pcb::set_shared_vm(bool shared)
    {
        if (_memory_manager != nullptr)
        {
            _memory_manager->shared_vm = shared;
        }
    }

    VMA *Pcb::get_vma()
    {
        return _memory_manager ? &_memory_manager->vma_data : nullptr;
    }

    const VMA *Pcb::get_vma() const
    {
        return _memory_manager ? &_memory_manager->vma_data : nullptr;
    }

    void Pcb::set_vma(VMA *vma)
    {
        if (vma != nullptr && _memory_manager != nullptr)
        {
            _memory_manager->vma_data = *vma;
            _memory_manager->rebuild_vma_index();
        }
    }

    int Pcb::get_prog_section_count() const
    {
        return _memory_manager ? _memory_manager->prog_section_count : 0;
    }

    const program_section_desc *Pcb::get_prog_sections() const
    {
        return _memory_manager ? _memory_manager->prog_sections : nullptr;
    }

    program_section_desc *Pcb::get_prog_sections()
    {
        return _memory_manager ? _memory_manager->prog_sections : nullptr;
    }

    void Pcb::set_prog_section_count(int count)
    {
        if (_memory_manager != nullptr)
        {
            _memory_manager->prog_section_count = count;
        }
    }

    uint64 Pcb::get_heap_start() const
    {
        return _memory_manager ? _memory_manager->heap_start : 0;
    }

    uint64 Pcb::get_heap_end() const
    {
        return _memory_manager ? _memory_manager->heap_end : 0;
    }

    uint64 Pcb::get_heap_size() const
    {
        return _memory_manager ? (_memory_manager->heap_end - _memory_manager->heap_start) : 0;
    }

    uint64 Pcb::get_mmap_cursor() const
    {
        return _memory_manager ? _memory_manager->mmap_cursor : 0;
    }

    void Pcb::set_heap_start(uint64 start_addr)
    {
        if (_memory_manager != nullptr)
        {
            _memory_manager->heap_start = start_addr;
        }
    }

    void Pcb::set_heap_end(uint64 end_addr)
    {
        if (_memory_manager != nullptr)
        {
            _memory_manager->heap_end = end_addr;
        }
    }

    void Pcb::set_mmap_cursor(uint64 next_addr)
    {
        if (_memory_manager != nullptr)
        {
            _memory_manager->mmap_cursor = next_addr;
        }
    }

    uint64 Pcb::get_size() const
    {
        return _memory_manager ? _memory_manager->get_total_memory_usage() : 0;
    }

    ofile *Pcb::ensure_ofile()
    {
        if (_ofile != nullptr)
        {
            return _ofile;
        }

        ofile *candidate = new ofile();
        if (candidate == nullptr)
        {
            return nullptr;
        }
        candidate->init("ofile");

        ofile *result = nullptr;
        const bool acquired_lock = _lock.acquire_unless_held();
        if (_ofile == nullptr)
        {
            _ofile = candidate;
            candidate = nullptr;
        }
        result = _ofile;
        if (acquired_lock)
        {
            _lock.release();
        }

        if (candidate != nullptr)
        {
            delete candidate;
        }
        return result;
    }

    sighand_struct *Pcb::ensure_sighand()
    {
        if (_sigactions != nullptr)
        {
            return _sigactions;
        }

        sighand_struct *candidate = new sighand_struct();
        if (candidate == nullptr)
        {
            return nullptr;
        }
        candidate->refcnt.store(1, eastl::memory_order_relaxed);
        memset(candidate->actions, 0, sizeof(candidate->actions));

        sighand_struct *result = nullptr;
        const bool acquired_lock = _lock.acquire_unless_held();
        if (_sigactions == nullptr)
        {
            _sigactions = candidate;
            candidate = nullptr;
        }
        result = _sigactions;
        if (acquired_lock)
        {
            _lock.release();
        }

        if (candidate != nullptr)
        {
            delete candidate;
        }
        return result;
    }

    void Pcb::cleanup_sighand()
    {
        if (_sigactions != nullptr)
        {
            // fetch_sub 的旧值唯一决定销毁者，避免两个退出 CPU 同时 delete。
            const int old_refcnt =
                _sigactions->refcnt.fetch_sub(1, eastl::memory_order_acq_rel);

            // 如果引用计数降到0或以下，释放所有资源
            if (old_refcnt <= 1)
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
        // mm 即将被释放或从 PCB 脱离；即使下面发现空/坏指针，也不能让
        // PCB 复用时继承旧页表的 trapframe 绑定。
        invalidate_trapframe_mapping_cache();
        if (_memory_manager != nullptr)
        {
            // 不能借用 _exiting 表示这个区间：execve 会在正常进程中
            // 销毁旧映像，registry pin 或文件后端清理又可能临时 sleep。
            // 调度器换回该内核清理上下文时必须跳过 enter_mm(old_mm)。
            if (_mm_cleanup_active)
            {
                panic("cleanup_memory_manager: recursive mm cleanup pid=%d tid=%d mm=%p",
                      _pid, _tid, _memory_manager);
            }
            _mm_cleanup_active = true;
            // 当前任务切换/退出前撤销本 CPU 的 mm 活跃标记；其它仍运行同一
            // CLONE_VM 地址空间的 CPU 继续保留在目标掩码中。
            if (Cpu::get_cpu()->get_cur_proc() == this)
            {
                hal::tlb::leave_mm(*_memory_manager);
            }
            if (!is_probably_live_mm_object(_memory_manager))
            {
                printfRed("[cleanup_memory_manager] 检测到异常 mm 指针，直接丢弃: pcb=%p pid=%d tid=%d mm=%p\n",
                          this, _pid, _tid, _memory_manager);
                _memory_manager = nullptr;
                _mm_cleanup_active = false;
                return;
            }

            if (_memory_manager->get_ref_count() <= 1)
            {
                // 只在最后一个地址空间持有者退出时清理非 VMA 管理的共享段附加记录（如 shmat）。
                shm::k_smm.detach_all_for_process(this, true, false);
            }

            ProcessMemoryManager *released_mm = _memory_manager;
            // 先从 PCB 摘掉指针，再原子归还引用。最终清理者会扫描进程池
            // 校验是否还有活跃持有者；若先 fetch_sub、后清指针，并发退出的
            // 倒数第二个线程会被误算成“引用计数漂移”，把已经归零的 mm
            // 重新抬成 1，最终无人负责销毁。
            _memory_manager = nullptr;
            // 只有本次 fetch_sub 从 1 降到 0 的调用者拥有删除权。不能在这里
            // 重新读取共享 ref_count，否则会删除另一颗 CPU 正在最终清理的 mm。
            if (released_mm->free_all_memory())
            {
                delete released_mm;
            }
            _mm_cleanup_active = false;
        }
    }

    // 设置新的内存管理器
    void Pcb::set_memory_manager(ProcessMemoryManager* mm)
    {
        // 先清理当前的内存管理器
        cleanup_memory_manager();
        
        // 设置新的内存管理器
        _memory_manager = mm;
        invalidate_trapframe_mapping_cache();
    }

    void Pcb::cleanup_ofile()
    {
        ofile *fd_table = nullptr;
        const bool acquired_lock = _lock.acquire_unless_held();
        fd_table = _ofile;
        _ofile = nullptr;
        if (acquired_lock)
        {
            _lock.release();
        }

        if (fd_table != nullptr)
        {
            if (!is_kernel_mapped_range((uint64)fd_table, sizeof(ofile)))
            {
                printfRed("[cleanup_ofile] 检测到异常 ofile 指针，直接丢弃: pcb=%p pid=%d ofile=%p\n",
                          this, _pid, fd_table);
                return;
            }

            fd_table->_lock.acquire();
            // 减少打开文件表的引用计数
            fd_table->_shared_ref_cnt--;

            // 如果引用计数降到0或以下，当前任务取得整张表的唯一销毁权。
            if (fd_table->_shared_ref_cnt <= 0)
            {
                fd_table->_lock.release();

                // CLONE_FILES 线程退出或失败回滚只归还一个表引用，并没有关闭
                // 线程组的 fd。只有最后一个持有者销毁整张表时，才按进程退出
                // 语义释放 POSIX record locks。
                fs::release_posix_record_locks_for_pid(_pid);

                // 每个 fd 槽位各持有一个 file 引用，逐槽释放即可。不能在 8 KiB
                // 内核栈上放置 1024 项临时数组，否则退出路径会直接踩穿栈。
                uint fd_scan_limit = fd_table->_highest_fd_plus_one;
                for (uint64 i = 0; i < fd_scan_limit; ++i)
                {
                    fs::file *file_obj = fd_table->_ofile_ptr[i];
                    fd_table->_ofile_ptr[i] = nullptr;
                    fd_table->_reserved[i] = false;
                    fd_table->_fl_cloexec[i] = false;
                    if (file_obj == nullptr)
                    {
                        continue;
                    }
                    if (!is_probably_live_file_object(file_obj))
                    {
                        printfRed("[cleanup_ofile] 检测到异常文件指针，直接丢弃: pcb=%p pid=%d fd=%d file=%p\n",
                                  this, _pid, (int)i, file_obj);
                        continue;
                    }
                    file_obj->free_file();
                }
                fd_table->_highest_fd_plus_one = 0;

                // 释放打开文件表结构本身
                delete fd_table;
            }
            else
            {
                fd_table->_lock.release();
            }
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

    int Pcb::get_io_priority()
    {
        _lock.acquire();
        int priority = _has_io_priority_override ? _io_priority_override : _priority;
        _lock.release();
        return priority;
    }

    void Pcb::set_io_priority_override(int priority)
    {
        if (priority < highest_proc_prio)
        {
            priority = highest_proc_prio;
        }
        else if (priority > lowest_proc_prio)
        {
            priority = lowest_proc_prio;
        }

        _lock.acquire();
        _io_priority_override = priority;
        _has_io_priority_override = true;
        _lock.release();
    }

    void Pcb::clear_io_priority_override()
    {
        _lock.acquire();
        _io_priority_override = _priority;
        _has_io_priority_override = false;
        _lock.release();
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

}
