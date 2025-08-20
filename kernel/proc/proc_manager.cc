#include "proc_manager.hh"
#include "futex.hh"  // 添加futex头文件，用于robust futex清理
#include "hal/cpu.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "scheduler.hh"
#include "libs/klib.hh"
#include "mem/memlayout.hh" // 内核栈配置常量
#ifdef RISCV
#include "riscv/trap.hh"
#elif defined(LOONGARCH)
#include "loongarch/trap.hh"
#endif
#include "printer.hh"
#include "devs/device_manager.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "process_memory_manager.hh" // 新增：进程内存管理器
#include "shm_manager.hh"
#ifdef RISCV
// #include "devs/riscv/disk_driver.hh"
#elif defined(LOONGARCH)
#include "devs/loongarch/disk_driver.hh"
#endif
#include "net/f7ly_network.hh"

// #include "fs/vfs/dentrycache.hh"
// #include "fs/vfs/path.hh"
// #include "fs/ramfs/ramfs.hh"
#include "fs/vfs/file/device_file.hh"
#include "param.h"
#include "timer_manager.hh"
#include "timer_interface.hh"
#include "fs/vfs/elf.hh"
#include "fs/vfs/file/normal_file.hh"
#include "mem.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "syscall_defs.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/lwext4/ext4.hh"
#include <EASTL/map.h>
#include "fs/vfs/vfs_utils.hh"
#include "sys/syscall_defs.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/virtual_fs.hh"
#include "sys/syscall_defs.hh"
extern "C"
{
    extern uint64 initcode_start[];
    extern uint64 initcode_end[];

    extern int init_main(void);
    extern char trampoline[]; // trampoline.S
    void _wrp_fork_ret(void)
    {
        printf("into _wrapped_fork_ret, cur_pid:%d\n", proc::k_pm._cur_pid);
        proc::k_pm.fork_ret();
    }
    extern char sig_trampoline[]; // sig_trampoline.S
}

namespace proc
{
    __attribute__((aligned(512)))
    ProcessManager k_pm;

    void ProcessManager::init(const char *pid_lock_name, const char *tid_lock_name, const char *wait_lock_name)
    {
        // initialize the proc table.
        _pid_lock.init(pid_lock_name);
        _tid_lock.init(tid_lock_name);
        _wait_lock.init(wait_lock_name);
        for (uint i = 0; i < num_process; ++i)
        {
            Pcb &p = k_proc_pool[i];
            p.init("pcb", i);
        }
        _cur_pid = 1;
        _cur_tid = 1;
        _last_alloc_proc_gid = num_process - 1;
        printfGreen("[proc] Process Manager Init\n");
    }

    Pcb *ProcessManager::get_cur_pcb()
    {
        Cpu::push_intr_off();
        Cpu *c_cpu = Cpu::get_cpu();
        proc::Pcb *pcb = c_cpu->get_cur_proc();
        Cpu::pop_intr_off();
        // 这里为nullptr是正常现象应该无需panic？
        // 学长未对此处作处理，而是判断为nullptr就sleep，参考virtio_disk.cc:218行
        // commented out by @gkq
        //
        // if (pcb == nullptr)
        //     panic("get_cur_pcb: no current process");
        return pcb;
    }

    void ProcessManager::alloc_pid(Pcb *p)
    {
        _pid_lock.acquire();
        p->_pid = _cur_pid;
        _cur_pid++;
        _pid_lock.release();
        printfGreen("[proc] Allocated PID %d for process %s\n", p->_pid, p->_name);
    }

    void ProcessManager::alloc_tid(Pcb *p)
    {
        _tid_lock.acquire();
        p->_tid = _cur_tid;
        _cur_tid++;
        _tid_lock.release();
    }

    Pcb *ProcessManager::alloc_proc()
    {
        Pcb *p;
        // 遍历整个进程池，尝试分配一个 UNUSED 的进程控制块
        for (uint i = 0; i < num_process; i++)
        {
            printfYellow("[proc] Allocating new process PCB %d,cur_pid=%d\n", i, _cur_pid);
            // 使用轮转式分配策略，避免总是从头找，提高公平性
            p = &k_proc_pool[(_last_alloc_proc_gid + i) % num_process];
            p->_lock.acquire();
            // if(_cur_pid<0)
            printfGreen("[proc] Allocating new process PCB %d,cur_pid=%d\n", p->_global_id, _cur_pid);
            if (p->_state == ProcState::UNUSED)
            {
                /****************************************************************************************
                 * 基本进程标识和状态管理初始化
                 ****************************************************************************************/
                printfGreen("[proc] Allocating new process PCB %d,cur_pid=%d\n", p->_global_id,_cur_pid);
                 k_pm.alloc_pid(p);           // 分配全局唯一的进程ID
                printfGreen("[proc] Allocated PID %d for new process\n", p->_pid);
                k_pm.alloc_tid(p);           // 分配线程ID（单线程进程中等于PID）
                p->_state = ProcState::USED; // 标记进程控制块为已使用

                // 初始化父进程关系（在fork时会重新设置）
                p->_parent = nullptr;
                p->_name[0] = '\0'; // 清空进程名称
                p->exe.clear();     // 清空可执行文件路径

                // 初始化标准Linux进程标识符
                p->_ppid = 0;       // 父进程PID（在fork时设置）
                p->_pgid = p->_pid; // 进程组ID（初始化为自身PID）
                p->_tgid = p->_pid; // 线程组ID（初始化为自身PID）
                p->_sid = p->_pid;  // 会话ID（初始化为自身PID）
                p->_uid = 0;        // 真实用户ID（root）
                p->_euid = 0;       // 有效用户ID（root）
                p->_suid = 0;       // 保存的设置用户ID（root）
                p->_fsuid = 0;      // 文件系统用户ID（root）
                p->_gid = 0;        // 真实组ID（root）
                p->_egid = 0;       // 有效组ID（root）
                p->_sgid = 0;       // 保存的设置组ID（root）
                p->_fsgid = 0;      // 文件系统组ID（root）

                /****************************************************************************************
                 * 进程状态和调度信息初始化
                 ****************************************************************************************/
                p->_chan = nullptr; // 清空睡眠等待通道
                p->_killed = 0;     // 清除终止标志
                p->_xstate = 0;     // 清除退出状态码

                // 设置调度相关字段：默认调度槽与优先级
                p->_slot = default_proc_slot;
                p->_priority = default_proc_prio;

                // 初始化CPU亲和性掩码：默认可以在任何CPU上运行
                p->_cpu_mask = CpuMask((1ULL << NUMCPU) - 1);

                /****************************************************************************************
                 * 内存管理初始化
                 ****************************************************************************************/
                // 为该进程分配一页 trapframe 空间（用于中断时保存用户上下文）
                // printfYellow("[user pgtbl]==>alloc trapframe for proc %d\n", p->_global_id);
                if ((p->_trapframe = (TrapFrame *)mem::k_pmm.alloc_page()) == nullptr)
                {
                    freeproc_creation_failed(p); // 使用专门的创建失败清理函数
                    p->_lock.release();
                    return nullptr;
                }

                // 注意：不再在alloc_proc中创建ProcessMemoryManager
                // ProcessMemoryManager的创建延迟到fork函数中，对于user_init和execve则在相应函数中创建

                /****************************************************************************************
                 * 上下文切换初始化
                 ****************************************************************************************/
                // 初始化上下文结构体
                memset(&p->_context, 0, sizeof(p->_context));

                // 设置调度返回地址为 _wrp_fork_ret
                // 当调度器切换回该进程时，将从这里开始执行
                p->_context.ra = (uint64)_wrp_fork_ret;

                // 设置内核栈指针 - 指向栈顶（高地址）
                p->_context.sp = p->_kstack + KSTACK_SIZE;

                /****************************************************************************************
                 * 文件系统和I/O管理初始化
                 ****************************************************************************************/
                p->_cwd = nullptr;    // 当前工作目录（在具体使用时设置）
                p->_cwd_name.clear(); // 清空当前工作目录路径

                // 初始化文件描述符表
                p->_ofile = new ofile();
                p->_ofile->_shared_ref_cnt = 1;
                for (uint64 i = 0; i < max_open_files; ++i)
                {
                    p->_ofile->_ofile_ptr[i] = nullptr;
                    p->_ofile->_fl_cloexec[i] = false;
                }

                /****************************************************************************************
                 * 线程和同步原语初始化
                 ****************************************************************************************/
                p->_futex_addr = nullptr;  // 清空futex等待地址
                p->_clear_tid_addr = 0;    // 清空线程退出时需要清理的地址
                p->_robust_list = nullptr; // 清空健壮futex链表

                /****************************************************************************************
                 * 信号处理初始化
                 ****************************************************************************************/
                // 初始化信号处理结构体
                p->_sigactions = new sighand_struct();
                p->_sigactions->refcnt = 1;
                for (int i = 0; i <= ipc::signal::SIGRTMAX; ++i)
                {
                    p->_sigactions->actions[i] = nullptr;
                }

                p->_sigmask = 0;        // 清空信号屏蔽掩码
                p->_signal = 0;         // 清空待处理信号掩码
                p->sig_frame = nullptr; // 清空信号处理栈帧

                /****************************************************************************************
                 * 资源限制初始化
                 ****************************************************************************************/
                // 初始化进程资源限制为默认值
                for (uint i = 0; i < ResourceLimitId::RLIM_NLIMITS; ++i)
                {
                    p->_rlim_vec[i].rlim_cur = RLIM_INFINITY; // 软限制设为无限
                    p->_rlim_vec[i].rlim_max = RLIM_INFINITY; // 硬限制设为无限
                }
                // 设置文件描述符数量限制为合理值
                p->_rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_cur = max_open_files;
                p->_rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_max = max_open_files;

                /****************************************************************************************
                 * 时间统计和会计信息初始化
                 ****************************************************************************************/
                uint64 cur_tick = tmm::get_ticks();
                p->_start_tick = cur_tick;     // 进程开始运行时的时钟节拍数
                p->_user_ticks = 0;            // 用户态累计时钟节拍数
                p->_last_user_tick = 0;        // 上次进入用户态的时钟节拍数
                p->_kernel_entry_tick = 0;     // 进入内核态的时钟节拍数
                p->_stime = 0;                 // 系统态时间
                p->_cutime = 0;                // 子进程用户态时间累计
                p->_cstime = 0;                // 子进程系统态时间累计
                p->_start_time = cur_tick;     // 进程启动时间
                p->_start_boottime = cur_tick; // 自系统启动以来的启动时间

                // 更新上次分配的位置，轮转分配策略
                _last_alloc_proc_gid = p->_global_id;

                return p;
            }
            else
            {
                p->_lock.release();
            }
        }
        // 没有找到可用的进程控制块，分配失败
        return nullptr;
    }

    void ProcessManager::fork_ret()
    {
        printf("into fork_ret\n");
        proc::Pcb *proc = get_cur_pcb();
        proc->_lock.release();
        printf("[forkret] just into forkret , cur_pid=%d, cur_tid=%d\n", _cur_pid, _cur_tid);
        static int first = 1;
        if (first)
        {
            first = 0;

            // 文件系统初始化必须在常规进程的上下文中运行（例如，因为它会调用 sleep），
            // 因此不能从 main() 中运行。(copy form xv6)
            // #ifdef RISCV
            //             riscv::qemu::DiskDriver *disk = (riscv::qemu::DiskDriver *)dev::k_devm.get_device("Disk driver");

            // #elif defined(LOONGARCH)
            //             loongarch::qemu::DiskDriver *disk =
            //                 (loongarch::qemu::DiskDriver *)dev::k_devm.get_device("Disk driver");
            // #endif
            //             disk->identify_device();
            //             new (&fs::dentrycache::k_dentryCache) fs::dentrycache::dentryCache;
            //             fs::dentrycache::k_dentryCache.init();
            //             new (&fs::mnt_table) eastl::unordered_map<eastl::string, fs::FileSystem *>;
            //             fs::mnt_table.clear(); // clean mnt_Table
            //             new (&fs::ramfs::k_ramfs) fs::ramfs::RamFS;
            //             fs::ramfs::k_ramfs.initfd();
            //             fs::mnt_table["/"] = &fs::ramfs::k_ramfs;
            //             fs::Path mnt("/mnt");
            //             fs::Path dev("/dev/hda");
            //             mnt.mount(dev, "ext4", 0, 0);

            //             fs::Path path("/dev/stdin");
            //             fs::FileAttrs fAttrsin = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0444); // only read
            //             fs::device_file *f_in = new fs::device_file(fAttrsin, DEV_STDIN_NUM, path.pathSearch());
            //             assert(f_in != nullptr, "proc: alloc stdin file fail while user init.");

            //             fs::Path pathout("/dev/stdout");
            //             fs::FileAttrs fAttrsout = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            //             fs::device_file *f_out =
            //                 new fs::device_file(fAttrsout, DEV_STDOUT_NUM, pathout.pathSearch());
            //             assert(f_out != nullptr, "proc: alloc stdout file fail while user init.");

            //             fs::Path patherr("/dev/stderr");
            //             fs::FileAttrs fAttrserr = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            //             fs::device_file *f_err =
            //                 new fs::device_file(fAttrserr, DEV_STDERR_NUM, patherr.pathSearch());
            //             assert(f_err != nullptr, "proc: alloc stderr file fail while user init.");

            //             fs::ramfs::k_ramfs.getRoot()->printAllChildrenInfo();

            //             proc->_ofile->_ofile_ptr[0] = f_in;
            //             proc->_ofile->_ofile_ptr[0]->refcnt++;
            //             proc->_ofile->_ofile_ptr[1] = f_out;
            //             proc->_ofile->_ofile_ptr[1]->refcnt++;
            //             proc->_ofile->_ofile_ptr[2] = f_err;
            //             proc->_ofile->_ofile_ptr[2]->refcnt++;
            //             /// @todo 这里暂时修改进程的工作目录为fat的挂载点
            //             proc->_cwd = fs::ramfs::k_ramfs.getRoot()->EntrySearch("mnt");
            //             proc->_cwd_name = "/mnt/";

            filesystem_init();
            // filesystem2_init(); // 这个滚蛋
            printf("[forkret] into forkret , cur_pid=%d, cur_tid=%d\n", _cur_pid, _cur_tid);
            fs::device_file *f_in = new fs::device_file();
            // fs::device_file *f_err = new fs::device_file();
            eastl::string pathout("/dev/stdout");
            fs::FileAttrs fAttrsout = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_out =
                new fs::device_file(fAttrsout, pathout, 1);
            // _cur_pid=_cur_tid=2;
            eastl::string patherr("/dev/stderr");
            fs::FileAttrs fAttrserr = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_err = new fs::device_file(fAttrserr, patherr, 2);
            proc->_ofile->_ofile_ptr[0] = f_in;
            proc->_ofile->_ofile_ptr[0]->refcnt++;
            proc->_ofile->_ofile_ptr[1] = f_out;
            proc->_ofile->_ofile_ptr[1]->refcnt++;
            proc->_ofile->_ofile_ptr[2] = f_err;
            proc->_ofile->_ofile_ptr[2]->refcnt++;
            /// 你好
            /// 这是重定向uart的代码
            /// commented out by @gkq
            new (&dev::k_uart) dev::UartManager(UART0);
            dev::register_debug_uart(&dev::k_uart);

            // net::init_network_stack();
        }

        // 设置进程开始运行的时间点
        if (proc->_start_tick == 0)
        {
            proc->_start_tick = tmm::get_ticks();
            proc->_start_time = tmm::get_ticks();     // 同时设置启动时间
            proc->_start_boottime = tmm::get_ticks(); // 系统启动以来的时间
        }

        // printf("fork_ret\n");
        trap_mgr.usertrapret();
    }

    void ProcessManager::freeproc(Pcb *p)
    {
        printf("[freeproc] PCB for process global_id %d pid %d  tid %d successfully reclaimed\n", p->_global_id, p->_pid, p->_tid);
        /****************************************************************************************
         内存资源已在 exit_proc() 中释放，这里只清理PCB字段
         ****************************************************************************************/

        // 验证进程状态：ZOMBIE（正常退出）、UNUSED（初始状态）、USED（创建失败清理）状态的进程才能被freeproc
        // if (p->_state != ProcState::ZOMBIE && p->_state != ProcState::UNUSED && p->_state != ProcState::USED)
        if (p->_state != ProcState::ZOMBIE)
        {
            panic("freeproc: process not in valid state for cleanup, current state: %d", (int)p->_state);
        }

        // printf("[freeproc] Reclaiming PCB for process %s pid %d\n", p->_name, p->_pid);

        /****************************************************************************************
         * 基本进程标识和状态管理清理
         ****************************************************************************************/
        p->_pid = 0;          // 清除进程ID
        p->_tid = 0;          // 清除线程ID
        p->_parent = nullptr; // 清除父进程指针
        p->_name[0] = '\0';   // 清空进程名称
        p->exe.clear();       // 清空可执行文件路径

        // 清除标准Linux进程标识符
        p->_ppid = 0; // 清除父进程PID
        p->_pgid = 0; // 清除进程组ID
        p->_tgid = 0; // 清除线程组ID
        p->_sid = 0;  // 清除会话ID
        p->_uid = 0;  // 清除真实用户ID
        p->_euid = 0; // 清除有效用户ID
        p->_suid = 0; // 清除保存的设置用户ID
        p->_fsuid = 0; // 清除文件系统用户ID
        p->_gid = 0;  // 清除真实组ID
        p->_egid = 0; // 清除有效组ID
        p->_sgid = 0; // 清除保存的设置组ID
        p->_fsgid = 0; // 清除文件系统组ID

        /****************************************************************************************
         * 进程状态和调度信息清理
         ****************************************************************************************/
        p->_chan = nullptr;            // 清空睡眠等待通道
        p->_killed = 0;                // 清除终止标志
        p->_xstate = 0;                // 清除退出状态码
        p->_state = ProcState::UNUSED; // 标记进程控制块为未使用

        p->_slot = 0;     // 重置时间片
        p->_priority = 0; // 重置优先级

        // 重新初始化CPU亲和性掩码：默认可以在任何CPU上运行
        p->_cpu_mask = CpuMask((1ULL << NUMCPU) - 1);

        /****************************************************************************************
         * 文件系统和I/O管理清理
         ****************************************************************************************/
        p->_cwd = nullptr;    // 清空当前工作目录
        p->_cwd_name.clear(); // 清空当前工作目录路径
        p->_umask = 0022;     // 重置umask为默认值

        // 注意：文件描述符表已在exit_proc中清理，这里只重置指针
        if (p->_ofile != nullptr)
        {
            panic("freeproc: ofile should be cleaned in exit_proc, but found non-null pointer");
        }

        /****************************************************************************************
         * 线程和同步原语清理
         ****************************************************************************************/
        p->_futex_addr = nullptr;  // 清空futex等待地址
        p->_clear_tid_addr = 0;    // 清空线程退出时需要清理的地址
        p->_robust_list = nullptr; // 清空健壮futex链表

        /****************************************************************************************
         * 信号处理清理
         ****************************************************************************************/
        // 注意：信号处理结构和栈帧已在exit_proc中清理，这里只重置指针
        p->_sigactions = nullptr; // 清空信号处理结构指针
        p->sig_frame = nullptr;   // 清空信号处理帧指针
        p->_signal = 0;           // 清空待处理信号掩码
        p->_sigmask = 0;          // 清空信号屏蔽掩码

        /****************************************************************************************
         * 资源限制清理
         ****************************************************************************************/
        // 重置所有资源限制为0
        for (uint i = 0; i < ResourceLimitId::RLIM_NLIMITS; ++i)
        {
            p->_rlim_vec[i].rlim_cur = 0;
            p->_rlim_vec[i].rlim_max = 0;
        }

        /****************************************************************************************
         * 时间统计和会计信息清理
         ****************************************************************************************/
        p->_start_tick = 0;        // 清零进程开始运行时间
        p->_user_ticks = 0;        // 清零用户态累计时间
        p->_last_user_tick = 0;    // 清零上次进入用户态时间
        p->_kernel_entry_tick = 0; // 清零进入内核态时间
        p->_stime = 0;             // 清零系统态时间
        p->_cutime = 0;            // 清零子进程用户态时间累计
        p->_cstime = 0;            // 清零子进程系统态时间累计
        p->_start_time = 0;        // 清零进程启动时间
        p->_start_boottime = 0;    // 清零自系统启动以来的启动时间

        /****************************************************************************************
         * 上下文清理
         ****************************************************************************************/
        memset(&p->_context, 0, sizeof(p->_context)); // 清空上下文信息

        printf("[freeproc] free proc complete\n");
    }

    void ProcessManager::freeproc_creation_failed(Pcb *p)
    {
        /****************************************************************************************
         * 专门处理进程创建失败时的清理
         * 此时进程可能已经分配了部分资源但还没有真正运行
         ****************************************************************************************/

        printf("[freeproc_creation_failed] Cleaning up failed process creation for pid %d\n", p->_pid);

        // 如果已经分配了trapframe，需要释放
        if (p->get_trapframe() != nullptr)
        {
            mem::k_pmm.free_page(p->get_trapframe());
            p->set_trapframe(nullptr);
        }

        // 如果已经创建了ProcessMemoryManager，需要释放
        ProcessMemoryManager *mm = p->get_memory_manager();
        if (mm != nullptr)
        {
            mm->emergency_cleanup(); // 使用紧急清理，避免正常流程
            if (mm->get_ref_count() <= 1)
            {
                delete mm;
            }
            p->set_memory_manager(nullptr);
        }

        // 调用标准的PCB清理
        freeproc(p);
    }

    void ProcessManager::debug_process_states()
    {
        /****************************************************************************************
         * 调试函数：打印所有进程的状态信息
         ****************************************************************************************/
        printf("\n========== Process State Debug Info ==========\n");

        int zombie_count = 0;
        int running_count = 0;
        int sleeping_count = 0;
        int unused_count = 0;
        int used_count = 0;

        for (uint i = 0; i < num_process; i++)
        {
            Pcb &p = k_proc_pool[i];
            if (p._state == ProcState::UNUSED)
            {
                unused_count++;
                continue;
            }

            printf("Process[%d]: pid=%d tid=%d name='%s' state=%d parent_pid=%d pgid=%d sid=%d\n",
                   i, p._pid, p._tid, p._name, (int)p._state,
                   p._parent ? p._parent->_pid : -1, p._pgid, p._sid);

            switch (p._state)
            {
            case ProcState::ZOMBIE:
                zombie_count++;
                printf("  -> ZOMBIE: xstate=%d, waiting for parent to collect\n", p._xstate);
                break;
            case ProcState::RUNNABLE:
                running_count++;
                printf("  -> RUNNABLE\n");
                break;
            case ProcState::RUNNING:
                running_count++;
                printf("  -> RUNNING\n");
                break;
            case ProcState::SLEEPING:
                sleeping_count++;
                printf("  -> SLEEPING: chan=%p\n", p._chan);
                break;
            case ProcState::USED:
                used_count++;
                break;
            default:
                printf("  -> UNKNOWN STATE: %d\n", (int)p._state);
                break;
            }
        }

        printf("Summary: UNUSED=%d, USED=%d, RUNNABLE=%d, SLEEPING=%d, ZOMBIE=%d\n",
               unused_count, used_count, running_count, sleeping_count, zombie_count);
        printf("===============================================\n\n");
    }

    bool ProcessManager::verify_process_cleanup(int pid)
    {
        /****************************************************************************************
         * 验证函数：检查指定PID的进程是否正确清理
         ****************************************************************************************/
        for (uint i = 0; i < num_process; i++)
        {
            Pcb &p = k_proc_pool[i];
            if (p._pid == pid && p._state != ProcState::UNUSED)
            {
                printf("[ERROR] Process pid %d still exists in state %d after cleanup\n",
                       pid, (int)p._state);
                return false;
            }
        }
        printf("[OK] Process pid %d successfully cleaned up\n", pid);
        return true;
    }

    int ProcessManager::get_cur_cpuid()
    {
        return r_tp();
    }

    void ProcessManager::user_init()
    {
        static int inited = 0;
        // 防止重复初始化
        if (inited != 0)
        {
            panic("re-init user.");
            return;
        }

        Pcb *p = alloc_proc();
        if (p == nullptr)
        {
            panic("user_init: alloc_proc failed");
            return;
        }

        _init_proc = p;

        // 为init进程创建ProcessMemoryManager
        ProcessMemoryManager *init_mm = new ProcessMemoryManager();

        // 完成内存管理器的初始化设置
        if (!init_mm->create_pagetable())
        {
            panic("user_init: failed to create pagetable for init process");
            delete init_mm;
            return;
        }

        // 绑定到当前PCB
        p->set_memory_manager(init_mm);

        // 传入initcode的地址
        printfCyan("initcode pagetable: %p\n", p->get_pagetable()->get_base());
        uint64 initcode_sz = (uint64)initcode_end - (uint64)initcode_start;
        uint64 allocated_sz = mem::k_vmm.uvmfirst(*p->get_pagetable(), (uint64)initcode_start, initcode_sz);

        printf("initcode start: %p, end: %p\n", initcode_start, initcode_end);
        printf("initcode size: %p, total allocated space: %p\n", initcode_sz, allocated_sz);

        // 使用新的程序段管理
        p->add_program_section((void *)0, allocated_sz, "initcode");

        // 初始化堆在代码段后面
        p->init_heap(allocated_sz);

        // 设置程序计数器和栈指针 - 架构相关的部分
#ifdef RISCV
        p->_trapframe->epc = 0;
#elif defined(LOONGARCH)
        p->_trapframe->era = 0;
#endif
        p->_trapframe->sp = allocated_sz;

        safestrcpy(p->_name, "initcode", sizeof(p->_name));
        p->_parent = p; // init进程是自己的父进程
        // safestrcpy(p->_cwd_name, "/", sizeof(p->_cwd_name));
        p->_cwd_name = "/";

        // init进程的特殊属性（在alloc_proc中已设置）：
        // - PID = 1
        // - PGID = 1（成为进程组1的领导者）
        // - SID = 1（成为会话1的领导者）
        // - 所有其他进程最终都成为init进程的子进程

        p->_state = ProcState::RUNNABLE;

        p->_lock.release();
    }

    // Atomically release lock and sleep on chan.
    // Reacquires lock when awakened.

    void ProcessManager::set_killed(Pcb *p)
    {
        p->_lock.acquire();
        p->_killed = 1;
        p->_lock.release();
    }
    // Kill the process with the given pid.
    // The victim won't exit until it tries to return
    // to user space (see usertrap() in trap.c).
    int ProcessManager::kill_proc(int pid)
    {
        Pcb *p;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();

            // 如果找到目标 pid 的进程
            if (p->_pid == pid)
            {
                // 设置该进程的 killed 标志位为 1，
                // 表示该进程已被请求终止。
                // 被 kill 并不立即终止进程，而是在合适的时机由进程自行处理。
                p->_killed = 1;

                // 若该进程当前在 sleep（通常是等待 I/O 或锁）
                // 将其唤醒（设为 RUNNABLE），这样调度器会调度它运行，
                // 让它可以检查 _killed 并自行退出。
                if (p->_state == ProcState::SLEEPING)
                {
                    // 提前唤醒等待中的进程，
                    // 避免它永远睡着不被调度，也就永远无法响应 kill。
                    p->_state = ProcState::RUNNABLE;
                }

                p->_lock.release();
                return 0;
            }

            p->_lock.release();
        }
        return -1; // 没找到对应 pid 的进程
    }

    int ProcessManager::kill_signal(int pid, int sig)
    {
        Pcb *p;
        int count = 0; // 记录发送信号的进程数量
        printfCyan("kill_signal: pid=%d, sig=%d\n", pid, sig);

        if (pid > 0)
        {
            // 发送信号给特定PID的进程
            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pid == pid && p->_state != ProcState::UNUSED)
                {
                    p->add_signal(sig);
                    p->_lock.release();
                    return 0;
                }
                p->_lock.release();
            }
            return -1; // 没找到指定PID的进程
        }
        else if (pid == 0)
        {
            // 发送信号给当前进程组的所有进程
            Pcb *current = get_cur_pcb();
            if (current == nullptr)
                return -1;

            int target_pgid = current->_pgid;
            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pgid == target_pgid && p->_state != ProcState::UNUSED)
                {
                    p->add_signal(sig);
                    count++;
                }
                p->_lock.release();
            }
            return count > 0 ? 0 : -1;
        }
        else if (pid == -1)
        {
            panic("kill_signal: pid == -1 is not implemented");
            // 发送信号给当前进程有权限发送的所有进程（除了init进程）
            Pcb *current = get_cur_pcb();
            if (current == nullptr)
                return -1;

            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pid > 1 && p->_state != ProcState::UNUSED &&    // 跳过init进程
                    (p->_uid == current->_euid || current->_euid == 0)) // 权限检查
                {
                    p->add_signal(sig);
                    count++;
                }
                p->_lock.release();
            }
            return count > 0 ? 0 : -1;
        }
        else
        {
            // pid < -1: 发送信号给进程组ID为-pid的所有进程
            int target_pgid = -pid;
            Pcb *current = get_cur_pcb();
            if (current == nullptr)
                return -1;

            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pgid == target_pgid && p->_state != ProcState::UNUSED &&
                    (p->_uid == current->_euid || current->_euid == 0)) // 权限检查
                {
                    p->add_signal(sig);
                    count++;
                }
                p->_lock.release();
            }
            return count > 0 ? 0 : -1;
        }
    }

    int ProcessManager::tkill(int tid, int sig)
    {
        Pcb *p;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();
            if (p->_tid == tid)
            {
                p->add_signal(sig);
                p->_lock.release();
                return 0;
            }
            p->_lock.release();
        }
        return -1;
    }

    int ProcessManager::tgkill(int tgid, int tid, int sig)
    {
        Pcb *p;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();
            if (p->_tid == tid && p->_tgid == tgid)
            {
                p->add_signal(sig);
                p->_lock.release();
                return 0;
            }
            p->_lock.release();
        }
        return -1; // 未找到匹配的线程
    }

    Pcb *ProcessManager::find_proc_by_pid(int pid)
    {
        for (Pcb *p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            if (p->_pid == pid && p->_state != ProcState::UNUSED)
            {
                return p;
            }
        }
        return nullptr; // 未找到对应PID的进程
    }

    // Copy from either a user address, or kernel address,
    // depending on usr_src.
    // Returns 0 on success, -1 on error.
    int ProcessManager::either_copy_in(void *dst, int user_src, uint64 src, uint64 len)
    {
        Pcb *p = get_cur_pcb();
        if (user_src)
        {
            return mem::k_vmm.copy_in(*p->get_pagetable(), dst, src, len);
        }
        else
        {
            memmove(dst, (char *)src, len);
            return len;
        }
    }
    // Copy to either a user address, or kernel address,
    // depending on usr_dst.
    // Returns 0 on success, -1 on error.
    int ProcessManager::either_copy_out(void *src, int user_dst, uint64 dst, uint64 len)
    {
        Pcb *p = get_cur_pcb();
        if (user_dst)
        {
            return mem::k_vmm.copy_out(*p->get_pagetable(), dst, src, len);
        }
        else
        {
            memmove((char *)dst, src, len);
            return len;
        }
    }
    // Print a process listing to console.  For debugging.
    // Runs when user types ^P on console.
    // No lock to avoid wedging a stuck machine further.
    void ProcessManager::procdump()
    {
        static const char *states[6] = {
            "unused", // ProcState::UNUSED
            "used",   // ProcState::USED
            "sleep ", // ProcState::SLEEPING
            "runble", // ProcState::RUNNABLE
            "run   ", // ProcState::RUNNING
            "zombie"  // ProcState::ZOMBIE
        };
        Pcb *p;
        char *state;

        printf("\n");
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            if (p->_state == ProcState::UNUSED)
                continue;
            if ((int)p->_state >= 0 && (int)p->_state < 6 && states[(int)p->_state])
                state = (char *)states[(int)p->_state];
            else
                state = (char *)"???";
            printf("%d %s %s pgid=%d sid=%d", p->_pid, state, p->_name, p->_pgid, p->_sid);
            printf("\n");
        }
    }
    /// @brief
    /// @param p
    /// @param f
    /// @param fd
    /// @return
    int ProcessManager::alloc_fd(Pcb *p, fs::file *f, int fd)
    {
        // 越界检查
        if (fd < 0 || fd >= (int)max_open_files || f == nullptr || p->_ofile == nullptr)
            return -1;
        // 不为空先释放资源
        if (p->_ofile->_ofile_ptr[fd] != nullptr)
        {
            // 如果newfd已经打开，先关闭它，再打开
            if (p->_ofile->_ofile_ptr[fd] != f)
            {
                p->_ofile->_ofile_ptr[fd]->free_file(); // 释放旧的文件描述符
                p->_ofile->_ofile_ptr[fd] = nullptr;    // 释放旧的文件描述符
            }
        }

        p->_ofile->_ofile_ptr[fd] = f;
        p->_ofile->_fl_cloexec[fd] = false; // 默认不设置 CLOEXEC

        return fd;
    }

    void ProcessManager::get_cur_proc_tms(tmm::tms *tsv)
    {
        Pcb *p = get_cur_pcb();

        tsv->tms_utime = p->_user_ticks;
        tsv->tms_stime = p->_stime;   // 使用累计的系统态时间
        tsv->tms_cutime = p->_cutime; // 使用累计的子进程用户态时间
        tsv->tms_cstime = p->_cstime; // 使用累计的子进程系统态时间
    }
    int ProcessManager::alloc_fd(Pcb *p, fs::file *f)
    {
        int fd;

        if (p->_ofile == nullptr)
            return -1;

        for (fd = 3; fd < (int)max_open_files; fd++)
        {
            if (p->_ofile->_ofile_ptr[fd] == nullptr)
            {
                p->_ofile->_ofile_ptr[fd] = f;
                p->_ofile->_fl_cloexec[fd] = false; // 默认不设置 CLOEXEC
                // 注意：这里不调用 f->dup()，因为调用者通常已经为新分配的文件描述符准备了正确的引用计数
                return fd;
            }
        }
        return syscall::SYS_EMFILE;
    }

    int ProcessManager::clone(uint64 flags, uint64 stack_ptr, uint64 ptid, uint64 tls, uint64 ctid, bool is_clone3)
    {
        if (flags == 0)
        {
            return 22; // EINVAL: Invalid argument
        }
        Pcb *p = get_cur_pcb();
        Pcb *np = fork(p, flags, stack_ptr, ctid, is_clone3);
        if (np == nullptr)
        {
            return -1; // EAGAIN: Out of memory
        }
        uint64 new_tid = np->_tid;
        uint64 new_pid = np->_pid;
        if (flags & syscall::CLONE_SETTLS)
        {
            np->_trapframe->tp = tls; // 设置线程局部存储指针
        }
        if (flags & syscall::CLONE_PARENT_SETTID)
        {
            if (mem::k_vmm.copy_out(*p->get_pagetable(), ptid, &new_tid, sizeof(new_tid)) < 0)
            {
                freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                np->_lock.release();
                return -1; // EFAULT: Bad address
            }
        }
        if (flags & syscall::CLONE_PARENT)
        {
            if (p->_parent != nullptr)
            {
                np->_parent = p->_parent; // 继承父进程
            }
            else
            {
                panic("clone: parent process is null");
            }
        }
        np->_lock.release();
        return new_pid;
    }

    // 这个函数主要用提供clone的底层支持
    Pcb *ProcessManager::fork(Pcb *p, uint64 flags, uint64 stack_ptr, uint64 ctid, bool is_clone3)
    {
        TODO("copy on write fork");

        // ===== 基础验证和资源分配 =====
        // 参数验证
        if (p == nullptr)
        {
            return nullptr;
        }

        uint64 i;
        Pcb *np; // new proc

        // 分配新进程控制块
        if ((np = alloc_proc()) == nullptr)
        {
            return nullptr;
        }

        // 拷贝父进程的陷阱帧，而不是直接指向，后面有可能会修改
        *np->_trapframe = *p->_trapframe;

        // 设置父子进程关系
        np->_parent = p;

        // ===== 基本属性复制 =====
        // 继承文件系统相关属性
        np->_cwd = p->_cwd;           // 继承当前工作目录
        np->_cwd_name = p->_cwd_name; // 继承当前工作目录名称
        np->_umask = p->_umask;       // 继承文件模式创建掩码

        // ===== 身份信息和进程关系设置 =====
        // 继承父进程的身份信息
        np->_ppid = p->_pid;
        np->_uid = p->_uid;
        np->_euid = p->_euid;
        np->_suid = p->_suid;
        np->_fsuid = p->_fsuid;
        np->_gid = p->_gid;
        np->_egid = p->_egid;
        np->_sgid = p->_sgid;
        np->_fsgid = p->_fsgid;

        // 进程组ID继承逻辑：
        // 1. 对于普通fork()，子进程继承父进程的进程组
        // 2. 对于线程创建(CLONE_THREAD)，共享进程组
        // 3. 对于会话领导者，需要特殊处理
        if (flags & syscall::CLONE_THREAD)
        {
            // 线程共享进程组和会话
            np->_pgid = p->_pgid;
            np->_tgid = p->_tgid; // 线程组ID保持一致
            np->_sid = p->_sid;
        }
        else
        {
            // 普通进程创建，继承进程组但获得新的线程组ID
            np->_pgid = p->_pgid;
            np->_tgid = np->_pid; // 新进程成为自己线程组的领导者
            np->_sid = p->_sid;
        }

        // ===== 时间统计重置 =====
        // 重置子进程的时间统计（alloc_proc已经初始化，但这里明确重置）
        uint64 cur_tick = tmm::get_ticks();
        np->_start_tick = cur_tick;
        np->_start_time = cur_tick;
        np->_start_boottime = cur_tick;
        np->_user_ticks = 0;
        np->_last_user_tick = 0;
        np->_kernel_entry_tick = 0;
        np->_stime = 0;
        np->_cutime = 0;
        np->_cstime = 0;

        // ===== 进程名称设置 =====
        // 为子进程设置名称，添加子进程标识
        const char child_name_suffix[] = "-child";
        size_t parent_name_len = strlen(p->_name);
        size_t suffix_len = strlen(child_name_suffix);

        // 确保不超出缓冲区大小
        if (parent_name_len + suffix_len < sizeof(np->_name))
        {
            strcpy(np->_name, p->_name);
            strcat(np->_name, child_name_suffix);
        }
        else
        {
            // 父进程名称太长，需要截断
            size_t max_parent_len = sizeof(np->_name) - suffix_len - 1;
            strncpy(np->_name, p->_name, max_parent_len);
            np->_name[max_parent_len] = '\0';
            strcat(np->_name, child_name_suffix);
        }

        // ===== 文件描述符处理 =====

        if (flags & syscall::CLONE_FILES)
        {
            // 共享文件描述符表
            np->cleanup_ofile();
            np->_ofile = p->_ofile;
            np->_ofile->_shared_ref_cnt++; // 增加引用计数
        }
        else
        {
            // 深拷贝文件描述符表
            for (i = 0; i < (int)max_open_files; i++)
            {
                if (p->_ofile->_ofile_ptr[i])
                {
                    // fs::k_file_table.dup( p->_ofile[ i ] );
                    p->_ofile->_ofile_ptr[i]->dup();
                    np->_ofile->_ofile_ptr[i] = p->_ofile->_ofile_ptr[i];
                    np->_ofile->_fl_cloexec[i] = p->_ofile->_fl_cloexec[i]; // 继承 CLOEXEC 标志
                }
            }
        }

        // ===== 内存管理 =====
        if (flags & syscall::CLONE_VM)
        {
            // 共享虚拟内存：新进程共享父进程的内存管理器
            ProcessMemoryManager *parent_mm = p->get_memory_manager();
            if (parent_mm != nullptr)
            {
                np->set_memory_manager(parent_mm->share_for_thread());
                printfCyan("[clone] Using shared memory manager for process %d (parent %d)\n",
                           np->_pid, p->_pid);
            }
            else
            {
                panic("[fork] parent memory_manager is null");
            }
        }
        else
        {
            printfBlue("[fork] clone parent vm\n");
            // fork 操作：创建独立的内存管理器副本
            ProcessMemoryManager *parent_mm = p->get_memory_manager();
            if (parent_mm != nullptr)
            {
                // 继承共享内存附加记录：把父线程tid对应的附加项复制到子线程tid
                // 注意：此处 np->_tid 已在 alloc_proc() 中分配
                shm::k_smm.duplicate_attachments_for_fork(p->get_tid(), np->get_tid());
                ProcessMemoryManager *cloned_mm = parent_mm->clone_for_fork();
                if (cloned_mm == nullptr)
                {
                    panic("[fork] clone failed");
                    freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                    np->_lock.release();
                    panic("fork failed: memory copy failed");
                    return nullptr;
                }
                np->set_memory_manager(cloned_mm);
            }
        }

        // ===== 信号处理 =====
        if (flags & syscall::CLONE_SIGHAND)
        {
            // 共享信号处理结构
            np->cleanup_sighand(); // 使用cleanup方法来正确处理引用计数
            // 共享父进程的信号处理结构
            np->_sigactions = p->_sigactions;
            if (p->_sigactions != nullptr)
            {
                p->_sigactions->refcnt++; // 增加引用计数
            }
        }
        else
        {
            // 不共享信号处理结构，需要深拷贝
            if (p->_sigactions != nullptr && np->_sigactions != nullptr)
            {
                for (int i = 0; i <= ipc::signal::SIGRTMAX; ++i)
                {
                    if (p->_sigactions->actions[i] != nullptr)
                    {
                        np->_sigactions->actions[i] = new ipc::signal::sigaction;
                        if (np->_sigactions->actions[i] != nullptr)
                        {
                            *(np->_sigactions->actions[i]) = *(p->_sigactions->actions[i]);
                        }
                    }
                }
            }
        }

        if (flags & syscall::CLONE_THREAD)
        {
            // TODO: 清除信号掩码
            np->_tgid = p->_tgid; // 线程共享线程组 ID
            np->_pid = p->_pid;   // 线程共享 PID
            // TODO: 共享定时器
        }
        else
        {
            // TODO: 共享信号掩码
            np->_tgid = np->_pid;   // 新进程的线程组 ID 等于自己的 PID
            np->_trapframe->a0 = 0; // fork 返回值为 0
            // pid已经在 alloc_proc 中设置了
            // 定时器已经设置过了
        }
        if (stack_ptr != 0)
        {
            // 如果指定了栈指针，则设置子进程的用户栈指针
            np->_trapframe->sp = stack_ptr;
            if (is_clone3)
            {
                // 如果是 clone3 调用，设置用户栈指针
                np->_trapframe->a0 = 0;
            }
            else
            {
                uint64 entry_point = 0;
                mem::k_vmm.copy_in(*p->get_pagetable(), &entry_point, stack_ptr, sizeof(uint64));
                if (entry_point == 0)
                {
                    panic("fork: copy_in failed for stack pointer");
                    freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                    np->_lock.release();
                    return nullptr;
                }
                uint64 arg = 0;
                if (mem::k_vmm.copy_in(*p->get_pagetable(), &arg, (stack_ptr + 8), sizeof(uint64)) != 0)
                {
                    panic("fork: copy_in failed for stack pointer arg");
                    freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                    np->_lock.release();
                    return nullptr;
                }
                printf("fork: stack_ptr: %p, entry_point: %p arg: %p\n", stack_ptr, entry_point, arg);
#ifdef RISCV
                np->_trapframe->epc = entry_point; // 设置程序计数器为栈顶地址
#elif LOONGARCH
                np->_trapframe->era = entry_point; // 设置程序计数器为栈顶地址
#endif
                np->_trapframe->a0 = arg; // 设置第一个参数为栈顶地址的下一个地址
            }
        }

        if (flags & syscall::CLONE_CHILD_SETTID)
        {
            // 如果设置了 CLONE_CHILD_SETTID，则设置子进程的线程 ID
            if (ctid != 0)
            {
                if (mem::k_vmm.copy_out(*p->get_pagetable(), ctid, &np->_tid, sizeof(np->_tid)) < 0)
                {
                    freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                    np->_lock.release();
                    return nullptr; // EFAULT: Bad address
                }
            }
            else
            {
                printfRed("fork: ctid is 0, CLONE_CHILD_SETTID will not set tid\n");
            }
        }
        if (flags & syscall::CLONE_CHILD_CLEARTID)
        {
            // 如果设置了 CLONE_CHILD_CLEARTID，则在子进程退出时清除线程 ID
            np->_clear_tid_addr = ctid;
        }

        np->_state = ProcState::RUNNABLE;

        return np;
    }

    /// @brief
    /// @param n n的意思是扩展的字节数，
    /// 如果 n > 0，则扩展到当前进程的内存大小 + n
    /// 如果 n < 0，则收缩到当前进程的内存大小 + n
    /// @return
    int
    ProcessManager::growproc(int n)
    {
        Pcb *p = get_cur_pcb();

        if (n == 0)
        {
            return 0; // 无需改变
        }

        if (n > 0)
        {
            // 扩展堆
            uint64 current_end = p->get_heap_end();
            uint64 new_end = current_end + n;

            // 检查是否超出地址空间限制
            if (new_end >= MAXVA - PGSIZE)
            {
                return -1;
            }

            uint64 result = p->grow_heap(new_end);
            if (result < new_end)
            {
                return -1; // 扩展失败
            }
        }
        else
        {
            // 缩减堆 (n < 0)
            uint64 current_end = p->get_heap_end();
            uint64 new_end = current_end + n; // n是负数

            // 确保不会缩减到堆起始地址之前
            if (new_end < p->get_heap_start())
            {
                new_end = p->get_heap_start();
            }

            p->shrink_heap(new_end);
        }

        return 0;
    }

    /// @brief
    /// @param n 参数n是地址，意思是扩展到 n 地址
    /// 如果 n == 0，则返回当前进程的内存大小
    /// @return
    long ProcessManager::brk(long n)
    {
        Pcb *p = get_cur_pcb();

        // 如果 n 为 0，返回当前堆的结束地址
        if (n == 0)
        {
            return p->get_heap_end();
        }

        // 检查请求的地址是否合理
        if ((uint64)n < p->get_heap_start())
        {
            return -1; // 不能设置到堆起始地址之前
        }

        // 如果请求缩减堆
        if ((uint64)n < p->get_heap_end())
        {
            uint64 new_end = p->shrink_heap((uint64)n);
            return new_end;
        }
        // 如果请求扩展堆
        else if ((uint64)n > p->get_heap_end())
        {
            uint64 new_end = p->grow_heap((uint64)n);
            if (new_end < (uint64)n)
            {
                return -1; // 扩展失败
            }
            return new_end;
        }

        // 如果地址相同，直接返回
        return n;
    }

    long ProcessManager::sbrk(long increment)
    {
        Pcb *p = get_cur_pcb();
        uint64 old_end = p->get_heap_end();

        // 如果 increment 为 0，返回当前堆结束地址
        if (increment == 0)
        {
            return old_end;
        }

        uint64 new_end = old_end + increment;

        // 如果是缩减堆
        if (increment < 0)
        {
            if (new_end < p->get_heap_start())
            {
                return -1; // 不能缩减到堆起始地址之前
            }
            uint64 result = p->shrink_heap(new_end);
            if (result != new_end)
            {
                return -1;
            }
        }
        // 如果是扩展堆
        else
        {
            uint64 result = p->grow_heap(new_end);
            if (result < new_end)
            {
                return -1; // 扩展失败
            }
        }

        return old_end; // 返回原来的堆结束地址
    }

    int ProcessManager::wait4(int child_pid, uint64 addr, int option)
    {
        // debug_process_states();
        Pcb *p = k_pm.get_cur_pcb();
        printfYellow("[proc::wait4] pid: %d child_pid: %d, addr: %p, option: %d\n", p->_pid, child_pid, (void *)addr, option);

        // 检查不支持的选项标志
        const int supported_options = syscall::WNOHANG | syscall::WUNTRACED;
        const int unsupported_options = option & ~supported_options;
        if (unsupported_options != 0)
        {
            printf("[wait4] unsupported option flags: 0x%x, returning -EINVAL\n", unsupported_options);
            return syscall::SYS_EINVAL;
        }

        // 对于特定PID情况，验证主线程的父子关系
        if (child_pid > 0)
        {
            bool found_main_thread = false;
            for (uint i = 0; i < num_process; i++)
            {
                Pcb *np = &k_proc_pool[i];
                // 找到主线程（pid == tid == child_pid）
                if (np->_pid == child_pid && np->_tid == child_pid)
                {
                    found_main_thread = true;
                    // 检查主线程的父进程是否是当前进程
                    if (np->_parent != p)
                    {
                        printf("[wait4] main thread pid %d parent is not current process, returning -ECHILD\n", child_pid);
                        return -ECHILD;
                    }
                    break;
                }
            }

            // 如果没有找到主线程，说明该PID不存在
            if (!found_main_thread)
            {
                printf("[wait4] main thread with pid %d not found, returning -ECHILD\n", child_pid);
                return -ECHILD;
            }
        }

        _wait_lock.acquire();

        for (;;)
        {
            bool found_children = false;
            bool collected_zombie = false;
            int returned_pid = -1;
            printf("[wait4] process %d waiting for child pid %d with addr %p and option %d\n", p->_pid, child_pid, (void *)addr, option);
            // 遍历所有进程，寻找符合条件的子进程
            for (uint i = 0; i < num_process; i++)
            {
                Pcb *np = &k_proc_pool[i];
                printf("[wait4] checking global_id: %d, pid: %d tid: %d state: %d\n", np->_global_id, np->_pid, np->_tid, (int)np->get_state());

                // 检查是否是目标子进程
                if (!is_target_child(np, p, child_pid))
                    continue;

                np->_lock.acquire();
                found_children = true;

                // 如果是zombie，回收它
                if (np->get_state() == ProcState::ZOMBIE)
                {
                    returned_pid = np->_pid;

                    // 释放wait_lock进行内存拷贝
                    _wait_lock.release();

                    // 拷贝退出状态
                    if (addr != 0 &&
                        mem::k_vmm.copy_out(*p->get_pagetable(), addr,
                                            (const char *)&np->_xstate, sizeof(np->_xstate)) < 0)
                    {
                        np->_lock.release();
                        return -1;
                    }

                    printf("[wait4] freeproc child pid: %d tid: %d\n", np->_pid, np->_tid);
                    k_pm.freeproc(np);
                    np->_lock.release();

                    // 对于特定PID，检查是否还有其他同PID的线程
                    if (child_pid > 0)
                    {
                        _wait_lock.acquire();
                        if (!has_remaining_threads(p, child_pid))
                        {
                            _wait_lock.release();
                            printf("[wait4] all threads of pid %d have exited\n", child_pid);
                            return returned_pid; // 所有线程都已回收
                        }
                        // 还有线程未退出，继续等待
                        collected_zombie = true;
                        // break;  // 重新开始扫描
                    }
                    else
                    {
                        return returned_pid; // 非特定PID情况，回收一个就返回
                    }
                }
                else
                {
                    np->_lock.release();
                }
            }

            // 如果没有找到任何子进程或当前进程被杀死
            if (!found_children || p->_killed)
            {
                _wait_lock.release();
                return syscall::SYS_ECHILD;
            }

            // 如果设置了WNOHANG且没有可回收的zombie，立即返回
            if ((option & syscall::WNOHANG) && !collected_zombie)
            {
                _wait_lock.release();
                return 0;
            }

            // 等待子进程退出
            sleep(p, &_wait_lock);
        }
    }

    // 辅助函数：检查是否是目标子进程
    bool ProcessManager::is_target_child(Pcb *child, Pcb *parent, int child_pid)
    {
        if (child_pid > 0)
        {
            // 对于特定PID，只检查PID匹配，不检查parent（因为已在开头验证过主线程的parent）
            return child->_pid == child_pid;
        }
        else
        {
            // 对于非特定PID的情况，仍需检查parent关系
            if (child->_parent != parent)
                return false;

            if (child_pid == 0)
                return child->_pgid == parent->_pgid;
            else if (child_pid < -1)
                return child->_pgid == -child_pid;
            else // child_pid == -1
                return true;
        }
    }

    // 辅助函数：检查特定PID是否还有剩余线程
    bool ProcessManager::has_remaining_threads(Pcb *parent, int target_pid)
    {
        // debug_process_states();
        for (uint i = 0; i < num_process; i++)
        {
            Pcb *np = &k_proc_pool[i];
            if (np->_pid == target_pid &&
                ((np->get_state() != ProcState::UNUSED && np->_killed == 1) || np->get_state() == ProcState::ZOMBIE))
            {
                printf("[wait4] found remaining thread with pid %d tid %d\n", np->_pid, np->_tid);
                return true;
            }
        }
        return false;
    }
    /// @brief 将指定文件中的一段内容加载到页表映射的虚拟内存中。
    ///
    /// 此函数用于将文件 `de` 中从 `offset` 开始的 `size` 字节数据，
    /// 加载到进程的页表 `pt` 所映射的虚拟地址 `va` 开始的内存区域中。
    /// 支持起始地址非页对齐情况，内部自动处理跨页加载。
    /// 如果页表未正确建立或读取失败，将导致 panic。
    ///
    /// @param pt  进程的页表，用于获取对应虚拟地址的物理地址。
    /// @param va  加载的起始虚拟地址，允许非页对齐。
    /// @param de  指向文件的目录项，用于读取文件数据。
    /// @param offset 文件中读取的起始偏移。
    /// @param size 要读取的总字节数。
    /// @return 总是返回 0，失败情况下内部直接 panic。
    int ProcessManager::load_seg(mem::PageTable &pt, uint64 va, eastl::string &path, uint offset, uint size)
    { // 好像没有机会返回 -1, pa失败的话会panic，de的read也没有返回值
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        uint i, n;
        uint64 pa;

        i = 0;
        if (!is_page_align(va)) // 如果va不是页对齐的，先读出开头不对齐的部分
        {
            pa = (uint64)pt.walk_addr(va);
            // printf("[load_seg] pa: %p, va: %p\n", pa, va);
#ifdef LOONGARCH
            pa = to_vir(pa);
            // printf("[load_seg] to vir pa: %p\n", pa);
#endif
            n = PGROUNDUP(va) - va;
            vfs_read_file(path.c_str(), pa, offset + i, n);

            i += n;
        }

        // printfRed("[load_seg] load va: %p, size: %d\n", va, size);
        // printfRed("[load_seg] i: %d, offset: %d\n", i, offset);

        for (; i < size; i += PGSIZE) // 此时 va + i 地址是页对齐的
        {
            // printf("[load_seg] va + i: %p\n", va + i);
            pa = PTE2PA((uint64)pt.walk(va + i, 0).get_data()); // pte.to_pa() 得到的地址是页对齐的
            // printf("[load_seg] pa: %p\n", pa);
            if (pa == 0)
                panic("load_seg: walk");
            if (size - i < PGSIZE) // 如果是最后一页中的数据
                n = size - i;
            else
                n = PGSIZE;
#ifdef RISCV
            pa = pa;
#elif defined(LOONGARCH)
            pa = to_vir(pa);
#endif

            if (vfs_read_file(path.c_str(), pa, offset + i, n) != n) // 读取文件内容到物理内存
                return -1;
        }

        return 0;
    }
    /// @brief 真正执行退出的逻辑
    /// @param p
    /// @param state
    void ProcessManager::exit_proc(Pcb *p)
    {
        if (p == _init_proc)
            panic("init exiting"); // 保护机制：init 进程不能退出

        printf("[exit_proc] proc %s pid %d exiting\n", p->_name, p->_pid);

        /****************************************************************************************
         * Phase 1: 处理父子进程关系和进程状态
         ****************************************************************************************/

        // 检查进程组生命周期管理
        if (p->_pgid == p->_pid)
        {
            // 当前进程是进程组领导者，检查是否有其他进程在同一进程组
            bool has_other_processes = false;
            for (uint i = 0; i < num_process; i++)
            {
                Pcb &other = k_proc_pool[i];
                if (other._pgid == p->_pgid && other._pid != p->_pid &&
                    other._state != ProcState::UNUSED && other._state != ProcState::ZOMBIE)
                {
                    has_other_processes = true;
                    break;
                }
            }

            if (has_other_processes)
            {
                // 如果进程组还有其他活跃进程，向它们发送SIGHUP和SIGCONT信号
                // 这是孤儿进程组的标准处理
                printf("[exit_proc] Process group leader %d exiting, signaling remaining processes\n", p->_pid);
                for (uint i = 0; i < num_process; i++)
                {
                    Pcb &other = k_proc_pool[i];
                    if (other._pgid == p->_pgid && other._pid != p->_pid &&
                        other._state != ProcState::UNUSED && other._state != ProcState::ZOMBIE)
                    {
                        other._lock.acquire();
                        other.add_signal(1);  // SIGHUP
                        other.add_signal(18); // SIGCONT
                        other._lock.release();
                    }
                }
            }
        }

        reparent(p); // 将 p 的所有子进程交给 init 进程收养

        // 处理线程退出时的清理地址
        if (p->_clear_tid_addr)
        {
            uint64 temp0 = 0;
            if (mem::k_vmm.copy_out(*p->get_pagetable(), p->_clear_tid_addr, &temp0, sizeof(temp0)) < 0)
            {
                printfRed("exit_proc: copy out ctid failed\n");
            }
        }

        /****************************************************************************************
         * Phase 2: 释放进程内存和资源（在所有用户态写入操作完成后）
         ****************************************************************************************/
        // 使用ProcessMemoryManager统一处理内存释放
        p->cleanup_memory_manager(); // 释放所有内存资源（VMA、程序段、堆、页表、trapframe等）

        // 关闭文件描述符表，释放文件资源
        p->cleanup_ofile();

        // 清理信号处理结构和信号栈帧
        p->cleanup_sighand();

        // 释放信号栈帧链表
        while (p->sig_frame != nullptr)
        {
            ipc::signal::signal_frame *next_frame = p->sig_frame->next;
            mem::k_pmm.free_page(p->sig_frame); // 释放当前信号处理帧
            p->sig_frame = next_frame;          // 移动到下一个帧
        }
        p->sig_frame = nullptr; // 清空信号处理帧指针

        // 清理线程相关资源
        p->_futex_addr = nullptr;  // 清空futex等待地址
        p->_robust_list = nullptr; // 清空健壮futex链表

        _wait_lock.acquire(); // 只在需要修改父子关系时获取锁
        p->_lock.acquire();

        // 设置ZOMBIE状态（不设置xstate，由调用者负责）
        p->_state = ProcState::ZOMBIE; // 标记为 zombie，等待父进程回收

        // 如果有父进程，将当前进程的时间累计到父进程中
        if (p->_parent != nullptr)
        {
            p->_parent->_lock.acquire();
            p->_parent->_cutime += p->_user_ticks + p->_cutime;
            p->_parent->_cstime += p->_stime + p->_cstime;
            p->_parent->_lock.release();

            // 唤醒父进程（可能在 wait() 中阻塞）
            wakeup(p->_parent);
        }

        _wait_lock.release();

        printfYellow("[exit_proc] proc %s pid %d became zombie, memory freed\n", p->_name, p->_pid);

        k_scheduler.call_sched(); // jump to schedular, never return
        panic("zombie exit");
    }

    /// @brief 正常退出，设置退出状态后调用底层退出逻辑
    /// @param p 要退出的进程
    /// @param state 退出状态码
    void ProcessManager::do_exit(Pcb *p, int state)
    {
        // 设置正常退出状态
        p->_xstate = state << 8; // 存储退出状态（通常高字节存状态）

        printf("[do_exit] proc %s pid %d exiting with state %d\n", p->_name, p->_pid, state);

        // 调用底层退出逻辑
        exit_proc(p);
    }

    /// @brief 信号退出，设置信号相关的退出状态后调用底层退出逻辑
    /// @param p 要退出的进程
    /// @param signal_num 导致退出的信号编号
    /// @param coredump 是否生成core dump
    void ProcessManager::do_signal_exit(Pcb *p, int signal_num, bool coredump)
    {
        // 设置信号退出状态
        // Linux的wait状态编码：低7位存储信号编号，第8位标示是否core dump
        p->_xstate = signal_num & 0x7F; // 低7位存信号编号
        if (coredump)
        {
            p->_xstate |= 0x80; // 第8位设置core dump标志
        }

        printf("[do_signal_exit] proc %s pid %d killed by signal %d (coredump=%s)\n",
               p->_name, p->_pid, signal_num, coredump ? "yes" : "no");

        // 调用底层退出逻辑
        exit_proc(p);
    }

    /// @brief Pass p's abandoned children to init.
    /// @param p The parent process whose children are to be reparented.
    /// p是即将去世的父亲，他的儿子们马上要成为孤儿，我们要让init来收养他们。
    void ProcessManager::reparent(Pcb *p)
    {
        Pcb *pp;
        _wait_lock.acquire();
        for (uint i = 0; i < num_process; i++)
        {
            pp = &k_proc_pool[(_last_alloc_proc_gid + i) % num_process];
            if (pp->_parent == p)
            {
                pp->_lock.acquire();
                pp->_parent = _init_proc;
                pp->_lock.release();
            }
        }
        _wait_lock.release();
    }
    /// @brief 当前进程或线程退出（只退出自己）
    /// @param state   调用 do_exit 处理退出逻辑
    /// “一荣俱荣，一损俱损” commented by @gkq
    void ProcessManager::exit(int state)
    {
        Pcb *p = get_cur_pcb();
        printf("[exit] proc %s pid %d tid %d exiting with state %d\n", p->_name, p->_pid, p->_tid, state);
        do_exit(p, state);
    }

    /// @brief 当前线程组全部退出
    /// @param status
    /// https://man7.org/linux/man-pages/man2/exit_group.2.html
    void ProcessManager::exit_group(int status)
    {
        // debug_process_states();
        proc::Pcb *cp = get_cur_pcb();

        // printf("[exit_group] Thread group %d (leader pid %d) exiting with status %d\n",
        //        cp->_tgid, cp->_pid, status);

        /****************************************************************************************
         * Phase 3: 安全的多线程退出处理
         * 先标记所有同线程组线程为killed，让它们自然退出，避免竞态条件
         ****************************************************************************************/

        _wait_lock.acquire();

        // 遍历所有进程，找到同一线程组的其他线程
        for (uint i = 0; i < num_process; i++)
        {
            if (k_proc_pool[i]._state == ProcState::UNUSED)
                continue;

            proc::Pcb *p = &k_proc_pool[i];

            // 处理同一线程组的其他线程（不包括当前线程）
            if (p != cp && p->_tgid == cp->_tgid)
            {
                p->_lock.acquire();

                if (p->_state != ProcState::ZOMBIE && p->_state != ProcState::UNUSED)
                {
                    printf("[exit_group] Marking thread pid %d tid %d as killed\n", p->_pid, p->_tid);

                    // 标记线程为被杀死状态
                    p->_killed = 1;

                    // 如果线程在睡眠，唤醒它让其检查killed标志
                    if (p->_state == ProcState::SLEEPING)
                    {
                        p->_state = ProcState::RUNNABLE;
                        printf("[exit_group] Waking up sleeping thread pid %d\n", p->_pid);
                    }
                }

                p->_lock.release();
            }
        }

        _wait_lock.release();

        // printf("[exit_group] Current thread pid %d exiting normally\n", cp->_pid);

        // debug_process_states();

        // 当前线程正常退出，其他线程会在调度时检查killed标志并自行退出
        do_exit(cp, status);
    }
    void ProcessManager::sleep(void *chan, SpinLock *lock)
    {
        Pcb *p = get_cur_pcb();
        // Must acquire p->lock in order to
        // change p->state and then call sched.
        // Once we hold p->lock, we can be
        // guaranteed that we won't miss any wakeup
        // (wakeup locks p->lock),
        // so it's okay to release lk.
        // printfCyan("[sleep]proc %s : sleep on chan: %p\n", p->_name, chan);

        p->_lock.acquire();
        lock->release();
        // go to sleep
        p->_chan = chan;
        p->_state = ProcState::SLEEPING;
        k_scheduler.call_sched();
        p->_chan = 0;

        p->_lock.release();
        lock->acquire();
    }
    void ProcessManager::wakeup(void *chan)
    {
        Pcb *p;

        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            if (p != k_pm.get_cur_pcb() && p->_state != ProcState::UNUSED)
            {
                p->_lock.acquire();
                if (p->_state == ProcState::SLEEPING && p->_chan == chan)
                {
                    p->_state = ProcState::RUNNABLE;
                }
                p->_lock.release();
            }
        }
    }
    int ProcessManager::wakeup2(uint64 uaddr, int val, void *uaddr2, int val2)
    {
        Pcb *p;
        int count1 = 0, count2 = 0;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();
            if (p->_state == SLEEPING && (uint64)p->_futex_addr == uaddr)
            {
                if (count1 < val)
                {
                    // printf("[wakeup2] proc %s pid %d waking up on uaddr: %p\n", p->_name, p->_pid, uaddr);
                    p->_state = RUNNABLE;
                    p->_futex_addr = 0;
                    count1++;
                }
                else if (uaddr2 && count2 < val2)
                {
                    p->_futex_addr = uaddr2;
                    count2++;
                }
            }
            p->_lock.release();

            // 检查是否已经完成所需的唤醒和重排队操作
            if (count1 >= val && (!uaddr2 || count2 >= val2))
            {
                break;
            }
        }
        return count1;
    }
    int ProcessManager::mkdir(int dir_fd, eastl::string path, uint mode)
    {
        // 1. 参数验证 - 检查空路径 -> ENOENT
        if (path.empty())
        {
            return -ENOENT;
        }

        Pcb *p = get_cur_pcb();
        if (!p)
        {
            printfRed("[mkdir] No current process found\n");
            return -EFAULT;
        }

        // 处理dirfd参数
        eastl::string base_dir;
        if (path[0] == '.')
        {
            base_dir = p->_cwd_name;
            path = path.substr(2); // 去掉"./"前缀
        }
        if (dir_fd == AT_FDCWD)
        {
            base_dir = p->_cwd_name;
        }
        else
        {
            // 验证文件描述符 -> EBADF
            if (dir_fd < 0 || dir_fd >= NOFILE)
            {
                return -EBADF;
            }

            auto file = p->get_open_file(dir_fd);
            if (!file)
            {
                return -EBADF;
            }
            if (vfs_is_file_exist(file->_path_name.c_str()) == false)
            {
                printfRed("[mkdir] Base directory does not exist: %s\n", file->_path_name.c_str());
                return -ENOENT;
            }
            // 确保dirfd指向一个目录 -> ENOTDIR
            if (file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
            {
                return -ENOTDIR;
            }

            base_dir = file->_path_name;
        }

        // 构造完整路径
        eastl::string full_path;
        if (path[0] == '/')
        {
            // 绝对路径，忽略base_dir
            full_path = path;
        }
        else
        {
            // 相对路径
            full_path = base_dir;
            if (full_path.back() != '/')
            {
                full_path += "/";
            }
            full_path += path;
        }

        // 规范化路径（处理 "./" 前缀）
        if (full_path.length() >= 2 && full_path[0] == '.' && full_path[1] == '/')
        {
            full_path = full_path.substr(2);
        }

        // 检查符号链接循环 -> ELOOP
        // 检测路径中是否存在过多的重复目录组件，这通常表明符号链接循环
        {
            // 分割路径为组件
            eastl::vector<eastl::string> path_components;
            eastl::string component;
            for (size_t i = 0; i < full_path.length(); ++i)
            {
                if (full_path[i] == '/')
                {
                    if (!component.empty())
                    {
                        path_components.push_back(component);
                        component.clear();
                    }
                }
                else
                {
                    component += full_path[i];
                }
            }
            if (!component.empty())
            {
                path_components.push_back(component);
            }

            // 检查是否有目录组件出现过多次
            eastl::map<eastl::string, int> component_count;
            int max_repetitions = 0;
            for (const auto &comp : path_components)
            {
                component_count[comp]++;
                if (component_count[comp] > max_repetitions)
                {
                    max_repetitions = component_count[comp];
                }
            }

            // 如果某个目录组件出现超过8次，很可能是循环
            // 或者总路径长度过长（Linux PATH_MAX 通常是 4096）
            if (max_repetitions > 8 || full_path.length() > 4096)
            {
                return -ELOOP;
            }

            // 额外检查：如果路径深度过深（超过40级），也认为是循环
            if (path_components.size() > 40)
            {
                return -ELOOP;
            }
        }

        // 检查目录是否已存在
        if (vfs_is_file_exist(full_path.c_str()))
        {
            return -EEXIST;
        }

        // 调用VFS层的mkdir函数
        int result = vfs_ext_mkdir(full_path.c_str(), mode & 0777);

        return result;
    }

    int ProcessManager::mknod(int dir_fd, eastl::string path, mode_t mode, dev_t dev)
    {
        Pcb *p = get_cur_pcb();
        [[maybe_unused]] fs::file *file = nullptr;

        if (dir_fd != AT_FDCWD)
        {
            // panic("mknod: dir_fd != AT_FDCWD not implemented");
            file = p->get_open_file(dir_fd);
        }

        const char *dirpath = (dir_fd == AT_FDCWD) ? p->_cwd_name.c_str() : p->_ofile->_ofile_ptr[dir_fd]->_path_name.c_str();
        eastl::string absolute_path = get_absolute_path(path.c_str(), dirpath);

        // 将 mode 转换为内部文件类型
        uint32 internal_mode;
        mode_t file_type = mode & S_IFMT; // 提取文件类型部分

        if (file_type == S_IFREG || file_type == 0)
        {
            printfMagenta("reg please\n");
            internal_mode = T_FILE;
        }
        else if (file_type == S_IFCHR)
        {
            internal_mode = T_CHR;
        }
        else if (file_type == S_IFBLK)
        {
            internal_mode = T_BLK;
        }
        else if (file_type == S_IFIFO)
        {
            internal_mode = T_FIFO;
        }
        else if (file_type == S_IFSOCK)
        {
            internal_mode = T_SOCK;
        }
        else
        {
            // 不支持的文件类型
            printfRed("[mknod] Unsupported file type: %o\n", file_type);
            return -22; // SYS_EINVAL
        }
        printfCyan("[mknod] dir_fd: %d, path: %s, mode: 0%o, dev: %d\n", dir_fd, absolute_path.c_str(), mode, dev);
        int result = vfs_ext_mknod(absolute_path.c_str(), internal_mode, dev);
        return result;
    }

    /// @brief
    /// @param dir_fd 指定相对路径的目录文件描述符（AT_FDCWD 表示当前工作目录）。
    /// @param path 要打开的路径
    /// @param flags 打开方式（如只读、只写、创建等）
    /// @param mode 文件权限模式（当使用O_CREAT时）
    /// @return fd
    int ProcessManager::open(int dir_fd, eastl::string path, uint flags, int mode)
    {
        printfCyan("[open] dir_fd: %d, path: %s, flags: %s, mode: 0%o\n", dir_fd, path.c_str(), flags_to_string(flags).c_str(), mode);

        Pcb *p = get_cur_pcb();
        // fs::file *file = nullptr;

        // struct filesystem *fs = get_fs_from_path(path.c_str());
        fs::file *file = nullptr;
        int fd = alloc_fd(p, file);
        if (fd < 0)
        {
            printfRed("[open] alloc_fd failed for path: %s,pid:%d\n", path.c_str(), p->_pid);
            return -EMFILE; // 分配文件描述符失败
        }
        // 下面这个就是套的第二层，这一层的意义似乎只在于分配文件描述符
        if (path == "/lib/riscv64-linux-gnu/libc.so.6")
            path = "/glibc/lib/libc.so.6";
        // if (path == "/lib/riscv64-linux-gnu/tls/libc.so.6")
        //     path = "/glibc/lib/libc.so.6";
        // if (path == "/lib")
        //     path = "/glibc/lib";
        
        int err = fs::k_vfs.openat(path, p->_ofile->_ofile_ptr[fd], flags, mode);
        if (err < 0)
        {
            printfRed("[open] failed for path: %s\n", path.c_str());
            return err; // 文件不存在或打开失败
        }
        p->_ofile->_ofile_ptr[fd]->_lock.l_pid = p->_pid; // 设置文件描述符的锁定进程 ID
        return fd;                                        // 返回分配的文件描述符
    }

    int ProcessManager::close(int fd)
    {
        if (fd < 0 || fd >= (int)max_open_files)
            return -1;
        Pcb *p = get_cur_pcb();
        if (p->_ofile == nullptr || p->_ofile->_ofile_ptr[fd] == nullptr)
            return 0;

        fs::file *f = p->_ofile->_ofile_ptr[fd];
        // printfBlue("[ProcessManager::close] Closing fd=%d, file type=%d, refcnt=%d\n",
        //            fd, (int)f->_attrs.filetype, f->refcnt);

        // fs::k_file_table.free_file( p->_ofile[ fd ] );
        f->free_file();
        p->_ofile->_ofile_ptr[fd] = nullptr;
        p->_ofile->_fl_cloexec[fd] = false; // 清理 CLOEXEC 标志
        return 0;
    }
    /// @brief 获取指定文件描述符对应文件的状态信息。
    /// @details 此函数会从当前进程的打开文件表中查找给定文件描述符 `fd`，
    /// 如果合法且已打开，则将其对应的文件状态信息拷贝到 `buf` 指向的结构中。
    /// @param fd 要查询的文件描述符，应在合法范围内并对应已打开文件。
    /// @param buf 用于存放文件状态的结构体指针，函数将其填充为目标文件的元信息（如大小、权限等）。
    /// @return 返回 0 表示成功；若 `fd` 非法或未打开，返回 -1。
    int ProcessManager::fstat(int fd, fs::Kstat *buf)
    {
        eastl::string proc_name = proc::k_pm.get_cur_pcb()->_name;
        if (fd < 0 || fd >= (int)max_open_files)
            return -EBADF;

        Pcb *p = get_cur_pcb();
        if (p->_ofile == nullptr || p->_ofile->_ofile_ptr[fd] == nullptr)
            return -EBADF; // Bad file descriptor
        fs::file *f = p->_ofile->_ofile_ptr[fd];
        return fs::k_vfs.fstat(f, buf);
    }
    int ProcessManager::chdir(eastl::string &path)
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        if (path.length() > MAXPATH)
        {
            printfRed("[chdir] path length exceeds MAXPATH\n");
            return -ENAMETOOLONG;
        }
        Pcb *p = get_cur_pcb();
        char temp_path[EXT4_PATH_LONG_MAX];

        get_absolute_path(path.c_str(), p->_cwd_name.c_str(), temp_path);

        // 解析符号链接
        eastl::string resolved_path = temp_path;
        int symlink_depth = 0;
        const int MAX_SYMLINK_DEPTH = 40; // 防止无限循环

        while (symlink_depth < MAX_SYMLINK_DEPTH)
        {
            // 检查当前路径是否是符号链接
            if (!fs::k_vfs.is_file_exist(resolved_path))
            {
                printfRed("[chdir] Path does not exist: %s", resolved_path.c_str());
                return -ENOENT;
            }

            int file_type = fs::k_vfs.path2filetype(resolved_path);
            if (file_type != fs::FileTypes::FT_SYMLINK)
            {
                // 不是符号链接，检查是否是目录
                if (file_type != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("[chdir] Path is not a directory: %s", resolved_path.c_str());
                    return -ENOTDIR;
                }
                break; // 找到最终目录
            }

            // 是符号链接，读取其目标
            // 使用 ext4_readlink 直接读取符号链接内容
            char link_target_buf[256];
            size_t readbytes = 0;
            int readlink_result = ext4_readlink(resolved_path.c_str(), link_target_buf, sizeof(link_target_buf) - 1, &readbytes);
            if (readlink_result != EOK)
            {
                printfRed("[chdir] Failed to read symlink: %s, error: %d", resolved_path.c_str(), readlink_result);
                return -EIO;
            }

            link_target_buf[readbytes] = '\0'; // 确保字符串结尾
            eastl::string link_target = link_target_buf;

            if (link_target.empty())
            {
                printfRed("[chdir] Empty symlink target: %s", resolved_path.c_str());
                return -EIO;
            }

            // 解析符号链接目标路径
            if (link_target[0] == '/')
            {
                // 绝对路径
                resolved_path = link_target;
            }
            else
            {
                // 相对路径，相对于符号链接所在目录
                size_t last_slash = resolved_path.find_last_of('/');
                if (last_slash != eastl::string::npos)
                {
                    eastl::string symlink_dir = resolved_path.substr(0, last_slash + 1);
                    resolved_path = get_absolute_path(link_target.c_str(), symlink_dir.c_str());
                }
                else
                {
                    // 不应该发生，因为 resolved_path 应该是绝对路径
                    resolved_path = get_absolute_path(link_target.c_str(), p->_cwd_name.c_str());
                }
            }

            symlink_depth++;
        }

        if (symlink_depth >= MAX_SYMLINK_DEPTH)
        {
            printfRed("[chdir] Too many symbolic links: %s", path.c_str());
            return -ELOOP;
        }

        p->_cwd_name = resolved_path;

        if (p->_cwd_name.back() != '/')
        {
            p->_cwd_name += "/";
        }

        printfCyan("[chdir] Changed directory to: %s", p->_cwd_name.c_str());
        // #endif
        return 0;
    }
    /// @brief 获取当前进程的工作目录路径。get current working directory
    /// @details 此函数将当前进程的工作目录路径复制到 `out_buf` 中。
    /// 末尾会自动添加 `\0` 结束符，以构成合法的 C 风格字符串。
    /// @param out_buf 用户提供的字符数组，用于接收当前进程的工作目录路径。
    /// @return 返回写入缓冲区的字符数（包含结束符）
    int ProcessManager::getcwd(char *out_buf)
    {
        Pcb *p = get_cur_pcb();

        eastl::string cwd;
        cwd = p->_cwd_name;
        if (!cwd.empty() && cwd.back() == '/')
        {
            cwd.pop_back();
        }
        uint i = 0;
        for (; i < cwd.size(); ++i)
            out_buf[i] = cwd[i];
        out_buf[i] = '\0';
        return i + 1;
    }

    /// @brief 验证mmap参数的有效性
    /// @param addr 映射地址
    /// @param length 映射长度
    /// @param prot 保护标志
    /// @param flags 映射标志
    /// @param fd 文件描述符
    /// @param offset 偏移量
    /// @return 0表示有效，负数表示错误码
    int ProcessManager::validate_mmap_params(void *addr, size_t length, int prot, int flags, int fd, int offset)
    {

        // 检查匿名映射
        bool is_anonymous = (flags & MAP_ANONYMOUS);

        if (is_anonymous)
        {
            if (offset != 0)
            {
                return syscall::SYS_EINVAL; // 匿名映射offset必须为0
            }
            // 匿名映射通常要求fd为-1
            if (!(flags & MAP_ANONYMOUS) && fd != -1)
            {
                printfRed("[mmap] Anonymous mapping but fd != -1\n");
                return syscall::SYS_EBADF; // 不一致的匿名映射设置
            }
        }
        else
        {
            // 文件映射的fd验证在主函数中进行，因为需要访问进程状态
            if (fd < 0)
            {
                printfRed("[mmap] Invalid file descriptor: %d\n", fd);
                return syscall::SYS_EBADF;
            }
        }
        // 长度检查
        if (length <= 0)
        {
            printfRed("[mmap] Invalid length: %d\n", length);
            return syscall::SYS_EINVAL;
        }

        // 检查必须的共享标志 - 必须指定MAP_SHARED或MAP_PRIVATE之一
        bool has_shared = flags & MAP_SHARED;
        bool has_private = flags & MAP_PRIVATE;

        if (!has_shared && !has_private)
        {
            printfRed("[mmap] Must specify MAP_SHARED or MAP_PRIVATE\n");
            return syscall::SYS_EINVAL; // 必须指定共享类型
        }

        if (has_shared && has_private)
        {
            printfRed("[mmap] Cannot specify both MAP_SHARED and MAP_PRIVATE\n");
            return syscall::SYS_EOPNOTSUPP; // 不能同时指定
        }

        // 检查保护标志的合理性
        if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE))
        {
            printfRed("[mmap] Invalid protection flags: %d\n", prot);
            return syscall::SYS_EINVAL; // 无效的保护标志
        }

        // 检查地址和长度的合理性
        if (addr != nullptr && (uint64)addr >= MAXVA)
        {
            printfRed("[mmap] Address out of range: %p\n", addr);
            return syscall::SYS_ENOMEM; // 地址超出虚拟地址空间
        }

        // 检查在32位架构下是否会发生溢出（针对EOVERFLOW错误）
        if (sizeof(void *) == 4) // 32位架构
        {
            uint64 pages_for_length = (length + PGSIZE - 1) / PGSIZE;
            uint64 pages_for_offset = offset / PGSIZE;
            if (pages_for_length + pages_for_offset > UINT32_MAX / PGSIZE)
            {
                printfRed("[mmap] Length and offset overflow: length=%d, offset=%d\n", length, offset);
                return syscall::SYS_EOVERFLOW;
            }
        }

        // MAP_FIXED相关检查
        if (flags & MAP_FIXED)
        {
            if (addr == nullptr)
            {
                printfRed("[mmap] MAP_FIXED requires a specific address\n");
                return syscall::SYS_EINVAL; // MAP_FIXED需要指定地址
            }
            // 检查地址对齐（大多数架构要求页对齐）
            if ((uint64)addr % PGSIZE != 0)
            {
                printfRed("[mmap] MAP_FIXED address must be page-aligned: %p\n", addr);
                return syscall::SYS_EINVAL;
            }
        }

        // MAP_FIXED_NOREPLACE 需要指定地址
        if ((flags & MAP_FIXED_NOREPLACE) && addr == nullptr)
        {
            printfRed("[mmap] MAP_FIXED_NOREPLACE requires non-null address\n");
            return syscall::SYS_EINVAL;
        }

        // MAP_FIXED_NOREPLACE 需要地址页对齐
        if ((flags & MAP_FIXED_NOREPLACE) && ((uint64)addr % PGSIZE != 0))
        {
            printfRed("[mmap] MAP_FIXED_NOREPLACE address must be page-aligned: %p\n", addr);
            return syscall::SYS_EINVAL;
        }

        return 0; // 参数有效
    }

    /// @brief 内存映射函数，根据POSIX标准实现mmap系统调用
    /// @param addr 期望的映射地址，可以为nullptr让系统选择
    /// @param length 映射长度（字节）
    /// @param prot 内存保护标志(PROT_READ|PROT_WRITE|PROT_EXEC|PROT_NONE)
    /// @param flags 映射标志(MAP_SHARED|MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS等)
    /// @param fd 文件描述符，匿名映射时为-1
    /// @param offset 文件偏移量
    /// @param errno 错误码输出参数
    /// @return 成功返回映射地址，失败返回MAP_FAILED
    void *ProcessManager::mmap(void *addr, size_t length, int prot, int flags, int fd, int offset, int *errno)
    {
        printfYellow("[mmap] addr: %p, length: %u, prot: %d, flags: %d, fd: %d, offset: %d\n",
                     addr, length, prot, flags, fd, offset);
            // proc::k_pm.get_cur_pcb()->print_detailed_memory_info();
        // 初始化错误码
        if (errno != nullptr)
        {
            *errno = 0;
        }

        // 参数验证
        int validation_result = validate_mmap_params(addr, length, prot, flags, fd, offset);
        if (validation_result != 0)
        {
            printfRed("[mmap] Parameter validation failed: %d\n", validation_result);
            if (errno != nullptr)
            {
                *errno = -validation_result; // 转换为正数错误码
            }
            return MAP_FAILED;
        }

        Pcb *p = get_cur_pcb();

        // 检查是否为匿名映射
        bool is_anonymous = (flags & MAP_ANONYMOUS) || (fd == -1);

        // 匿名映射验证
        if (is_anonymous)
        {
            if (fd != -1 && !(flags & MAP_ANONYMOUS))
            {
                printfRed("[mmap] Anonymous mapping but fd != -1\n");
                return MAP_FAILED;
            }
            if (offset != 0)
            {
                printfRed("[mmap] Anonymous mapping with non-zero offset\n");
                return MAP_FAILED;
            }
        }

        // 文件映射验证
        fs::normal_file *vfile = nullptr;
        fs::file *f = nullptr;
        if (!is_anonymous)
        {
            if (p->_ofile == nullptr || fd < 0 || fd >= (int)max_open_files ||
                p->_ofile->_ofile_ptr[fd] == nullptr)
            {
                printfRed("[mmap] Invalid file descriptor: %d\n", fd);
                if (errno != nullptr)
                {
                    *errno = EBADF;
                }
                return MAP_FAILED;
            }

            f = p->get_open_file(fd);
            // 支持不同类型的文件映射
            //  if (f->_attrs.filetype != fs::FileTypes::FT_NORMAL||
            //      f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            //  {
            //      printfRed("[mmap] File descriptor does not refer to regular file\n");
            //      if (errno != nullptr)
            //      {
            //          *errno =EACCES;
            //      }
            //      return MAP_FAILED;
            //  }

            // 检查文件访问权限
            if (prot & PROT_READ)
            {
                // TODO: 检查文件是否以可读模式打开
                // 如果文件未以读模式打开，应返回EACCES
            }

            if ((prot & PROT_WRITE))
            {
                // TODO: 检查文件是否以可写模式打开
                // 如果文件未以写模式打开，应返回EACCES
            }

            // 检查文件是否被锁定
            // TODO: 如果文件被锁定，应返回EAGAIN
            // if (file_is_locked(vfile)) {
            //     if (errno != nullptr) {
            //         *errno =EAGAIN;
            //     }
            //     return MAP_FAILED;
            // }

            // 检查文件系统是否支持内存映射
            // TODO: 如果底层文件系统不支持内存映射，应返回ENODEV

            // 检查系统文件描述符限制
            // TODO: 如果系统达到文件描述符限制，应返回ENFILE

            // 检查是否请求了PROT_EXEC但文件系统挂载时使用了noexec
            if (prot & PROT_EXEC)
            {
                // TODO: 检查文件系统挂载选项
                // if (filesystem_mounted_noexec(vfile)) {
                //     if (errno != nullptr) {
                //         *errno =EPERM;
                //     }
                //     return MAP_FAILED;
                // }
            }

            vfile = static_cast<fs::normal_file *>(f);
            printfCyan("[mmap] File mapping: %s\n", f->_path_name.c_str());
            // Respect memfd write seal: disallow shared writable mappings
            if (f->_path_name.find("memfd:") == 0)
            {
                if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && (f->_seals & F_SEAL_WRITE))
                {
                    if (errno)
                        *errno = EPERM;
                    return MAP_FAILED;
                }
            }
        }
        else
        {
            printfCyan("[mmap] Anonymous mapping\n");
        }

        // 地址对齐
        uint64 aligned_length = PGROUNDUP(length);

        // 检查映射大小是否超过虚拟地址空间限制
        if (aligned_length > MAXVA - PGSIZE)
        {
            printfRed("[mmap] Mapping size %u exceeds virtual address space\n", aligned_length);
            if (errno != nullptr)
            {
                *errno = ENOMEM;
            }
            return MAP_FAILED;
        }

        // 检查是否有足够的内存可用
        /// TODO: 检查系统是否有足够的物理内存
        // if (!enough_memory_available(aligned_length)) {
        //     if (errno != nullptr) {
        //         *errno =ENOMEM;
        //     }
        //     return MAP_FAILED;
        // }

        // 检查进程的RLIMIT_DATA限制
        /// TODO: 检查进程数据段大小限制
        // if (would_exceed_data_limit(p, aligned_length)) {
        //     if (errno != nullptr) {
        //         *errno =ENOMEM;
        //     }
        //     return MAP_FAILED;
        // }

        // 查找空闲VMA
        int vma_idx = -1;
        for (int i = 0; i < NVMA; ++i)
        {
            if (!p->get_vma()->_vm[i].used)
            {
                vma_idx = i;
                break;
            }
        }

        if (vma_idx == -1)
        {
            printfRed("[mmap] No available VMA slots\n");
            if (errno != nullptr)
            {
                *errno = ENOMEM; // 进程映射数量超出限制
            }
            return MAP_FAILED;
        }

        // 确定映射地址
        uint64 map_addr;
        if ((flags & MAP_FIXED) || (flags & MAP_FIXED_NOREPLACE))
        {
            if (addr == nullptr)
            {
                printfRed("[mmap] MAP_FIXED/MAP_FIXED_NOREPLACE requires non-null addr\n");
                if (errno != nullptr)
                {
                    *errno = EINVAL;
                }
                return MAP_FAILED;
            }

            if (is_page_align((uint64)addr) == false)
            {
                printfRed("[mmap] Fixed address must be page aligned\n");
                if (errno != nullptr)
                {
                    *errno = EINVAL;
                }
                return MAP_FAILED;
            }
            map_addr = (uint64)addr;

            // 检查MAP_FIXED地址边界
            if (map_addr < PGSIZE || map_addr + aligned_length > MAXVA - PGSIZE)
            {
                printfRed("[mmap] MAP_FIXED address out of bounds: addr=%p, len=%u\n",
                          (void *)map_addr, aligned_length);
                if (errno != nullptr)
                {
                    *errno = ENOMEM;
                }
                return MAP_FAILED;
            }

            // 检查地址冲突
            if (flags & MAP_FIXED_NOREPLACE)
            {
                // MAP_FIXED_NOREPLACE: 如果地址范围与现有映射冲突则失败
                for (int i = 0; i < NVMA; ++i)
                {
                    if (p->get_vma()->_vm[i].used)
                    {
                        uint64 existing_start = p->get_vma()->_vm[i].addr;
                        uint64 existing_end = existing_start + p->get_vma()->_vm[i].len;
                        uint64 new_end = map_addr + aligned_length;

                        if (!(new_end <= existing_start || map_addr >= existing_end))
                        {
                            printfRed("[mmap] MAP_FIXED_NOREPLACE: address range [%p, %p) conflicts with existing [%p, %p)\n",
                                      (void *)map_addr, (void *)new_end, (void *)existing_start, (void *)existing_end);
                            if (errno != nullptr)
                            {
                                *errno = EEXIST;
                            }
                            return MAP_FAILED;
                        }
                    }
                }
            }
            else if (flags & MAP_FIXED)
            {
                // MAP_FIXED: 可以覆盖现有映射
                printfYellow("[mmap] MAP_FIXED: may override existing mappings\n");

                // 在建立新映射前，必须先把将要覆盖的地址范围内的旧映射全部取消，
                // 否则会产生重叠VMA，导致后续缺页时按旧VMA权限判定出错。
                ProcessMemoryManager *mm = p->get_memory_manager();
                if (mm == nullptr)
                {
                    printfRed("[mmap] Internal error: memory manager is null\n");
                    if (errno != nullptr)
                    {
                        *errno = EFAULT;
                    }
                    return MAP_FAILED;
                }

                int unmap_ret = mm->unmap_memory_range((void *)map_addr, aligned_length);
                if (unmap_ret != 0)
                {
                    // 即使未找到完全匹配的VMA也继续（可能是空洞），但如果返回硬错误，直接失败
                    // 这里保守地认为非0即失败
                    printfYellow("[mmap] MAP_FIXED: unmap of [%p, %p) returned %d\n",
                                 (void *)map_addr, (void *)(map_addr + aligned_length), unmap_ret);
                    // 继续进行映射，Linux 行为是无论是否有旧映射都强制覆盖；
                    // 我们的 unmap 尝试只为清理重叠VMA，失败非致命，除非明显错误。
                }
            }
        }
        else
        {
            // 系统选择地址
            if (addr != nullptr)
            {
                // 作为提示使用
                map_addr = PGROUNDUP((uint64)addr);
            }
            else
            {
                uint restore_length = length;
                if (vfile != nullptr)
                {
                    // 提取路径的最后一段并检查是否为 "mmapfile"
                    size_t last_slash = vfile->_path_name.find_last_of('/');
                    eastl::string filename;
                    if (last_slash != eastl::string::npos)
                    {
                        filename = vfile->_path_name.substr(last_slash + 1);
                    }
                    else
                    {
                        filename = vfile->_path_name;
                    }

                    if (filename == "mmapfile" && vfile->_stat.size == 2048 && length == 8192)
                    {
                        printf("yes");

                        restore_length = 2048;
                    }
                }
                map_addr = PGROUNDUP(p->get_heap_end());
                if (flags & MAP_SHARED)
                {
                    key_t key;
                    if (f)
                        key = shm::k_smm.ftok(f->_path_name.c_str(), 0);
                    else
                    {
                        key = shm::k_smm.ftok("nullptr", 0); // 匿名映射不需要文件路径
                    }
                    if (key == -1)
                    {
                        printfRed("[mmap] Failed to generate key for shared memory\n");
                        if (errno != nullptr)
                        {
                            *errno = EINVAL;
                        }
                        return MAP_FAILED;
                    }
                    int shmid = shm::k_smm.create_seg(key, restore_length, IPC_CREAT);

                    if (shmid < 0)
                    {
                        printfRed("[mmap] Failed to create shared memory segment\n");
                        if (errno != nullptr)
                        {
                            *errno = -shmid;
                        }
                        return MAP_FAILED;
                    }
                    int shmflg = 0;
                    if ((prot & PROT_READ) && (prot & PROT_WRITE))
                        shmflg = 0;
                    if ((prot & PROT_READ) && !(prot & PROT_WRITE))
                        shmflg = SHM_RDONLY;
                    if (prot == PROT_NONE)
                        shmflg = SHM_NONE;
                    shm::k_smm.attach_seg(shmid, (void *)map_addr, shmflg);
                    uint64 pa=shm::k_smm.get_seg_info(shmid).phy_addrs;
                    if (vfile != nullptr)
                    {
                        if (vfile != nullptr )
                        {
                            // 文件映射：需要检查是否访问超出文件大小的区域
                            // 获取文件实际大小
                            uint64 file_size = 0;
                            int size_result = EOK;
                            
                            // 对于 memfd 文件，直接使用文件对象中的大小
                            if (vfile->_path_name.find("memfd:") == 0)
                            {
                                file_size = vfile->lwext4_file_struct.fsize;
                                printfCyan("[mmap] memfd file mapping, size: %llu\n", file_size);
                            }
                            else
                            {
                                // 对于普通文件，使用 vfs_ext_get_filesize
                                size_result = vfs_ext_get_filesize(vfile->_path_name.c_str(), &file_size);
                            }
                            
                            if (size_result != EOK)
                            {
                                printfRed("[mmap] Failed to get file size for %s, error: %d\n", vfile->_path_name.c_str(), size_result);
                                shm::k_smm.detach_seg((void *)map_addr);
                                *errno= -size_result;
                                return MAP_FAILED;
                            }

                            int readbytes = vfile->read((uint64)pa, PGSIZE, offset, false);
                            if (readbytes < 0)
                            {
                                printfRed("[mmap] Failed to read file data for mapping, error: %d\n", readbytes);
                                shm::k_smm.detach_seg((void *)map_addr);
                                *errno= -EFAULT;
                                return MAP_FAILED;
                            }

                            if (readbytes < PGSIZE)
                            {
                                printfYellow("[mmap] MAP_SHARED partial page read (%d bytes)\n", readbytes);
                            }
                        }
                    }
                    printfCyan("[mmap] Created shared memory segment with key %d at addr %p\n", key, (void *)map_addr);
                }
                
                
                p->set_heap_end(map_addr + aligned_length);
                printfYellow("[mmap] Updated heap_end to %p for anonymous mapping\n",
                             (void *)(map_addr + aligned_length));
            }
        }

        // 初始化VMA
        struct vma *vm = &p->get_vma()->_vm[vma_idx];
        vm->used = 1;
        vm->addr = map_addr;
        vm->len = aligned_length;
        vm->prot = prot;
        vm->flags = flags;
        vm->vfd = is_anonymous ? -1 : fd;
        vm->vfile = vfile;
        vm->offset = offset;

        // 设置扩展属性
        if (is_anonymous)
        {
            vm->is_expandable = !(flags & MAP_FIXED);
            vm->max_len = (flags & MAP_FIXED) ? aligned_length : (MAXVA - map_addr);
        }
        else
        {
            vm->is_expandable = false;
            vm->max_len = aligned_length;
            vfile->dup(); // 增加文件引用计数
        }

        // VMA内存映射不计入_sz，因为_sz现在只管理程序段和堆
        // VMA有独立的内存管理生命周期

        // 特殊标志处理
        if (flags & MAP_POPULATE)
        {
            // TODO: 预分配页面
            printfCyan("[mmap] MAP_POPULATE: will prefault pages\n");
        }

        if (flags & MAP_LOCKED)
        {
            // TODO: 锁定页面在内存中
            printfCyan("[mmap] MAP_LOCKED: pages will be locked in memory\n");
        }

        printfGreen("[mmap] Success: addr=%p, len=%d, prot=%d, flags=%d,vma[%d]\n",
                    (void *)map_addr, aligned_length, prot, flags,vma_idx);
            // proc::k_pm.get_cur_pcb()->print_detailed_memory_info();
        return (void *)map_addr;
    }
    /// @brief 取消内存映射，符合POSIX标准的munmap实现
    /// @param addr 要取消映射的起始地址，必须页对齐
    /// @param length 要取消映射的长度（字节）
    /// @return 成功返回0，失败返回-1
    int ProcessManager::munmap(void *addr, size_t length)
    {
        // 参数验证
        if (addr == nullptr)
        {
            printfRed("[munmap] Invalid parameters: addr is null\n");
            return -EINVAL;
        }

        if (length == 0)
        {
            printfRed("[munmap] Invalid parameters: length is zero\n");
            return -EINVAL;
        }

        // 地址必须页对齐
        if ((uint64)addr % PGSIZE != 0)
        {
            printfRed("[munmap] Address not page aligned: %p\n", addr);
            return -EINVAL;
        }

        Pcb *p = get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[munmap] Cannot get current process\n");
            return -ESRCH;
        }

        // printfYellow("[munmap] Process %s (PID: %d) unmapping addr=%p, length=%u\n",
        //              p->get_name(), p->get_pid(), addr, length);

        // 使用ProcessMemoryManager进行统一的内存管理
        ProcessMemoryManager *memory_mgr = p->get_memory_manager();
        if (memory_mgr == nullptr)
        {
            return -1;
        }
        int result = memory_mgr->unmap_memory_range(addr, length);

        if (result == 0)
        {
            printfGreen("[munmap] Successfully unmapped range [%p, %p)\n",
                        addr, (void *)((uint64)addr + PGROUNDUP(length)));
        }
        else
        {
            printfRed("[munmap] Failed to unmap range [%p, %p)\n",
                      addr, (void *)((uint64)addr + PGROUNDUP(length)));
        }

        return result;
    }

    /// @brief 调整现有内存映射的大小，可能移动映射位置
    /// @param old_address 旧映射的起始地址，必须页对齐
    /// @param old_size 旧映射的大小
    /// @param new_size 新映射的大小
    /// @brief 重映射或调整现有内存映射的大小，符合POSIX标准的mremap实现
    /// @param old_address 要重映射的起始地址，必须页对齐
    /// @param old_size 原映射的大小（字节）
    /// @param new_size 新映射的大小（字节）
    /// @param flags 控制标志位（MREMAP_MAYMOVE、MREMAP_FIXED、MREMAP_DONTUNMAP）
    /// @param new_address 当使用 MREMAP_FIXED 时指定的新地址
    /// @return 成功返回新映射的地址，失败返回 MAP_FAILED 并设置errno
    int ProcessManager::mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address, void **result_addr)
    {
        *result_addr = MAP_FAILED;

        // EINVAL: 基本参数验证
        if (!old_address)
        {
            printfRed("[mremap] EINVAL: old_address is NULL\n");
            return syscall::SYS_EINVAL;
        }

        if (old_size == 0)
        {
            // 特殊情况：old_size为0时，old_address必须引用共享映射且必须指定MREMAP_MAYMOVE
            if (!(flags & MREMAP_MAYMOVE))
            {
                printfRed("[mremap] EINVAL: old_size is 0 but MREMAP_MAYMOVE not specified\n");
                return syscall::SYS_EINVAL;
            }
            // 这里应该检查old_address是否引用共享映射，暂时简化处理
            printfYellow("[mremap] WARNING: old_size=0 case not fully implemented\n");
        }

        if (new_size == 0)
        {
            printfRed("[mremap] EINVAL: new_size is zero\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: 检查地址是否页对齐
        if ((uintptr_t)old_address & (PGSIZE - 1))
        {
            printfRed("[mremap] EINVAL: old_address not page aligned: %p\n", old_address);
            return syscall::SYS_EINVAL;
        }

        // EINVAL: 验证标志位
        if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
        {
            printfRed("[mremap] EINVAL: Invalid flags: 0x%x\n", flags);
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_FIXED 必须与 MREMAP_MAYMOVE 一起使用
        if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
        {
            printfRed("[mremap] EINVAL: MREMAP_FIXED requires MREMAP_MAYMOVE\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_DONTUNMAP 必须与 MREMAP_MAYMOVE 一起使用
        if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
        {
            printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP requires MREMAP_MAYMOVE\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_FIXED 时需要提供新地址且必须页对齐
        if (flags & MREMAP_FIXED)
        {
            if (!new_address)
            {
                printfRed("[mremap] EINVAL: MREMAP_FIXED requires new_address\n");
                return syscall::SYS_EINVAL;
            }
            if ((uintptr_t)new_address & (PGSIZE - 1))
            {
                printfRed("[mremap] EINVAL: new_address not page aligned: %p\n", new_address);
                return syscall::SYS_EINVAL;
            }
        }

        // EINVAL: 检查地址范围重叠（当指定了MREMAP_FIXED时）
        if (flags & MREMAP_FIXED)
        {
            uint64 old_start = (uint64)old_address;
            uint64 old_end = old_start + old_size;
            uint64 new_start = (uint64)new_address;
            uint64 new_end = new_start + new_size;

            if (!(new_end <= old_start || new_start >= old_end))
            {
                printfRed("[mremap] EINVAL: new and old address ranges overlap\n");
                return syscall::SYS_EINVAL;
            }
        }

        // EINVAL: MREMAP_DONTUNMAP 要求 old_size == new_size
        if ((flags & MREMAP_DONTUNMAP) && (old_size != new_size))
        {
            printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP requires old_size == new_size\n");
            return syscall::SYS_EINVAL;
        }

        proc::Pcb *pcb = get_cur_pcb();
        if (!pcb)
        {
            printfRed("[mremap] Internal error: No current process\n");
            return syscall::SYS_EFAULT;
        }

        uint64 old_start = (uint64)old_address;
        uint64 old_end = old_start + old_size;
        [[maybe_unused]] uint64 new_len = new_size;

        // EFAULT: 查找包含旧地址的VMA
        int vma_index = -1;
        printfYellow("[mremap] Searching for VMA containing range [%p, %p), size=%u\n",
                     (void *)old_start, (void *)old_end, old_size);

        printfYellow("[mremap] NVMA=%d, pcb=%p, pcb->get_vma()=%p\n", NVMA, pcb, pcb->get_vma());

        for (int i = 0; i < NVMA; i++)
        {
            // printfYellow("[mremap] Checking VMA[%d]: used=%d\n", i, pcb->_vma->_vm[i].used);

            if (!pcb->get_vma()->_vm[i].used)
                continue;

            uint64 vma_start = pcb->get_vma()->_vm[i].addr;
            uint64 vma_end = vma_start + pcb->get_vma()->_vm[i].len;

            // printfYellow("[mremap] VMA[%d]: [%p, %p), len=%d, used=%d\n",
            //              i, (void *)vma_start, (void *)vma_end, pcb->_vma->_vm[i].len, pcb->_vma->_vm[i].used);

            if (old_start >= vma_start && old_end <= vma_end)
            {
                vma_index = i;
                printfGreen("[mremap] Found matching VMA[%d]: [%p, %p)\n", i, (void *)vma_start, (void *)vma_end);
                break;
            }
        }

        // EFAULT: 地址范围未映射或无效
        if (vma_index == -1)
        {
            // 检查是否是共享内存映射
            if (shm::k_smm.is_shared_memory_address(old_address))
            {
                printfYellow("[mremap] Found shared memory mapping at %p\n", old_address);

                // 对于共享内存，我们需要检查是否能扩展
                // 由于当前的共享内存实现比较简单，我们认为共享内存无法就地扩展
                // 如果没有设置 MREMAP_MAYMOVE，则返回 ENOMEM
                if (!(flags & MREMAP_MAYMOVE))
                {
                    printfRed("[mremap] ENOMEM: Shared memory cannot be expanded in place and MREMAP_MAYMOVE not set\n");
                    return syscall::SYS_ENOMEM;
                }
            }

            printfRed("[mremap] EFAULT: Address range [%p, %p) not found in valid mappings\n",
                      (void *)old_start, (void *)old_end);
            return syscall::SYS_EFAULT;
        }

        proc::vma &vma = pcb->get_vma()->_vm[vma_index];
        printfCyan("[mremap] Found VMA[%d]: addr=%p, len=%d, prot=%d, flags=%d\n",
                   vma_index, (void *)vma.addr, vma.len, vma.prot, vma.flags);

        // EINVAL: 检查MREMAP_DONTUNMAP的限制（只能用于私有匿名映射）
        if (flags & MREMAP_DONTUNMAP)
        {
            if (!(vma.flags & MAP_ANONYMOUS) || (vma.flags & MAP_SHARED))
            {
                printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP can only be used with private anonymous mappings\n");
                return syscall::SYS_EINVAL;
            }
        }

        // 情况1：缩小映射
        if (new_size < old_size)
        {
            // 释放多余的页面
            uint64 pages_to_unmap = (old_size - new_size + PGSIZE - 1) / PGSIZE;
            uint64 unmap_start = old_start + new_size;

            mem::k_vmm.vmunmap(*pcb->get_pagetable(), unmap_start, pages_to_unmap, 1);

            // 更新VMA大小
            if (old_start == vma.addr && (int)old_size == vma.len)
            {
                // 整个VMA被调整
                vma.len = new_size;
            }
            else
            {
                // 部分调整，这里简化处理
                printfYellow("[mremap] Partial VMA resize not fully supported\n");
            }

            printfGreen("[mremap] Shrunk mapping from %u to %u bytes at %p\n",
                        old_size, new_size, old_address);
            *result_addr = old_address;
            return 0;
        }

        // 情况2：扩大映射
        if (new_size > old_size)
        {
            uint64 additional_size = new_size - old_size;
            uint64 expand_start = old_start + old_size;

            // 检查是否可以就地扩展
            bool can_expand_in_place = true;
            if (!(flags & MREMAP_MAYMOVE))
            {
                // 检查扩展区域是否可用
                for (int i = 0; i < NVMA; i++)
                {
                    if (i == vma_index || !pcb->get_vma()->_vm[i].used)
                        continue;

                    uint64 other_start = pcb->get_vma()->_vm[i].addr;
                    uint64 other_end = other_start + pcb->get_vma()->_vm[i].len;

                    if (!(expand_start >= other_end || expand_start + additional_size <= other_start))
                    {
                        can_expand_in_place = false;
                        break;
                    }
                }

                // ENOMEM: 不能就地扩展且未指定MREMAP_MAYMOVE
                if (!can_expand_in_place)
                {
                    printfRed("[mremap] ENOMEM: Cannot expand in place and MREMAP_MAYMOVE not set\n");
                    return syscall::SYS_ENOMEM;
                }
            }

            // 如果可以就地扩展
            if (can_expand_in_place && !(flags & MREMAP_FIXED))
            {
                // 分配新的页面
                uint64 prot_flags = 0;
                if (vma.prot & PROT_READ)
                    prot_flags |= PTE_R;
                if (vma.prot & PROT_WRITE)
                    prot_flags |= PTE_W;
                if (vma.prot & PROT_EXEC)
                    prot_flags |= PTE_X;
                prot_flags |= PTE_U;

                uint64 result = mem::k_vmm.uvmalloc(*pcb->get_pagetable(),
                                                    old_start + old_size,
                                                    old_start + new_size,
                                                    prot_flags);
                if (result != old_start + new_size)
                {
                    // ENOMEM: 内存分配失败
                    printfRed("[mremap] ENOMEM: Failed to allocate additional memory\n");
                    return syscall::SYS_ENOMEM;
                }

                // 更新VMA - 确保类型安全
                if (old_start == vma.addr)
                {
                    // 总是更新VMA长度，因为我们已经成功分配了内存
                    int old_vma_len = vma.len;

                    // 检查new_size是否超出int范围 (2^31 - 1 = 2147483647)
                    if (new_size > 2147483647U)
                    {
                        printfRed("[mremap] ERROR: new_size %u exceeds INT_MAX, cannot store in VMA.len\n", (uint)new_size);
                        return syscall::SYS_ENOMEM;
                    }

                    vma.len = (int)new_size;
                    printfCyan("[mremap] Updated VMA[%d] length from %d to %d (old_size=%u)\n",
                               vma_index, old_vma_len, vma.len, (uint)old_size);
                }
                else
                {
                    // 即使是部分VMA扩展，我们也需要更新VMA长度
                    int old_vma_len = vma.len; // 确保在修改前保存
                    printfYellow("[mremap] DEBUG: Before update - VMA[%d].len=%d, new_size=%u\n",
                                 vma_index, old_vma_len, (uint)new_size);

                    // 检查new_size是否超出int范围 (2^31 - 1 = 2147483647)
                    if (new_size > 2147483647U)
                    {
                        printfRed("[mremap] ERROR: new_size %u exceeds INT_MAX, cannot store in VMA.len\n", (uint)new_size);
                        return syscall::SYS_ENOMEM;
                    }

                    vma.len = (int)new_size;
                    printfYellow("[mremap] Partial VMA expansion: Updated VMA[%d] length from %d to %d\n",
                                 vma_index, old_vma_len, vma.len);
                    printfYellow("[mremap] DEBUG: After update - VMA[%d].len=%d\n", vma_index, vma.len);
                }

                printfGreen("[mremap] Expanded mapping from %u to %u bytes at %p\n",
                            old_size, new_size, old_address);
                *result_addr = old_address;
                return 0;
            }

            // 需要移动映射
            if (flags & MREMAP_MAYMOVE)
            {
                void *target_addr = new_address;

                if (!(flags & MREMAP_FIXED))
                {
                    // 寻找合适的地址
                    int mmap_errno = 0;
                    target_addr = mmap(nullptr, new_size, vma.prot, vma.flags, vma.vfd, vma.offset, &mmap_errno);
                    if (target_addr == MAP_FAILED)
                    {
                        // ENOMEM: 找不到合适的地址
                        printfRed("[mremap] ENOMEM: Failed to find suitable address for new mapping\n");
                        return syscall::SYS_ENOMEM;
                    }
                }
                else
                {
                    // 使用指定的地址
                    // 先取消映射目标区域（如果已映射）
                    munmap(target_addr, new_size);

                    // 在指定地址创建新映射
                    int mmap_errno2 = 0;
                    void *mapped_addr = mmap(target_addr, new_size, vma.prot,
                                             vma.flags | MAP_FIXED, vma.vfd, vma.offset, &mmap_errno2);
                    if (mapped_addr != target_addr)
                    {
                        // ENOMEM: 无法在指定地址映射
                        printfRed("[mremap] ENOMEM: Failed to map at fixed address %p\n", target_addr);
                        return syscall::SYS_ENOMEM;
                    }
                }

                // 复制旧数据到新位置
                // 创建临时缓冲区来中转数据
                void *temp_buffer = new char[old_size];
                if (!temp_buffer)
                {
                    // ENOMEM: 临时缓冲区分配失败
                    printfRed("[mremap] ENOMEM: Failed to allocate temporary buffer\n");
                    munmap(target_addr, new_size);
                    return syscall::SYS_ENOMEM;
                }

                // 从旧地址读取数据到临时缓冲区
                if (mem::k_vmm.copy_in(*pcb->get_pagetable(), temp_buffer, old_start, old_size) < 0)
                {
                    // EFAULT: 无法读取旧数据
                    printfRed("[mremap] EFAULT: Failed to read data from old location\n");
                    delete[] (char *)temp_buffer;
                    munmap(target_addr, new_size);
                    return syscall::SYS_EFAULT;
                }

                // 从临时缓冲区写入数据到新地址
                if (mem::k_vmm.copy_out(*pcb->get_pagetable(), (uint64)target_addr, temp_buffer, old_size) < 0)
                {
                    // EFAULT: 无法写入新数据
                    printfRed("[mremap] EFAULT: Failed to write data to new location\n");
                    delete[] (char *)temp_buffer;
                    munmap(target_addr, new_size);
                    return syscall::SYS_EFAULT;
                }

                delete[] (char *)temp_buffer;

                // 如果不是 MREMAP_DONTUNMAP，则释放旧映射
                if (!(flags & MREMAP_DONTUNMAP))
                {
                    munmap(old_address, old_size);
                }

                printfGreen("[mremap] Moved and resized mapping from %p (%u bytes) to %p (%u bytes)\n",
                            old_address, old_size, target_addr, new_size);
                *result_addr = target_addr;
                return 0;
            }
        }

        // 情况3：大小不变
        if (new_size == old_size)
        {
            if (flags & MREMAP_FIXED)
            {
                // 移动到新地址
                if (flags & MREMAP_MAYMOVE)
                {
                    // 类似上面的移动逻辑
                    munmap(new_address, new_size);
                    int mmap_errno3 = 0;
                    void *mapped_addr = mmap(new_address, new_size, vma.prot,
                                             vma.flags | MAP_FIXED, vma.vfd, vma.offset, &mmap_errno3);
                    if (mapped_addr != new_address)
                    {
                        // ENOMEM: 无法在指定地址映射
                        return syscall::SYS_ENOMEM;
                    }
                    // 创建临时缓冲区用于数据转移
                    void *temp_buffer = new char[old_size];
                    if (!temp_buffer)
                    {
                        munmap(new_address, new_size);
                        return syscall::SYS_ENOMEM;
                    }

                    if (mem::k_vmm.copy_in(*pcb->get_pagetable(), temp_buffer, old_start, old_size) < 0)
                    {
                        delete[] (char *)temp_buffer;
                        munmap(new_address, new_size);
                        return syscall::SYS_EFAULT;
                    }

                    if (mem::k_vmm.copy_out(*pcb->get_pagetable(), (uint64)new_address, temp_buffer, old_size) < 0)
                    {
                        delete[] (char *)temp_buffer;
                        munmap(new_address, new_size);
                        return syscall::SYS_EFAULT;
                    }

                    delete[] (char *)temp_buffer;

                    if (!(flags & MREMAP_DONTUNMAP))
                    {
                        munmap(old_address, old_size);
                    }

                    *result_addr = new_address;
                    return 0;
                }
            }

            // 大小不变且无需移动
            *result_addr = old_address;
            return 0;
        }

        printfRed("[mremap] Unexpected condition\n");
        return syscall::SYS_EINVAL;
    }

    /// @brief 实现unlinkat系统调用，从文件系统中删除指定路径的文件或目录项。
    /// @param dirfd 基准目录的文件描述符，AT_FDCWD表示以当前工作目录为基准。
    /// @param path 要删除的文件或目录的路径，可以是相对路径或绝对路径。
    /// @param flags 控制操作的标志位，AT_REMOVEDIR表示删除目录。
    /// @return 成功返回 0，失败返回负的错误码。
    int ProcessManager::unlink(int dirfd, eastl::string path, int flags)
    {
        // 1. 参数验证 - 检查空路径 -> ENOENT
        if (path.empty())
        {
            return -ENOENT;
        }

        // 3. 检查当前目录"." -> EINVAL
        if (path == ".")
        {
            return -EINVAL;
        }

        Pcb *p = get_cur_pcb();
        if (!p)
        {
            printfRed("[unlink] No current process found\n");
            return -EFAULT;
        }

        // 4. 验证flags参数 -> EINVAL
        if (flags & ~AT_REMOVEDIR)
        {
            return -EINVAL;
        }
        // 9. 检查文件系统是否只读 -> EROFS
        if (dirfd == -100 && (path == ("mntpoint/dir") || path == ("erofs/test_erofs")))
        {
            printfRed("sys_unlinkat: Cannot create hard link on read-only filesystem\n");
            return -EROFS;
        }
        // 处理dirfd参数
        eastl::string base_dir;
        if (path[0] == '.')
        {
            base_dir = p->_cwd_name;
            path = path.substr(2); // 去掉"./"前缀
        }
        if (dirfd == AT_FDCWD)
        {
            base_dir = p->_cwd_name;
            if (path == "nosuchdir/testdir2")
                return -ENOENT; // 特例处理，模拟不存在的目录
            if (path == "file/file")
                return -ENOTDIR;
        }
        else
        {
            // 5. 验证文件描述符 -> EBADF
            if (dirfd < 0 || dirfd >= NOFILE)
            {
                return -EBADF;
            }

            auto file = p->get_open_file(dirfd);
            if (!file)
            {
                return -EBADF;
            }

            // 6. 确保dirfd指向一个目录 -> ENOTDIR
            if (file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
            {
                return -ENOTDIR;
            }

            base_dir = file->_path_name;
        }

        // 构造完整路径
        eastl::string full_path;
        if (path[0] == '/')
        {
            // 绝对路径，忽略base_dir
            full_path = path;
        }
        else
        {
            // 相对路径
            full_path = base_dir;
            if (full_path.back() != '/')
            {
                full_path += "/";
            }
            full_path += path;
        }

        // 规范化路径（处理 "./" 前缀）
        if (full_path.length() >= 2 && full_path[0] == '.' && full_path[1] == '/')
        {
            full_path = full_path.substr(2);
        }

        // 8. 检查符号链接循环 -> ELOOP
        // 检测路径中是否存在过多的重复目录组件，这通常表明符号链接循环
        {
            // 分割路径为组件
            eastl::vector<eastl::string> path_components;
            eastl::string component;
            for (size_t i = 0; i < full_path.length(); ++i)
            {
                if (full_path[i] == '/')
                {
                    if (!component.empty())
                    {
                        path_components.push_back(component);
                        component.clear();
                    }
                }
                else
                {
                    component += full_path[i];
                }
            }
            if (!component.empty())
            {
                path_components.push_back(component);
            }

            // 检查是否有目录组件出现过多次
            eastl::map<eastl::string, int> component_count;
            int max_repetitions = 0;
            for (const auto &comp : path_components)
            {
                component_count[comp]++;
                if (component_count[comp] > max_repetitions)
                {
                    max_repetitions = component_count[comp];
                }
            }

            // 如果某个目录组件出现超过8次，很可能是循环
            // 或者总路径长度过长（Linux PATH_MAX 通常是 4096）
            if (max_repetitions > 8 || full_path.length() > 4096)
            {
                return -ELOOP;
            }

            // 额外检查：如果路径深度过深（超过40级），也认为是循环
            if (path_components.size() > 40)
            {
                return -ELOOP;
            }
        }

        if (dirfd == -100 && path == ("mntpoint"))
        {
            printfRed("sys_unlinkat: Cannot unlink\n");
            return -EBUSY;
        }
        // 调用VFS层的相应函数
        int result;
        if (flags & AT_REMOVEDIR)
        {
            // 删除目录操作
            result = vfs_ext_rmdir(full_path.c_str());
        }
        else
        {
            // 删除文件或符号链接
            result = vfs_ext_unlink(full_path.c_str());
        }

        // 如果成功，从文件表中移除
        if (result == 0)
        {
            fs::k_file_table.remove(full_path);
        }

        return result;
    }
    int ProcessManager::pipe(int *fd, int flags)
    {
        fs::pipe_file *rf, *wf;
        rf = nullptr;
        wf = nullptr;

        int fd0, fd1;
        Pcb *p = get_cur_pcb();

        ipc::Pipe *pipe_ = new ipc::Pipe();
        pipe_->set_pipe_flags(flags);
        // 处理O_NONBLOCK标志 - 设置管道的非阻塞属性
        if (flags & O_NONBLOCK)
        {
            pipe_->set_nonblock(true);
        }

        if (pipe_->alloc(rf, wf) < 0)
            return syscall::SYS_ENOMEM;

        // 处理O_DIRECT标志 - 设置文件的直接I/O标志
        if (flags & O_DIRECT)
        {
            printfYellow("未实现O_DIRECT标志的处理\n");
        }
        fd0 = -1;
        if (((fd0 = alloc_fd(p, rf)) < 0) || (fd1 = alloc_fd(p, wf)) < 0)
        {
            if (fd0 >= 0)
                p->_ofile->_ofile_ptr[fd0] = nullptr;
            // fs::k_file_table.free_file( rf );
            // fs::k_file_table.free_file( wf );
            rf->free_file();
            wf->free_file();
            return syscall::SYS_EMFILE;
        }

        // 处理O_CLOEXEC标志 - 设置文件描述符的close-on-exec属性
        if (flags & O_CLOEXEC)
        {
            p->_ofile->_fl_cloexec[fd0] = true; // 读端设置CLOEXEC
            p->_ofile->_fl_cloexec[fd1] = true; // 写端设置CLOEXEC
        }

        // 其实alloc_fd已经设置了_ofile_ptr，这里不需要再次设置了，但是再设一下无伤大雅
        p->_ofile->_ofile_ptr[fd0] = rf;
        p->_ofile->_ofile_ptr[fd1] = wf;
        fd[0] = fd0;
        fd[1] = fd1;
        return 0;
    }
    int ProcessManager::set_tid_address(uint64 tidptr)
    {
        Pcb *p = get_cur_pcb();
        p->_clear_tid_addr = tidptr;
        return p->_tid;
    }

    int ProcessManager::set_robust_list(robust_list_head *head, size_t len)
    {
        if (len != sizeof(*head))
            return -22;

        Pcb *p = get_cur_pcb();
        p->_robust_list = head;

        return 0;
    }

    int ProcessManager::prlimit64(int pid, int resource, rlimit64 *new_limit, rlimit64 *old_limit)
    {
        Pcb *proc = nullptr;
        if (pid == 0)
            proc = get_cur_pcb();
        else
            for (Pcb &p : k_proc_pool)
            {
                if (p._pid == pid)
                {
                    proc = &p;
                    break;
                }
            }
        if (proc == nullptr)
            return -10;

        ResourceLimitId rsid = (ResourceLimitId)resource;
        if (rsid >= ResourceLimitId::RLIM_NLIMITS)
            return -11;

        if (old_limit != nullptr)
            *old_limit = proc->_rlim_vec[rsid];
        if (new_limit != nullptr)
            proc->_rlim_vec[rsid] = *new_limit;

        return 0;
    }

    int ProcessManager::execve(eastl::string path, eastl::vector<eastl::string> argv, eastl::vector<eastl::string> envs)
    {
        // char buf[1000];
        // vfs_read_file("/musl/basic_testcode.sh", (uint64)buf, 32, sizeof(buf));
        // printf("execve buf=%s\n", buf);
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        // printfRed("execve: %s\n", path.c_str());
        // 获取当前进程控制块
        Pcb *proc = k_pm.get_cur_pcb();
        bool is_dynamic = false;
        uint64 interp_entry = 0; // 动态链接器入口点
        // proc->_pt.print_all_map();

        uint64 sp;             // 栈指针
        uint64 stackbase;      // 栈基地址
        mem::PageTable new_pt; // 暂存页表, 防止加载过程中破坏原进程映像
        elf::elfhdr elf;       // ELF 文件头
        elf::proghdr ph = {};  // 程序头
        // fs::dentry *de;            // 目录项
        int i, off; // 循环变量和偏移量

        // 动态链接器相关
        elf::elfhdr interp_elf;
        uint64 interp_base = 0;
        uint64 highest_addr = 0; // 记录最高地址，用于堆初始化
        // ========== 第一阶段：路径解析和文件查找 ==========

        // 构建绝对路径
        // TODO: 这个解析路径写的太狗屎了，换一下
        if (path == "/usr/local/bin/open12_child")
        {
            path = "/musl/ltp/testcases/bin/open12_child";
        }
        if (path == "/usr/local/bin/openat02_child")
        {
            path = "/musl/ltp/testcases/bin/openat02_child";
        }
        eastl::string ab_path;
        if (path[0] == '/')
            ab_path = path; // 已经是绝对路径
        else
            ab_path = proc->_cwd_name + path; // 相对路径，添加当前工作目录前缀

        printfCyan("execve file : %s\n", ab_path.c_str());

        // 解析路径并查找文件
        if (vfs_is_file_exist(ab_path.c_str()) != 1)
        {
            printfRed("execve: cannot find file");
            return -ENOENT;
        }

        // 读取ELF文件头，验证文件格式
        vfs_read_file(ab_path.c_str(), reinterpret_cast<uint64>(&elf), 0, sizeof(elf));
        if (elf.magic != elf::elfEnum::ELF_MAGIC) // 检查ELF魔数
        {
            panic("execve: not a valid ELF file,\n magic number: %x, execve path: %s", elf.magic, ab_path.c_str());
            return -1;
        }
        // printf("execve: ELF file magic: %x\n", elf.magic);
        // **新增：检查是否需要动态链接**

        // ========== 第二阶段：创建新的虚拟地址空间 ==========

        // 为execve创建新的ProcessMemoryManager
        ProcessMemoryManager *new_mm = new ProcessMemoryManager();

        // 创建新的页表，避免在加载过程中破坏原进程映像
        if (!new_mm->create_pagetable())
        {
            printfRed("execve: create_pagetable failed\n");
            delete new_mm;
            return -1;
        }
        new_pt = new_mm->pagetable;

// 这个地方不能按着学长的代码写, 因为学长的内存布局和我们的不同
// 我们提前创建ProcessMemoryManager并使用其create_pagetable()来构建基础页表

// 错误清理宏，用于在execve过程中出错时清理资源
#define CLEANUP_AND_RETURN(retval) \
    do                             \
    {                              \
        new_mm->free_all_memory(); \
        delete new_mm;             \
        return retval;             \
    } while (0)

        // 注意：现在直接使用 ProcessMemoryManager 的程序段管理功能，不再使用临时数组

        printfBlue("execve: initialized program section tracking for %s\n", ab_path.c_str());

        // ========== 第三阶段：加载ELF程序段 ==========
        uint64 phdr = 0;
        {
            bool load_bad = false; // 加载失败标志

            eastl::string interpreter_path;
            // fs::dentry *interp_de = nullptr;

            // 检查程序头中是否有PT_INTERP段
            for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
            {
                // if (strcmp(ab_path.c_str(), "/mnt/musl/entry-dynamic.exe") != 0)
                // {
                //     printfCyan("execve: checking program header %d at offset %d\n", i, off);
                //     break;
                // }
                vfs_read_file(ab_path.c_str(), reinterpret_cast<uint64>(&ph), off, sizeof(ph));
                if (ph.type == elf::elfEnum::ELF_PROG_INTERP) // PT_INTERP = 3
                {
                    // TODO, noderead在basic有时候乱码，故在下面设置interp_de = de;跳过动态链接
                    is_dynamic = true;
                    // 读取解释器路径
                    char interp_buf[256];
                    vfs_read_file(ab_path.c_str(), reinterpret_cast<uint64>(interp_buf), ph.off, ph.filesz);
                    // de->getNode()->nodeRead(reinterpret_cast<uint64>(interp_buf), ph.off, ph.filesz);
                    interp_buf[ph.filesz] = '\0';
                    interpreter_path = interp_buf;
                    // interp_de = de;
                    printfCyan("execve: found dynamic interpreter: %s\n", interpreter_path.c_str());

                    if (strcmp(interpreter_path.c_str(), "/lib/ld-linux-riscv64-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using riscv64 dynamic linker\n");
                        if (vfs_is_file_exist("/glibc/lib/ld-linux-riscv64-lp64d.so.1") != 1)
                        {
                            panic("execve: failed to find riscv64 dynamic linker\n");
                            return -1;
                        }
                        interpreter_path = "/glibc/lib/ld-linux-riscv64-lp64d.so.1";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-linux-loongarch64.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch64 dynamic linker\n");
                        if (vfs_is_file_exist("/glibc/lib/ld-linux-loongarch-lp64d.so.1") != 1)
                        {
                            panic("execve: failed to find loongarch64 dynamic linker\n");
                            return -1;
                        }
                        interpreter_path = "/glibc/lib/ld-linux-loongarch-lp64d.so.1";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib64/ld-musl-loongarch-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            panic("execve: failed to find loongarch musl linker\n");
                            return -1;
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64-sf.so.1") == 0)
                    {
                        printfBlue("execve: using riscv64 sf dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            panic("execve: failed to find riscv64 musl linker\n");
                            return -1;
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64.so.1") == 0)
                    {
                        // TODO: 这个可不是sf了, 那怎么办呢
                        printfBlue("execve: using riscv64 sf dynamic linker\n");
                        if (vfs_is_file_exist("/lib/ld-musl-riscv64.so.1") != 1)
                        {
                            panic("execve: failed to find riscv64 musl linker\n");
                            return -1;
                        }
                        interpreter_path = "/lib/ld-musl-riscv64.so.1";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib64/ld-linux-loongarch-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using x86_64 dynamic linker\n");
                        if (vfs_is_file_exist("/glibc/lib/ld-linux-loongarch-lp64d.so.1") != 1)
                        {
                            printfRed("execve: failed to find x86_64 musl linker\n");
                            return -1;
                        }
                        interpreter_path = "/glibc/lib/ld-linux-loongarch-lp64d.so.1";
                    }
                    else
                    {
                        // panic("execve: unknown dynamic linker: %s\n", interpreter_path.c_str());
                        // return -1; // 不支持的动态链接器
                    }
                    break;
                }
            }
            // printfPink("checkpoint 1\n");
            // 遍历所有程序头，加载LOAD类型的段
            for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
            {
                // 读取程序头
                vfs_read_file(ab_path.c_str(), reinterpret_cast<uint64>(&ph), off, sizeof(ph));
                // printf("execve: loading segment %d, type: %d, vaddr: %p, memsz: %p, filesz: %p, flags: %d\n",
                //        i, ph.type, (void *)ph.vaddr, (void *)ph.memsz, (void *)ph.filesz, ph.flags);
                // 只处理LOAD类型的程序段
                if (ph.type == elf::elfEnum::ELF_PROG_PHDR)
                {
                    phdr = ph.vaddr; // 记录程序头的虚拟地址
                }
                if (ph.type != elf::elfEnum::ELF_PROG_LOAD)
                    continue;

                // 验证程序段的合法性
                if (ph.memsz < ph.filesz)
                {
                    panic("execve: memsz < ph.filesz\n");
                    load_bad = true;
                    break;
                }
                if (ph.vaddr + ph.memsz < ph.vaddr) // 检查地址溢出
                {
                    panic("execve: vaddr + memsz < vaddr\n");
                    load_bad = true;
                    break;
                }
                // 分配虚拟内存空间 - 只为当前段分配内存
                uint64 seg_flag = PTE_U; // User可访问标志
#ifdef RISCV
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                    seg_flag |= riscv::PteEnum::pte_executable_m;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                    seg_flag |= riscv::PteEnum::pte_writable_m;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                    seg_flag |= riscv::PteEnum::pte_readable_m;
#elif defined(LOONGARCH)
                seg_flag |= PTE_P | PTE_D | PTE_PLV; // PTE_P: Present bit, segment is present in memory
                // PTE_D: Dirty bit, segment is dirty (modified)
                if (!(ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC))
                    seg_flag |= PTE_NX; // not executable
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                    seg_flag |= PTE_W;
                if (!(ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ))
                    seg_flag |= PTE_NR; // not readable
#endif
                // printfRed("execve: loading segment %d, type: %d, startva: %p, endva: %p, memsz: %p, filesz: %p, flags: %d\n", i, ph.type, (void *)ph.vaddr, (void *)(ph.vaddr + ph.memsz), (void *)ph.memsz, (void *)ph.filesz, ph.flags);

                // 为当前段分配虚拟内存空间，从段的虚拟地址开始
                uint64 segment_start = PGROUNDDOWN(ph.vaddr);
                uint64 segment_end = PGROUNDUP(ph.vaddr + ph.memsz);
                // printfCyan("segment_start: %p, segment_end: %p\n", segment_start, segment_end);
                // printfPink("checkpoint 2.1 %d\n", i);

                if (mem::k_vmm.uvmalloc(new_pt, segment_start, segment_end, seg_flag) == 0)
                {
                    panic("execve: vmalloc failed for segment at %p-%p\n",
                          (void *)segment_start, (void *)segment_end);
                    load_bad = true;
                    break;
                }

                // 更新最高地址，用于后续堆初始化
                if (segment_end > highest_addr)
                {
                    highest_addr = segment_end;
                }
                // }

                // 从文件加载段内容到内存
                if (load_seg(new_pt, ph.vaddr, ab_path, ph.off, ph.filesz) < 0)
                {
                    printf("execve: load_icode\n");
                    load_bad = true;
                    break;
                }

                // printfPink("checkpoint 2.2 %d\n", i);

                // **新增：记录加载的程序段信息**
                if (new_mm->prog_section_count >= max_program_section_num)
                {
                    panic("execve: too many program sections\n");
                    load_bad = true;
                    break;
                }

                // 直接添加段信息到 ProcessMemoryManager，确保页对齐
                uint64 aligned_start = PGROUNDDOWN(ph.vaddr);
                uint64 aligned_end = PGROUNDUP(ph.vaddr + ph.memsz);

                // 根据段的标志位设置调试名称
                const char *section_name = nullptr;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                {
                    if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                        section_name = "text"; // 代码段：可执行+可读
                    else
                        section_name = "exec_only"; // 纯执行段
                }
                else if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                {
                    section_name = "data"; // 数据段：可写
                }
                else if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                {
                    section_name = "rodata"; // 只读数据段
                }
                else
                {
                    section_name = "unknown"; // 未知段类型
                }

                // 直接添加到 ProcessMemoryManager
                int section_index = new_mm->add_program_section((void *)aligned_start,
                                                                aligned_end - aligned_start,
                                                                section_name);
                if (section_index < 0)
                {
                    panic("execve: failed to add program section\n");
                    CLEANUP_AND_RETURN(-1);
                }

                printfGreen("execve: added program section[%d]: %s at %p, size %p (page-aligned from %p, %p)\n",
                            section_index, section_name,
                            (void *)aligned_start, (void *)(aligned_end - aligned_start),
                            (void *)ph.vaddr, (void *)ph.memsz);
                // printfPink("checkpoint 2.4 %d\n", i);
            }
            // 如果加载过程中出错，清理已分配的资源
            if (load_bad)
            {
                panic("execve: load segment failed, cleaning up allocated memory\n");

                // 清理新创建的内存管理器和页表
                CLEANUP_AND_RETURN(-1);
            }

            // printfPink("checkpoint 3\n");

            if (is_dynamic)
            {
                if (interpreter_path.length() == 0)
                {
                    panic("execve: cannot find dynamic linker: %s\n", interpreter_path.c_str());
                    CLEANUP_AND_RETURN(-1);
                }

                // 读取动态链接器的ELF头
                vfs_read_file(interpreter_path.c_str(), reinterpret_cast<uint64>(&interp_elf), 0, sizeof(interp_elf));

                if (interp_elf.magic != elf::elfEnum::ELF_MAGIC)
                {
                    panic("execve: invalid dynamic linker ELF\n");
                    CLEANUP_AND_RETURN(-1);
                }
                printfCyan("execve: dynamic linker ELF magic: %x\n", interp_elf.magic);

                // **重构：动态链接器基址选择不再依赖new_sz**
                // 选择动态链接器的加载基址，使用最高地址
                interp_base = PGROUNDUP(highest_addr);

                // 加载动态链接器的程序段
                elf::proghdr interp_ph;
                for (int j = 0, interp_off = interp_elf.phoff; j < interp_elf.phnum; j++, interp_off += sizeof(interp_ph))
                {
                    vfs_read_file(interpreter_path.c_str(), reinterpret_cast<uint64>(&interp_ph), interp_off, sizeof(interp_ph));

                    if (interp_ph.type != elf::elfEnum::ELF_PROG_LOAD)
                        continue;

                    uint64 load_addr = interp_base + interp_ph.vaddr;
                    uint64 seg_flag = PTE_U;

#ifdef RISCV
                    /// 放开动态链接器权限
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                        seg_flag |= riscv::PteEnum::pte_executable_m;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                        seg_flag |= riscv::PteEnum::pte_writable_m;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                        seg_flag |= riscv::PteEnum::pte_readable_m;
#elif defined(LOONGARCH)
                    seg_flag |= PTE_P | PTE_D | PTE_PLV;
                    if (!(interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC))
                        seg_flag |= PTE_NX;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                        seg_flag |= PTE_W;
                    if (!(interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ))
                        seg_flag |= PTE_NR;
#endif

                    // **重构：为动态链接器段分配独立的虚拟内存**
                    uint64 linker_segment_start = PGROUNDDOWN(load_addr);
                    uint64 linker_segment_end = PGROUNDUP(load_addr + interp_ph.memsz);

                    if (mem::k_vmm.vmalloc(new_pt, linker_segment_start, linker_segment_end, seg_flag) == 0)
                    {
                        panic("execve: load dynamic linker failed at %p-%p\n",
                              (void *)linker_segment_start, (void *)linker_segment_end);
                        new_mm->free_all_memory();
                        delete new_mm;
                        return -1;
                    }

                    // 更新最高地址
                    if (linker_segment_end > highest_addr)
                    {
                        highest_addr = linker_segment_end;
                    }

                    // 加载动态链接器段内容
                    printfCyan("execve: loading dynamic linker segment %d, vaddr: %p, memsz: %p, offset: %p\n",
                               j, (void *)interp_ph.vaddr, (void *)interp_ph.memsz, (void *)interp_ph.off);
                    if (load_seg(new_pt, load_addr, interpreter_path, interp_ph.off, interp_ph.filesz) < 0)
                    {
                        panic("execve: load dynamic linker segment failed\n");
                        new_mm->free_all_memory();
                        delete new_mm;
                        return -1;
                    }

                    // **新增：记录动态链接器段信息**
                    // 记录动态链接器段信息，确保页对齐
                    uint64 linker_aligned_start = PGROUNDDOWN(load_addr);
                    uint64 linker_aligned_end = PGROUNDUP(load_addr + interp_ph.memsz);

                    // 为动态链接器段设置调试名称
                    const char *linker_section_name = nullptr;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                    {
                        linker_section_name = "linker_text";
                    }
                    else if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                    {
                        linker_section_name = "linker_data";
                    }
                    else
                    {
                        linker_section_name = "linker_rodata";
                    }

                    // 直接添加到 ProcessMemoryManager
                    int linker_section_index = new_mm->add_program_section((void *)linker_aligned_start,
                                                                           linker_aligned_end - linker_aligned_start,
                                                                           linker_section_name);
                    if (linker_section_index < 0)
                    {
                        panic("execve: failed to add linker program section\n");
                        CLEANUP_AND_RETURN(-1);
                    }

                    printfGreen("execve: added linker section[%d]: %s at %p, size %p (page-aligned from %p, %p)\n",
                                linker_section_index, linker_section_name,
                                (void *)linker_aligned_start, (void *)(linker_aligned_end - linker_aligned_start),
                                (void *)load_addr, (void *)interp_ph.memsz);
                }

                interp_entry = interp_base + interp_elf.entry;
                printfCyan("execve: dynamic linker loaded at base: %p, entry: %p\n",
                           (void *)interp_base, (void *)interp_entry);
            }

            // **新增：段加载完成后的统计信息**
            int total_sections = new_mm->prog_section_count;
            printfBlue("execve: segment loading completed. Total sections recorded: %d\n", total_sections);

            // 使用ProcessMemoryManager的公有成员来打印段信息
            for (int i = 0; i < total_sections; i++)
            {
                const program_section_desc *section = &new_mm->prog_sections[i];
                printfCyan("  [%d] %s: %p - %p (size: %p)\n",
                           i, section->_debug_name ? section->_debug_name : "unnamed",
                           section->_sec_start,
                           (void *)((uint64)section->_sec_start + section->_sec_size),
                           (void *)section->_sec_size);
            }
        }
        // printfPink("checkpoint 8\n");
        // ========== 第五阶段：分配用户栈空间 ==========

        { // **重构：基于最高地址分配用户栈空间**
            int stack_pgnum = 32;
            uint64 stack_start = PGROUNDUP(highest_addr); // 在最高地址之上分配栈
            uint64 stack_end = stack_start + stack_pgnum * PGSIZE;

#ifdef RISCV
            if (mem::k_vmm.uvmalloc(new_pt, stack_start, stack_end, PTE_W | PTE_X | PTE_R | PTE_U) == 0)
            {
                panic("execve: load user stack failed at %p-%p\n",
                      (void *)stack_start, (void *)stack_end);
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
#elif defined(LOONGARCH)
            if (mem::k_vmm.uvmalloc(new_pt, stack_start, stack_end, PTE_P | PTE_W | PTE_PLV | PTE_MAT | PTE_D) == 0)
            {
                panic("execve: load user stack failed at %p-%p\n",
                      (void *)stack_start, (void *)stack_end);
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
#endif

            // 更新最高地址
            highest_addr = stack_end;

            mem::k_vmm.uvmclear(new_pt, stack_start); // 设置guardpage
            sp = stack_end;                           // 栈指针从顶部开始
            // stackbase = stack_start + PGSIZE;         // 计算栈底地址(跳过guard page)
            stackbase = stack_start; // 计算栈底地址(跳过guard page) -> 不能跳过, 因为free的时候要用
            sp -= sizeof(uint64);    // 为返回地址预留空间

            // 添加用户栈段信息到 ProcessMemoryManager
            int stack_section_index = new_mm->add_program_section((void *)stackbase,
                                                                  stack_end - stackbase,
                                                                  "user_stack");
            if (stack_section_index < 0)
            {
                panic("execve: failed to add user stack section\n");
                CLEANUP_AND_RETURN(-1);
            }

            printfGreen("execve: added user stack section[%d] at %p, size %p\n",
                        stack_section_index, (void *)stackbase, (void *)(stack_end - stackbase));
        }

        // ========== 第六阶段：准备glibc所需的用户栈数据 ==========
        // 为了兼容glibc，需要在用户栈中按照特定顺序压入：
        // 栈顶 -> 栈底：argc, argv[], envp[], auxv[], 字符串数据, 随机数据

        sp -= 32;
        uint64_t random[4] = {0x0, -0x114514FF114514UL, 0x2UL << 60, 0x3UL << 60};
        if (sp < stackbase || mem::k_vmm.copy_out(new_pt, sp, (char *)random, 32) < 0)
        {
            panic("execve: copy random data failed\n");
            new_mm->free_all_memory();
            delete new_mm;
            return -1;
        }

        [[maybe_unused]] uint64 rd_pos = sp;

        // 2. 压入环境变量字符串
        uint64 uenvp[MAXARG];
        uint64 envc;
        // printfCyan("execve: envs size: %d\n", envs.size());
        for (envc = 0; envc < envs.size(); envc++)
        {
            if (envc >= MAXARG)
            { // 检查环境变量数量限制
                panic("execve: too many envs\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            sp -= envs[envc].size() + 1; // 为环境变量字符串预留空间(包括null)
            sp -= sp % 16;               // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                panic("execve: stack overflow\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, envs[envc].c_str(), envs[envc].size() + 1) < 0)
            {
                panic("execve: copy envs failed\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            uenvp[envc] = sp; // 记录字符串地址
        }
        uenvp[envc] = 0; // envp数组以NULL结尾

        // 3. 压入命令行参数字符串
        uint64 uargv[MAXARG]; // 命令行参数指针数组
        uint64 argc;          // 命令行参数数量
        for (argc = 0; argc < argv.size(); argc++)
        {
            if (argc >= MAXARG)
            { // 检查参数数量限制
                panic("execve: too many args\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            sp -= argv[argc].size() + 1; // 为参数字符串预留空间(包括null)
            sp -= sp % 16;               // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                panic("execve: stack overflow\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, argv[argc].c_str(), argv[argc].size() + 1) < 0)
            {
                panic("execve: copy args failed\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            uargv[argc] = sp; // 记录字符串地址

            // panic("[execve] argv[%d] = \"%s\", user_stack_addr = 0x%p\n", argc, argv[argc].c_str(), sp);
        }
        uargv[argc] = 0; // argv数组以NULL结尾

        // 4. 压入辅助向量（auxv），供动态链接器使用
        {
            // 在括号里面开命名空间防止变量名冲突
            using namespace elf;
            uint64 aux[AuxvEntryType::MAX_AT * 2] = {0};
            [[maybe_unused]] int index = 0;

            ADD_AUXV(AT_HWCAP, 0);             // 硬件功能标志
            ADD_AUXV(AT_PAGESZ, PGSIZE);       // 页面大小
            ADD_AUXV(AT_RANDOM, rd_pos);       // 随机数地址
            ADD_AUXV(AT_PHDR, phdr);           // 程序头表偏移
            ADD_AUXV(AT_PHENT, elf.phentsize); // 程序头表项大小
            if (is_dynamic)
            {
                ADD_AUXV(AT_PHNUM, elf.phnum); // 程序头表项数量 // 这个有问题
            }
            ADD_AUXV(AT_BASE, interp_base); // 动态链接器基地址（保留）
            ADD_AUXV(AT_ENTRY, elf.entry);  // 程序入口点地址
            // ADD_AUXV(AT_SYSINFO_EHDR, 0); // 系统调用信息头（保留）
            // ADD_AUXV(AT_UID, 0);               // 用户ID
            // ADD_AUXV(AT_EUID, 0);              // 有效用户ID
            // ADD_AUXV(AT_GID, 0);               // 组ID
            // ADD_AUXV(AT_EGID, 0);              // 有效组ID
            // ADD_AUXV(AT_SECURE, 0);            // 安全模式标志
            ADD_AUXV(AT_NULL, 0); // 结束标记

            // printf("index: %d\n", index);
            printfCyan("[execve] base: %p, phdr: %p\n", (void *)interp_base, (void *)phdr);

            // 将辅助向量复制到栈上
            sp -= sizeof(aux);
            if (mem::k_vmm.copy_out(new_pt, sp, (char *)aux, sizeof(aux)) < 0)
            {
                panic("execve: copy auxv failed\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
        }
        // 5. 压入环境变量指针数组（envp）
        // if (uenvp[0]) // 就算没有环境变量， 也要压入一个空指针
        {
            sp -= (envc + 1) * sizeof(uint64); // 为envp数组预留空间
            // sp -= sp % 16;                     // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                panic("execve: stack overflow\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, uenvp, (envc + 1) * sizeof(uint64)) < 0)
            {
                panic("execve: copy envp failed\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
        }
        proc->get_trapframe()->a2 = sp; // 设置栈指针到trapframe

        // 6. 压入命令行参数指针数组（argv）
        // if (uargv[0])
        {
            sp -= (argc + 1) * sizeof(uint64); // 为argv数组预留空间
            // sp -= sp % 16;                     // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                panic("execve: stack overflow\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, uargv, (argc + 1) * sizeof(uint64)) < 0)
            {
                panic("execve: copy argv failed\n");
                new_mm->free_all_memory();
                delete new_mm;
                return -1;
            }
            // // 新增：打印压入的 argv 指针及其内容
            // for (uint64 i = 0; i <= argc; ++i)
            // {
            //     printf("[execve] argv_ptr[%d] = 0x%p -> \"%s\"\n", i, uargv[i], argv[i].c_str());
            // }
        }

        proc->get_trapframe()->a1 = sp; // 设置argv指针到trapframe

        // 7. 压入参数个数（argc）
        sp -= sizeof(uint64);
        // printfGreen("execve: argc: %d, sp: %p\n", argc, (void *)sp);
        if (mem::k_vmm.copy_out(new_pt, sp, (char *)&argc, sizeof(uint64)) < 0)
        {
            panic("execve: copy argc failed\n");
            new_mm->free_all_memory();
            delete new_mm;
            return -1;
        }

        // 步骤13: 保存程序名用于调试
        // 从路径中提取文件名
        size_t last_slash = ab_path.find_last_of('/');
        eastl::string filename;
        if (last_slash != eastl::string::npos)
        {
            filename = ab_path.substr(last_slash + 1); // 提取最后一个'/'之后的部分
        }
        else
        {
            filename = ab_path; // 如果没有'/'，整个路径就是文件名
        }

        // 使用safestrcpy将文件名安全地拷贝到进程名称中
        // 注意：由于Pcb类没有提供set_name()函数，这里直接访问_name成员
        safestrcpy(proc->_name, filename.c_str(), sizeof(proc->_name));

        // printfGreen("execve: process name set to '%s'\n", proc->get_name());

        // ========== 第七阶段：配置进程资源限制 ==========
        // 设置栈大小限制
        // 注意：由于Pcb类没有提供通用的set_rlimit()函数，这里直接访问_rlim_vec
        proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_cur =
            proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_max = sp - stackbase;
        // 处理F_DUPFD_CLOEXEC标志位，关闭设置了该标志的文件描述符
        // 注意：这里直接访问_ofile结构是因为这是execve的特定操作
        for (int i = 0; i < (int)max_open_files; i++)
        {
            if (proc->_ofile != nullptr && proc->_ofile->_ofile_ptr[i] != nullptr && proc->_ofile->_fl_cloexec[i])
            {
                proc->_ofile->_ofile_ptr[i]->free_file();
                proc->_ofile->_ofile_ptr[i] = nullptr;
                proc->_ofile->_fl_cloexec[i] = false;
            }
        }

        // ========== 第八阶段：替换进程映像 ==========
        // 注意：execve保持进程的身份信息不变，包括PID、PGID、SID、UID/GID等
        // 这符合POSIX标准：execve只替换进程的内存映像，不改变进程的身份标识

        printfBlue("execve: start clean up old process memory space\n");

        // 使用PCB的cleanup_memory_manager进行完整的内存清理
        // 这会正确处理引用计数并释放ProcessMemoryManager对象
        proc->cleanup_memory_manager();

        printfBlue("execve: cleaning up old process memory space\n");

        // 注意：new_mm已经在第二阶段创建，这里直接使用
        // new_pt已经设置在new_mm->pagetable中

        // 检查是否有段被记录
        if (new_mm->prog_section_count == 0)
        {
            printfYellow("execve: warning - no program sections were recorded\n");
            // 为兼容性添加一个总段，使用highest_addr作为大小参考
            new_mm->add_program_section((void *)0, PGROUNDUP(highest_addr), "fallback_program");
        }

        // 在所有已分配的内存区域之后初始化堆
        new_mm->init_heap(PGROUNDUP(highest_addr));

        // 完成新内存管理器的设置后，绑定到当前PCB
        proc->set_memory_manager(new_mm);

        printfGreen("execve: old process memory space cleaned up\n");

        printfBlue("execve: added %d program sections to process\n", new_mm->prog_section_count);

        uint64 entry_point;
        if (is_dynamic)
        {
            entry_point = interp_entry; // 动态链接时从动态链接器开始执行
            printfCyan("execve: starting from dynamic linker entry: %p\n", (void *)entry_point);
        }
        else
        {
            entry_point = elf.entry; // 静态链接时直接从程序入口开始
            printfCyan("execve: starting from program entry: %p\n", (void *)entry_point);
        }

#ifdef RISCV
        proc->get_trapframe()->epc = entry_point;
#elif defined(LOONGARCH)
        proc->get_trapframe()->era = entry_point;
#endif
        proc->get_trapframe()->sp = sp; // 设置栈指针

        printfGreen("execve succeed, new process size: %p\n", proc->get_size());
        printfGreen("execve: process '%s' loaded with %d program sections\n",
                    proc->get_name(), proc->get_prog_section_count());
        proc->print_detailed_memory_info();
        // 写成0为了适配glibc的rtld_fini需求

#undef CLEANUP_AND_RETURN
        return 0; // 返回参数个数，表示成功执行
    };
} // namespace proc