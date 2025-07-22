#include "proc_manager.hh"
#include "hal/cpu.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "scheduler.hh"
#include "libs/klib.hh"
#include "trap.hh"
#include "printer.hh"
#include "devs/device_manager.hh"
#include "fs/lwext4/ext4_errno.hh"
#ifdef RISCV
#include "devs/riscv/disk_driver.hh"
#elif defined(LOONGARCH)
#include "devs/loongarch/disk_driver.hh"
#endif

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

#include "fs/vfs/vfs_utils.hh"
#include "sys/syscall_defs.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/virtual_fs.hh"
extern "C"
{
    extern uint64 initcode_start[];
    extern uint64 initcode_end[];

    extern int init_main(void);
    extern char trampoline[]; // trampoline.S
    void _wrp_fork_ret(void)
    {
        // printf("into _wrapped_fork_ret\n");
        proc::k_pm.fork_ret();
    }
    extern char sig_trampoline[]; // sig_trampoline.S
}

namespace proc
{
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
            // 使用轮转式分配策略，避免总是从头找，提高公平性
            p = &k_proc_pool[(_last_alloc_proc_gid + i) % num_process];
            p->_lock.acquire();
            if (p->_state == ProcState::UNUSED)
            {
                /****************************************************************************************
                 * 基本进程标识和状态管理初始化
                 ****************************************************************************************/
                k_pm.alloc_pid(p);           // 分配全局唯一的进程ID
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
                p->_gid = 0;        // 真实组ID（root）
                p->_egid = 0;       // 有效组ID（root）

                /****************************************************************************************
                 * 进程状态和调度信息初始化
                 ****************************************************************************************/
                p->_chan = nullptr; // 清空睡眠等待通道
                p->_killed = 0;     // 清除终止标志
                p->_xstate = 0;     // 清除退出状态码

                // 设置调度相关字段：默认调度槽与优先级
                p->_slot = default_proc_slot;
                p->_priority = default_proc_prio;

                /****************************************************************************************
                 * 内存管理初始化
                 ****************************************************************************************/
                // 为该进程分配一页 trapframe 空间（用于中断时保存用户上下文）
                // printfYellow("[user pgtbl]==>alloc trapframe for proc %d\n", p->_global_id);
                if ((p->_trapframe = (TrapFrame *)mem::k_pmm.alloc_page()) == nullptr)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }

                p->_sz = 0;            // 初始化用户空间内存大小为0
                p->_shared_vm = false; // 不共享虚拟内存

                // 初始化虚拟内存区域管理
                p->_vma = new Pcb::VMA();
                p->_vma->_ref_cnt = 1;
                for (int i = 0; i < NVMA; ++i)
                {
                    p->_vma->_vm[i].used = 0; // 标记所有VMA为未使用
                }

#ifdef LOONGARCH
                p->elf_base = 0; // 初始化ELF加载基地址
#endif

                /****************************************************************************************
                 * 上下文切换初始化
                 ****************************************************************************************/
                // 初始化上下文结构体
                memset(&p->_context, 0, sizeof(p->_context));

                // 设置调度返回地址为 _wrp_fork_ret
                // 当调度器切换回该进程时，将从这里开始执行
                p->_context.ra = (uint64)_wrp_fork_ret;

                // 设置内核栈指针
                p->_context.sp = p->_kstack + PGSIZE;

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

                /****************************************************************************************
                 * 程序段描述初始化
                 ****************************************************************************************/
                p->_prog_section_cnt = 0; // 清空程序段计数
                for (int i = 0; i < max_program_section_num; ++i)
                {
                    p->_prog_sections[i]._sec_start = nullptr;
                    p->_prog_sections[i]._sec_size = 0;
                    p->_prog_sections[i]._debug_name = nullptr;
                }

                /****************************************************************************************
                 * 页表创建
                 ****************************************************************************************/
                // 创建进程自己的页表（空的页表）
                _proc_create_vm(p);
                if (p->_pt.get_base() == 0)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }

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
        // printf("into fork_ret\n");
        proc::Pcb *proc = get_cur_pcb();
        proc->_lock.release();

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
            fs::device_file *f_in = new fs::device_file();
            // fs::device_file *f_err = new fs::device_file();
            eastl::string pathout("/dev/stdout");
            fs::FileAttrs fAttrsout = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_out =
                new fs::device_file(fAttrsout, pathout, 1);

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

    void ProcessManager::_proc_create_vm(Pcb *p)
    {
        p->_pt = proc_pagetable(p);
    }

    void ProcessManager::freeproc(Pcb *p)
    {
        /****************************************************************************************
         * 虚拟内存区域管理清理
         ****************************************************************************************/
        // 处理VMA的引用计数
        bool should_free_vma = false;
        if (p->_vma != nullptr)
        {
            if (--p->_vma->_ref_cnt <= 0)
            {
                should_free_vma = true;
            }
            else
            {
                printfYellow("freeproc: vma ref count not zero, ref_cnt: %d\n", p->_vma->_ref_cnt);
            }
        }

        // 如果应该释放VMA，则处理所有VMA条目
        if (should_free_vma && p->_vma != nullptr)
        {
            for (int i = 0; i < NVMA; ++i)
            {
                // printfBlue("freeproc: checking vma %d, addr: %p, len: %d,used:%d\n", i, p->_vm[i].addr, p->_vm[i].len,p->_vm[i].used);
                if (p->_vma->_vm[i].used)
                {
                    // 只对文件映射进行写回操作
                    if (p->_vma->_vm[i].vfile != nullptr && p->_vma->_vm[i].flags == MAP_SHARED && (p->_vma->_vm[i].prot & PROT_WRITE) != 0)
                    {
                        p->_vma->_vm[i].vfile->write(p->_vma->_vm[i].addr, p->_vma->_vm[i].len);
                    }

                    // 只对文件映射释放文件引用
                    if (p->_vma->_vm[i].vfile != nullptr)
                    {
                        p->_vma->_vm[i].vfile->free_file();
                    }

                    // 修复vmunmap调用：逐页检查并取消映射
                    uint64 va_start = PGROUNDDOWN(p->_vma->_vm[i].addr);
                    uint64 va_end = PGROUNDUP(p->_vma->_vm[i].addr + p->_vma->_vm[i].len);

                    // 逐页检查并取消映射，避免对未映射的页面进行操作
                    for (uint64 va = va_start; va < va_end; va += PGSIZE)
                    {
                        mem::Pte pte = p->_pt.walk(va, 0);
                        if (!pte.is_null() && pte.is_valid())
                        {
                            // 只对实际映射的页面进行取消映射
                            mem::k_vmm.vmunmap(*p->get_pagetable(), va, 1, 1);
                        }
                    }
                    p->_vma->_vm[i].used = 0;
                }
            }
            // 只有当VMA引用计数为0时才删除VMA
            delete p->_vma;
        }

        // 重置VMA指针
        p->_vma = nullptr;
        p->_shared_vm = false; // 重置共享虚拟内存标志

        /****************************************************************************************
         * 内存管理清理
         ****************************************************************************************/
        // 释放trapframe页面
        if (p->_trapframe)
        {
            mem::k_pmm.free_page(p->_trapframe);
            p->_trapframe = nullptr;
        }

        // 释放页表
        if (p->_pt.get_base())
        {
            proc_freepagetable(p->_pt, p->_sz);
        }

        p->_sz = 0; // 重置用户空间内存大小

#ifdef LOONGARCH
        p->elf_base = 0; // 重置ELF基地址
#endif

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
        p->_gid = 0;  // 清除真实组ID
        p->_egid = 0; // 清除有效组ID

        /****************************************************************************************
         * 进程状态和调度信息清理
         ****************************************************************************************/
        p->_chan = nullptr;            // 清空睡眠等待通道
        p->_killed = 0;                // 清除终止标志
        p->_xstate = 0;                // 清除退出状态码
        p->_state = ProcState::UNUSED; // 标记进程控制块为未使用

        p->_slot = 0;     // 重置时间片
        p->_priority = 0; // 重置优先级

        /****************************************************************************************
         * 文件系统和I/O管理清理
         ****************************************************************************************/
        p->_cwd = nullptr;    // 清空当前工作目录
        p->_cwd_name.clear(); // 清空当前工作目录路径

        // 使用cleanup_ofile方法处理文件描述符表
        p->cleanup_ofile();

        /****************************************************************************************
         * 线程和同步原语清理
         ****************************************************************************************/
        p->_futex_addr = nullptr;  // 清空futex等待地址
        p->_clear_tid_addr = 0;    // 清空线程退出时需要清理的地址
        p->_robust_list = nullptr; // 清空健壮futex链表

        /****************************************************************************************
         * 信号处理清理
         ****************************************************************************************/
        // 使用cleanup_sighand方法处理信号处理结构
        p->cleanup_sighand();

        // 清空信号处理栈帧链表
        while (p->sig_frame != nullptr)
        {
            ipc::signal::signal_frame *next_frame = p->sig_frame->next;
            mem::k_pmm.free_page(p->sig_frame); // 释放当前信号处理帧
            p->sig_frame = next_frame;          // 移动到下一个帧
        }
        p->sig_frame = nullptr; // 清空信号处理帧指针
        p->_signal = 0;         // 清空待处理信号掩码
        p->_sigmask = 0;        // 清空信号屏蔽掩码

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
         * 程序段描述清理
         ****************************************************************************************/
        p->_prog_section_cnt = 0; // 清零程序段计数
        for (int i = 0; i < max_program_section_num; ++i)
        {
            p->_prog_sections[i]._sec_start = nullptr;
            p->_prog_sections[i]._sec_size = 0;
            p->_prog_sections[i]._debug_name = nullptr;
        }

        /****************************************************************************************
         * 上下文清理
         ****************************************************************************************/
        memset(&p->_context, 0, sizeof(p->_context)); // 清空上下文信息
    }

    int ProcessManager::get_cur_cpuid()
    {
        return r_tp();
    }

    mem::PageTable ProcessManager::proc_pagetable(Pcb *p)
    {
        mem::PageTable pt = mem::k_vmm.vm_create();
        mem::PageTable empty_pt = mem::PageTable();
        if (pt.is_null())
            printfRed("proc_pagetable: vm_create failed\n");
        if (pt.get_base() == 0)
        {
            printfRed("proc_pagetable: pt already exists\n");
            return empty_pt; // 如果已经有页表了，直接返回空页表
        }
#ifdef RISCV
        if (mem::k_vmm.map_pages(pt, TRAMPOLINE, PGSIZE, (uint64)trampoline, riscv::PteEnum::pte_readable_m | riscv::pte_executable_m) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);
            printfRed("proc_pagetable: map trampoline failed\n");
            return empty_pt;
        }
        // printfGreen("trampoline: %p\n", trampoline);
        // printfGreen("TRAMPOLINE: %p\n", TRAMPOLINE);
        if (mem::k_vmm.map_pages(pt, TRAPFRAME, PGSIZE, (uint64)(p->get_trapframe()), riscv::PteEnum::pte_readable_m | riscv::PteEnum::pte_writable_m) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);

            printfRed("proc_pagetable: map trapframe failed\n");
            return empty_pt;
        }
        if (mem::k_vmm.map_pages(pt, SIG_TRAMPOLINE, PGSIZE, (uint64)sig_trampoline, riscv::PteEnum::pte_readable_m | riscv::pte_executable_m | riscv::PteEnum::pte_user_m) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);

            panic("proc_pagetable: map sigtrapframe failed\n");
            return empty_pt;
        }

#elif defined(LOONGARCH)
        if (mem::k_vmm.map_pages(pt, TRAPFRAME, PGSIZE, (uint64)(p->_trapframe), PTE_V | PTE_NX | PTE_P | PTE_W | PTE_R | PTE_MAT | PTE_D) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);
            printfRed("proc_pagetable: map trapframe failed\n");
            return empty_pt;
        }
        if (mem::k_vmm.map_pages(pt, SIG_TRAMPOLINE, PGSIZE, (uint64)sig_trampoline, PTE_P | PTE_MAT | PTE_D | PTE_U) == 0)
        {
            printf("Fail to map sig_trampoline\n");
            mem::k_vmm.vmfree(pt, 0);
            return empty_pt;
        }
#endif
        return pt;
    }
    void ProcessManager::proc_freepagetable(mem::PageTable &pt, uint64 sz)
    {
        printfCyan("proc_freepagetable: freeing pagetable %p, size %u\n", pt.get_base(), sz);
#ifdef RISCV
        // riscv还有 trampoline的映射
        mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
#endif
        mem::k_vmm.vmunmap(pt, TRAPFRAME, 1, 0);
        mem::k_vmm.vmunmap(pt, SIG_TRAMPOLINE, 1, 0);
#ifdef RISCV
        mem::k_vmm.vmfree(pt, sz);
#elif LOONGARCH
        Pcb *proc = get_cur_pcb();
        // loongarch不是从0开始的，所以不需要从0开始释放
        mem::k_vmm.vmfree(pt, sz - proc->elf_base, proc->elf_base);
#endif
    }

    void ProcessManager::user_init()
    {
#ifdef RISCV
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

        // 传入initcode的地址
        printfCyan("initcode pagetable: %p\n", p->_pt.get_base());
        uint64 initcode_sz = (uint64)initcode_end - (uint64)initcode_start;
        p->_sz = mem::k_vmm.uvmfirst(p->_pt, (uint64)initcode_start, initcode_sz);
        
        printf("initcode start: %p, end: %p\n", initcode_start, initcode_end);
        printf("initcode size: %p, total allocated space: %p\n", initcode_sz, p->_sz);

        p->_trapframe->epc = 0;
        p->_trapframe->sp = p->_sz;

        safestrcpy(p->_name, "initcode", sizeof(p->_name));
        p->_parent = p;
        // safestrcpy(p->_cwd_name, "/", sizeof(p->_cwd_name));
        p->_cwd_name = "/";

        p->_state = ProcState::RUNNABLE;

        p->_lock.release();

#elif defined(LOONGARCH)
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

        // 传入initcode的地址
        printfCyan("initcode pagetable: %p\n", p->_pt.get_base());
        uint64 initcode_sz = (uint64)initcode_end - (uint64)initcode_start;
        p->_sz = mem::k_vmm.uvmfirst(p->_pt, (uint64)initcode_start, initcode_sz);
        
        printf("initcode start: %p, end: %p\n", initcode_start, initcode_end);
        printf("initcode size: %p, total allocated space: %p\n", initcode_sz, p->_sz);

        p->_trapframe->era = 0;     // 设置程序计数器为0
        p->_trapframe->sp = p->_sz; // 设置栈指针为总空间大小

        safestrcpy(p->_name, "initcode", sizeof(p->_name));
        p->_parent = p;
        // safestrcpy(p->_cwd_name, "/", sizeof(p->_cwd_name));
        p->_cwd_name = "/";

        p->_state = ProcState::RUNNABLE;

        p->_lock.release();
#endif
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
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();
            if (p->_pid == pid || (p->_parent != NULL && p->_parent->_pid == pid))
            {
                p->add_signal(sig);
                p->_lock.release();
                return 0;
            }
            p->_lock.release();
        }
        return -1;
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

    // Copy from either a user address, or kernel address,
    // depending on usr_src.
    // Returns 0 on success, -1 on error.
    int ProcessManager::either_copy_in(void *dst, int user_src, uint64 src, uint64 len)
    {
        Pcb *p = get_cur_pcb();
        if (user_src)
        {
            return mem::k_vmm.copy_in(p->_pt, dst, src, len);
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
            return mem::k_vmm.copy_out(p->_pt, dst, src, len);
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
            printf("%d %s %s", p->_pid, state, p->_name);
            printf("\n");
        }
    }
    int ProcessManager::alloc_fd(Pcb *p, fs::file *f, int fd)
    {
        // 越界检查
        if (fd < 0 || fd >= (int)max_open_files || f == nullptr || p->_ofile == nullptr)
            return -1;
        // 不为空先释放资源
        if (p->_ofile->_ofile_ptr[fd] != nullptr)
        {
            close(fd);
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
                return fd;
            }
        }
        return syscall::SYS_EMFILE;
    }

    int ProcessManager::clone(uint64 flags, uint64 stack_ptr, uint64 ptid, uint64 tls, uint64 ctid)
    {
        if (flags == 0)
        {
            return 22; // EINVAL: Invalid argument
        }
        Pcb *p = get_cur_pcb();
        Pcb *np = fork(p, flags, stack_ptr, ctid, false);
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
            if (mem::k_vmm.copy_out(p->_pt, ptid, &new_tid, sizeof(new_tid)) < 0)
            {
                freeproc(np);
                np->_lock.release();
                return -1; // EFAULT: Bad address
            }
        }
        if (flags & syscall::CLONE_PARENT)
        {
            _wait_lock.acquire();
            if (p->_parent != nullptr)
            {
                np->_parent = p->_parent; // 继承父进程
            }
            else
            {
                panic("clone: parent process is null");
            }
            _wait_lock.release();
        }
        np->_lock.release();
        return new_pid;
    }

    // 这个函数主要用提供clone的底层支持
    Pcb *ProcessManager::fork(Pcb *p, uint64 flags, uint64 stack_ptr, uint64 ctid, bool is_clone3)
    {
        TODO("copy on write fork");
        uint64 i;
        Pcb *np; // new proc

        // Allocate process.
        if ((np = alloc_proc()) == nullptr)
        {
            return nullptr;
        }
        *np->_trapframe = *p->_trapframe; // 拷贝父进程的陷阱值，而不是直接指向, 后面有可能会修改
        // 继承父进程的其他属性
        np->_sz = p->_sz;
#ifdef LOONGARCH
        np->elf_base = p->elf_base; // 继承 ELF 基地址
#endif

        _wait_lock.acquire();
        np->_parent = p;
        _wait_lock.release();

        np->_cwd = p->_cwd;           // 继承当前工作目录
        np->_cwd_name = p->_cwd_name; // 继承当前工作目录名称

        // 继承父进程的身份信息
        np->_ppid = p->_pid;
        np->_pgid = p->_pgid;
        np->_sid = p->_sid;
        np->_uid = p->_uid;
        np->_euid = p->_euid;
        np->_gid = p->_gid;
        np->_egid = p->_egid;

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

        np->_user_ticks = 0;

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
        mem::PageTable *curpt, *newpt;
        curpt = p->get_pagetable();
        newpt = np->get_pagetable();
        if (flags & syscall::CLONE_VM)
        {
            // 共享虚拟内存：新进程共享父进程的页表
            np->_pt.share_from(p->_pt); // 共享父进程的页表

            np->_vma = p->_vma;  // 继承父进程的虚拟内存区域映射
            p->_vma->_ref_cnt++; // 增加父进程的虚拟内存区域映射引用计数

            // 在共享页表的情况下，需要标记为共享虚拟内存
            // 因为子进程有自己的trapframe，但共享父进程的页表
            // 我们需要在usertrapret时动态映射正确的trapframe
            np->_shared_vm = true; // 标记为共享虚拟内存

            printfCyan("[clone] Using shared page table for process %d (parent %d), ref count: %d\n",
                       np->_pid, p->_pid, np->_pt.get_ref_count());
        }
        else
        {
#ifdef RISCV
            if (mem::k_vmm.vm_copy(*curpt, *newpt, 0, p->_sz) < 0)
            {
                freeproc(np);
                np->_lock.release();
                return nullptr;
            }
#elif LOONGARCH
            if (mem::k_vmm.vm_copy(*curpt, *newpt, p->elf_base, p->_sz - p->elf_base) < 0)
            {
                freeproc(np);
                np->_lock.release();
                return nullptr;
            }
#endif
            for (i = 0; i < NVMA; ++i)
            {
                if (p->_vma->_vm[i].used)
                {
                    memmove(&np->_vma->_vm[i], &p->_vma->_vm[i], sizeof(p->_vma->_vm[i]));
                    // 只对文件映射增加引用计数
                    if (p->_vma->_vm[i].vfile != nullptr)
                    {
                        p->_vma->_vm[i].vfile->dup(); // 增加引用计数
                    }
                }
            }
        }

        // 处理信号处理共享
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
                mem::k_vmm.copy_in(p->_pt, &entry_point, stack_ptr, sizeof(uint64));
                if (entry_point == 0)
                {
                    panic("fork: copy_in failed for stack pointer");
                    freeproc(np);
                    np->_lock.release();
                    return nullptr;
                }
                uint64 arg = 0;
                if (mem::k_vmm.copy_in(p->_pt, &arg, (stack_ptr + 8), sizeof(uint64)) != 0)
                {
                    panic("fork: copy_in failed for stack pointer arg");
                    freeproc(np);
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
                if (mem::k_vmm.copy_out(p->_pt, ctid, &np->_tid, sizeof(np->_tid)) < 0)
                {
                    freeproc(np);
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
        uint64 sz;
        Pcb *p = get_cur_pcb();

        sz = p->_sz;
        if (n > 0)
        {
            if (sz + n >= MAXVA - PGSIZE)
                return -1;
            if ((sz = mem::k_vmm.uvmalloc(p->_pt, sz, sz + n, PTE_W)) == 0)
            {
                return -1;
            }
        }
        else if (n < 0)
        {
            sz = mem::k_vmm.uvmdealloc(p->_pt, sz, sz + n);
        }
        p->_sz = sz;
        return 0;
    }

    /// @brief
    /// @param n 参数n是地址，意思是扩展到 n 地址
    /// 如果 n == 0，则返回当前进程的内存大小
    /// @return
    long ProcessManager::brk(long n)
    {
        uint64 addr = get_cur_pcb()->_sz;
        if (n == 0)
        {
            return addr;
        }
        // printfCyan("[brk]  let's map to %d,now our size is%d\n",n,addr);
        if (growproc(n - addr) < 0)
        {
            return -1;
        }
        return n;
    }

    int ProcessManager::wait4(int child_pid, uint64 addr, int option)
    {
        // copy from RUOK-os
        Pcb *p = k_pm.get_cur_pcb();
        int havekids, pid;
        Pcb *np = nullptr;
        if (child_pid > 0)
        {
            // 如果指定了 child_pid（大于 0），说明只等待这个特定子进程
            bool has_child = false;
            // 遍历进程池，查找是否存在这个特定子进程，且它的父进程是当前进程
            for (auto &tmp : k_proc_pool)
            {
                if (tmp._pid == child_pid && tmp._parent == p)
                {
                    has_child = true;
                    break;
                }
            }
            if (!has_child)
                return -1;
        }

        _wait_lock.acquire();
        for (;;)
        {
            havekids = 0;
            for (np = k_proc_pool; np < &k_proc_pool[num_process]; np++)
            {
                if (child_pid > 0 && np->_pid != child_pid)
                    continue;

                if (np->_parent == p)
                {
                    np->_lock.acquire();
                    havekids = 1;
                    printfGreen("[wait4]: child %d state: %d name: %s\n", np->_pid, (int)np->_state, np->_name);
                    if (np->get_state() == ProcState::ZOMBIE)
                    {
                        pid = np->_pid;
                        // printf("[wait4]: child->xstate: %d\n", np->_xstate);
                        if (addr != 0 &&
                            mem::k_vmm.copy_out(p->_pt, addr, (const char *)&np->_xstate,
                                                sizeof(np->_xstate)) < 0)
                        {
                            np->_lock.release();
                            _wait_lock.release();
                            return -1;
                        }
                        /// @todo release shm

                        k_pm.freeproc(np);
                        np->_lock.release();
                        _wait_lock.release();
                        return pid;
                    }
                    np->_lock.release();
                }
            }

            // WNOHANG: 如果设置了 WNOHANG 选项，则不阻塞等待子进程退出
            if (option & syscall::WNOHANG)
            {
                _wait_lock.release();
                return 0; // 立即返回，不阻塞
            }

            if (!havekids || p->_killed)
            {
                _wait_lock.release();
                return -1;
            }

            // wait children to exit
            sleep(p, &_wait_lock);
        }
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
    void ProcessManager::exit_proc(Pcb *p, int state)
    {

        if (p == _init_proc)
            panic("init exiting"); // 保护机制：init 进程不能退出
        // log_info( "exit proc %d", p->_pid );

        reparent(p); // 将 p 的所有子进程交给 init 进程收养
        _wait_lock.acquire();

        if (p->_parent)
            wakeup(p->_parent); // 唤醒父进程（可能在 wait() 中阻塞）)
        if (p->_clear_tid_addr)
        {
            uint64 temp0 = 0;
            if (mem::k_vmm.copy_out(p->_pt, p->_clear_tid_addr, &temp0, sizeof(temp0)) < 0)
            {
                printfRed("exit_proc: copy out ctid failed\n");
            }
        }

        p->_lock.acquire();
        p->_xstate = state << 8;       // 存储退出状态（通常高字节存状态）
        p->_state = ProcState::ZOMBIE; // 标记为 zombie，等待父进程回收

        // 如果有父进程，将当前进程的时间累计到父进程中
        if (p->_parent != nullptr)
        {
            p->_parent->_lock.acquire();
            p->_parent->_cutime += p->_user_ticks + p->_cutime;
            p->_parent->_cstime += p->_stime + p->_cstime;
            p->_parent->_lock.release();
        }

        _wait_lock.release();
        // printf("[exit_proc] proc %s pid %d exiting with state %d\n", p->_name, p->_pid, state);
        k_scheduler.call_sched(); // jump to schedular, never return
        panic("zombie exit");
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
    /// @param state   调用 exit_proc 处理退出逻辑
    /// “一荣俱荣，一损俱损” commented by @gkq
    void ProcessManager::exit(int state)
    {
        Pcb *p = get_cur_pcb();
        printf("[exit] proc %s pid %d tid %d exiting with state %d\n", p->_name, p->_pid, p->_tid, state);
        exit_proc(p, state);
    }

    /// @brief 当前线程组全部退出
    /// @param status
    /// https://man7.org/linux/man-pages/man2/exit_group.2.html
    void ProcessManager::exit_group(int status)
    {
        TODO("rm /temp")
        proc::Pcb *cp = get_cur_pcb();

        _wait_lock.acquire();

        for (uint i = 0; i < num_process; i++)
        {
            if (k_proc_pool[i]._state == ProcState::UNUSED)
                continue;
            proc::Pcb *p = &k_proc_pool[i];
            // 释放同一线程组中其他线程的资源
            if (p != cp && p->_tgid == cp->_tgid)
            {
                // 退出同一线程组的其他线程
                freeproc(p);
            }
        }
        _wait_lock.release();

        exit_proc(cp, status);
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

                if (count1 >= val && count2 >= val2)
                {
                    p->_lock.release();
                    break;
                }
            }
            p->_lock.release();
        }
        return count1;
    }
    int ProcessManager::mkdir(int dir_fd, eastl::string path, uint flags)
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        Pcb *p = get_cur_pcb();
        [[maybe_unused]] fs::file *file = nullptr;

        if (dir_fd != AT_FDCWD)
        {
            panic("mkdir: dir_fd != AT_FDCWD not implemented");
            file = p->get_open_file(dir_fd);
        }

        const char *dirpath = (dir_fd == AT_FDCWD) ? p->_cwd_name.c_str() : p->_ofile->_ofile_ptr[dir_fd]->_path_name.c_str();
        eastl::string absolute_path = get_absolute_path(path.c_str(), dirpath);
        vfs_mkdir(absolute_path.c_str(), 0777); //< 传入绝对路径，权限777表示所有人都可RWX

        return 0;
    }
    /// @brief
    /// @param dir_fd 指定相对路径的目录文件描述符（AT_FDCWD 表示当前工作目录）。
    /// @param path 要打开的路径
    /// @param flags 打开方式（如只读、只写、创建等）
    /// @return fd
    int ProcessManager::open(int dir_fd, eastl::string path, uint flags)
    {
        printfCyan("[open] dir_fd: %d, path: %s, flags: %s\n", dir_fd, path.c_str(), flags_to_string(flags).c_str());

        Pcb *p = get_cur_pcb();
        // fs::file *file = nullptr;

        // struct filesystem *fs = get_fs_from_path(path.c_str());
        fs::file *file = nullptr;
        int fd = alloc_fd(p, file);
        if (fd < 0)
        {
            printfRed("[open] alloc_fd failed for path: %s\n", path.c_str());
            return -EMFILE; // 分配文件描述符失败
        }
        // 下面这个就是套的第二层，这一层的意义似乎只在于分配文件描述符
        int err = fs::k_vfs.openat(path, p->_ofile->_ofile_ptr[fd], flags);
        if (err < 0)
        {
            printfRed("[open] failed for path: %s\n", path.c_str());
            return err; // 文件不存在或打开失败
        }
        return fd; // 返回分配的文件描述符
    }

    int ProcessManager::close(int fd)
    {
        if (fd < 0 || fd >= (int)max_open_files)
            return -1;
        Pcb *p = get_cur_pcb();
        if (p->_ofile == nullptr || p->_ofile->_ofile_ptr[fd] == nullptr)
            return 0;
        // fs::k_file_table.free_file( p->_ofile[ fd ] );
        p->_ofile->_ofile_ptr[fd]->free_file();
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
            return -1;

        Pcb *p = get_cur_pcb();
        if (p->_ofile == nullptr || p->_ofile->_ofile_ptr[fd] == nullptr)
            return -1;
        fs::file *f = p->_ofile->_ofile_ptr[fd];
        vfs_fstat(f, buf);

        return 0;
    }
    int ProcessManager::chdir(eastl::string &path)
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
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
    int ProcessManager::validate_mmap_params(void *addr, int length, int prot, int flags, int fd, int offset)
    {
        // 长度检查
        if (length <= 0) {
            return syscall::SYS_EINVAL;
        }

        // 检查必须的共享标志 - 必须指定MAP_SHARED或MAP_PRIVATE之一
        bool has_shared = flags & MAP_SHARED;
        bool has_private = flags & MAP_PRIVATE;
        
        if (!has_shared && !has_private) {
            return syscall::SYS_EINVAL; // 必须指定共享类型
        }
        
        if (has_shared && has_private) {
            return syscall::SYS_EINVAL; // 不能同时指定
        }

        // 检查保护标志的合理性
        if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE)) {
            return syscall::SYS_EINVAL; // 无效的保护标志
        }

        // 检查匿名映射
        bool is_anonymous = (flags & MAP_ANONYMOUS) || (fd == -1);
        
        if (is_anonymous) {
            if (offset != 0) {
                return syscall::SYS_EINVAL; // 匿名映射offset必须为0
            }
            // 匿名映射通常要求fd为-1
            if (!(flags & MAP_ANONYMOUS) && fd != -1) {
                return syscall::SYS_EBADF; // 不一致的匿名映射设置
            }
        } else {
            // 文件映射的fd验证在主函数中进行，因为需要访问进程状态
            if (fd < 0) {
                return syscall::SYS_EBADF;
            }
        }

        // MAP_FIXED相关检查
        if (flags & MAP_FIXED) {
            if (addr == nullptr) {
                return syscall::SYS_EINVAL; // MAP_FIXED需要指定地址
            }
            // 检查地址对齐（大多数架构要求页对齐）
            if ((uint64)addr % PGSIZE != 0) {
                return syscall::SYS_EINVAL;
            }
            
            // MAP_FIXED_NOREPLACE不能与MAP_FIXED同时使用（Linux实现中）
            if ((flags & MAP_FIXED_NOREPLACE) && !(flags & MAP_FIXED)) {
                return syscall::SYS_EINVAL;
            }
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
    /// @return 成功返回映射地址，失败返回MAP_FAILED
    void *ProcessManager::mmap(void *addr, int length, int prot, int flags, int fd, int offset)
    {
        printfYellow("[mmap] addr: %p, length: %d, prot: %d, flags: %d, fd: %d, offset: %d\n",
                     addr, length, prot, flags, fd, offset);

        // 参数验证
        int validation_result = validate_mmap_params(addr, length, prot, flags, fd, offset);
        if (validation_result != 0) {
            printfRed("[mmap] Parameter validation failed: %d\n", validation_result);
            return MAP_FAILED;
        }

        Pcb *p = get_cur_pcb();

        // 检查是否为匿名映射
        bool is_anonymous = (flags & MAP_ANONYMOUS) || (fd == -1);
        
        // 匿名映射验证
        if (is_anonymous) {
            if (fd != -1 && !(flags & MAP_ANONYMOUS)) {
                printfRed("[mmap] Anonymous mapping but fd != -1\n");
                return MAP_FAILED;
            }
            if (offset != 0) {
                printfRed("[mmap] Anonymous mapping with non-zero offset\n");
                return MAP_FAILED;
            }
        }

        // 文件映射验证
        fs::normal_file *vfile = nullptr;
        fs::file *f = nullptr;
        if (!is_anonymous) {
            if (p->_ofile == nullptr || fd < 0 || fd >= (int)max_open_files || 
                p->_ofile->_ofile_ptr[fd] == nullptr) {
                printfRed("[mmap] Invalid file descriptor: %d\n", fd);
                return MAP_FAILED;
            }
            
            f = p->get_open_file(fd);
            if (f->_attrs.filetype != fs::FileTypes::FT_NORMAL) {
                printfRed("[mmap] File descriptor does not refer to regular file\n");
                return MAP_FAILED;
            }
            
            // 检查文件访问权限
            if (prot & PROT_READ) {
                // 文件必须可读
                // TODO: 检查文件打开模式是否支持读取
            }
            if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) {
                // 共享写映射要求文件以读写模式打开
                // TODO: 检查文件打开模式是否支持写入
            }
            
            vfile = static_cast<fs::normal_file *>(f);
            printfCyan("[mmap] File mapping: %s\n", f->_path_name.c_str());
        } else {
            printfCyan("[mmap] Anonymous mapping\n");
        }

        // 地址对齐
        uint64 aligned_length = PGROUNDUP(length);
        if (aligned_length + p->_sz > MAXVA - PGSIZE) {
            printfRed("[mmap] Would exceed virtual address space\n");
            return MAP_FAILED;
        }

        // 查找空闲VMA
        int vma_idx = -1;
        for (int i = 0; i < NVMA; ++i) {
            if (!p->_vma->_vm[i].used) {
                vma_idx = i;
                break;
            }
        }
        
        if (vma_idx == -1) {
            printfRed("[mmap] No available VMA slots\n");
            return MAP_FAILED;
        }

        // 确定映射地址
        uint64 map_addr;
        if (flags & MAP_FIXED) {
            if (addr == nullptr) {
                printfRed("[mmap] MAP_FIXED requires non-null addr\n");
                return MAP_FAILED;
            }
            
            if(is_page_align((uint64)addr) == false) {
                printfRed("[mmap] MAP_FIXED address must be page aligned\n");
                return MAP_FAILED;
            }
            map_addr = (uint64)addr;
            
            if (flags & MAP_FIXED_NOREPLACE) {
                // 检查是否与现有映射冲突
                for (int i = 0; i < NVMA; ++i) {
                    if (p->_vma->_vm[i].used) {
                        uint64 existing_start = p->_vma->_vm[i].addr;
                        uint64 existing_end = existing_start + p->_vma->_vm[i].len;
                        uint64 new_end = map_addr + aligned_length;
                        
                        if (!(new_end <= existing_start || map_addr >= existing_end)) {
                            printfRed("[mmap] MAP_FIXED_NOREPLACE: address range conflicts\n");
                            return MAP_FAILED;
                        }
                    }
                }
            } else {
                // MAP_FIXED 可以覆盖现有映射
                // TODO: 取消映射冲突区域

            }
        } else {
            // 系统选择地址
            if (addr != nullptr) {
                // 作为提示使用
                map_addr = PGROUNDUP((uint64)addr);
            } else {
                map_addr = p->_sz;
            }
        }

        // 初始化VMA
        struct vma *vm = &p->_vma->_vm[vma_idx];
        vm->used = 1;
        vm->addr = map_addr;
        vm->len = aligned_length;
        vm->prot = prot;
        vm->flags = flags;
        vm->vfd = is_anonymous ? -1 : fd;
        vm->vfile = vfile;
        vm->offset = offset;
        
        // 设置扩展属性
        if (is_anonymous) {
            vm->is_expandable = !(flags & MAP_FIXED);
            vm->max_len = (flags & MAP_FIXED) ? aligned_length : (MAXVA - map_addr);
        } else {
            vm->is_expandable = false;
            vm->max_len = aligned_length;
            vfile->dup(); // 增加文件引用计数
        }

        // 更新进程大小
        if (!(flags & MAP_FIXED)) {
            p->_sz += aligned_length;
        } else {
            uint64 end_addr = map_addr + aligned_length;
            if (end_addr > p->_sz) {
                p->_sz = end_addr;
            }
        }

        // 特殊标志处理
        if (flags & MAP_POPULATE) {
            // TODO: 预分配页面
            printfCyan("[mmap] MAP_POPULATE: will prefault pages\n");
        }
        
        if (flags & MAP_LOCKED) {
            // TODO: 锁定页面在内存中
            printfCyan("[mmap] MAP_LOCKED: pages will be locked in memory\n");
        }

        printfGreen("[mmap] Success: addr=%p, len=%d, prot=%d, flags=%d\n", 
                   (void*)map_addr, aligned_length, prot, flags);
        
        return (void *)map_addr;
    }
    /// @brief 取消内存映射，符合POSIX标准的munmap实现
    /// @param addr 要取消映射的起始地址，必须页对齐
    /// @param length 要取消映射的长度（字节）
    /// @return 成功返回0，失败返回-1
    int ProcessManager::munmap(void *addr, int length)
    {
        if (addr == nullptr || length <= 0) {
            printfRed("[munmap] Invalid parameters: addr=%p, length=%d\n", addr, length);
            return -1;
        }

        // 地址必须页对齐
        if ((uint64)addr % PGSIZE != 0) {
            printfRed("[munmap] Address not page aligned: %p\n", addr);
            return -1;
        }

        Pcb *p = get_cur_pcb();
        uint64 unmap_start = (uint64)addr;
        uint64 unmap_end = unmap_start + length;
        uint64 aligned_length = PGROUNDUP(length);

        printfYellow("[munmap] addr=%p, length=%d (aligned=%u)\n", addr, length, aligned_length);

        // 查找覆盖此地址范围的所有VMA
        for (int i = 0; i < NVMA; ++i) {
            if (!p->_vma->_vm[i].used) continue;

            struct vma *vm = &p->_vma->_vm[i];
            uint64 vma_start = vm->addr;
            uint64 vma_end = vma_start + vm->len;

            // 检查是否有重叠
            if (unmap_end <= vma_start || unmap_start >= vma_end) {
                continue; // 没有重叠
            }

            printfCyan("[munmap] Found overlapping VMA %d: [%p, %p)\n", i, (void*)vma_start, (void*)vma_end);

            // 计算重叠区域
            uint64 overlap_start = MAX(unmap_start, vma_start);
            uint64 overlap_end = MIN(unmap_end, vma_end);

            // 处理MAP_SHARED文件映射的写回
            if ((vm->flags & MAP_SHARED) && (vm->prot & PROT_WRITE) && vm->vfile != nullptr) {
                // TODO: 实现脏页写回
                printfCyan("[munmap] Should write back MAP_SHARED pages for file %s\n", 
                          vm->vfile->_path_name.c_str());
                // 对于现在，我们跳过写回，因为文件系统接口还不完整
            }

            // 取消物理页面映射
            uint64 va_start = PGROUNDDOWN(overlap_start);
            uint64 va_end = PGROUNDUP(overlap_end);
            
            for (uint64 va = va_start; va < va_end; va += PGSIZE) {
                mem::Pte pte = p->_pt.walk(va, 0);
                if (!pte.is_null() && pte.is_valid()) {
                    // 取消映射并释放物理页面
                    mem::k_vmm.vmunmap(*p->get_pagetable(), va, 1, 1);
                    printfCyan("[munmap] Unmapped page at va=%p\n", (void*)va);
                }
            }

            // 更新VMA结构
            if (overlap_start == vma_start && overlap_end == vma_end) {
                // 完全取消映射整个VMA
                printfCyan("[munmap] Completely unmapping VMA %d\n", i);
                
                if (vm->vfile != nullptr) {
                    vm->vfile->free_file(); // 减少文件引用计数
                }
                
                vm->used = 0;
                vm->addr = 0;
                vm->len = 0;
                vm->vfile = nullptr;
                vm->vfd = -1;
                
            } else if (overlap_start == vma_start) {
                // 从头部开始取消映射
                printfCyan("[munmap] Unmapping from start of VMA %d\n", i);
                
                uint64 remaining_len = vma_end - overlap_end;
                vm->addr = overlap_end;
                vm->len = remaining_len;
                vm->offset += (overlap_end - vma_start);
                
            } else if (overlap_end == vma_end) {
                // 从尾部开始取消映射
                printfCyan("[munmap] Unmapping from end of VMA %d\n", i);
                
                vm->len = overlap_start - vma_start;
                
            } else {
                // 从中间取消映射，需要分割VMA
                printfRed("[munmap] Middle unmapping not fully supported yet\n");
                // TODO: 实现VMA分割，需要找到空闲VMA槽位来创建第二个VMA
                // 目前简单处理：截断到取消映射的起始位置
                vm->len = overlap_start - vma_start;
            }
        }

        printfGreen("[munmap] Successfully unmapped range [%p, %p)\n", addr, (void*)unmap_end);
        return 0;
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
        if (!old_address) {
            printfRed("[mremap] EINVAL: old_address is NULL\n");
            return syscall::SYS_EINVAL;
        }

        if (old_size == 0) {
            // 特殊情况：old_size为0时，old_address必须引用共享映射且必须指定MREMAP_MAYMOVE
            if (!(flags & MREMAP_MAYMOVE)) {
                printfRed("[mremap] EINVAL: old_size is 0 but MREMAP_MAYMOVE not specified\n");
                return syscall::SYS_EINVAL;
            }
            // 这里应该检查old_address是否引用共享映射，暂时简化处理
            printfYellow("[mremap] WARNING: old_size=0 case not fully implemented\n");
        }

        if (new_size == 0) {
            printfRed("[mremap] EINVAL: new_size is zero\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: 检查地址是否页对齐
        if ((uintptr_t)old_address & (PGSIZE - 1)) {
            printfRed("[mremap] EINVAL: old_address not page aligned: %p\n", old_address);
            return syscall::SYS_EINVAL;
        }

        // EINVAL: 验证标志位
        if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP)) {
            printfRed("[mremap] EINVAL: Invalid flags: 0x%x\n", flags);
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_FIXED 必须与 MREMAP_MAYMOVE 一起使用
        if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE)) {
            printfRed("[mremap] EINVAL: MREMAP_FIXED requires MREMAP_MAYMOVE\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_DONTUNMAP 必须与 MREMAP_MAYMOVE 一起使用
        if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE)) {
            printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP requires MREMAP_MAYMOVE\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_FIXED 时需要提供新地址且必须页对齐
        if (flags & MREMAP_FIXED) {
            if (!new_address) {
                printfRed("[mremap] EINVAL: MREMAP_FIXED requires new_address\n");
                return syscall::SYS_EINVAL;
            }
            if ((uintptr_t)new_address & (PGSIZE - 1)) {
                printfRed("[mremap] EINVAL: new_address not page aligned: %p\n", new_address);
                return syscall::SYS_EINVAL;
            }
        }

        // EINVAL: 检查地址范围重叠（当指定了MREMAP_FIXED时）
        if (flags & MREMAP_FIXED) {
            uint64 old_start = (uint64)old_address;
            uint64 old_end = old_start + old_size;
            uint64 new_start = (uint64)new_address;
            uint64 new_end = new_start + new_size;
            
            if (!(new_end <= old_start || new_start >= old_end)) {
                printfRed("[mremap] EINVAL: new and old address ranges overlap\n");
                return syscall::SYS_EINVAL;
            }
        }

        // EINVAL: MREMAP_DONTUNMAP 要求 old_size == new_size
        if ((flags & MREMAP_DONTUNMAP) && (old_size != new_size)) {
            printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP requires old_size == new_size\n");
            return syscall::SYS_EINVAL;
        }

        proc::Pcb *pcb = get_cur_pcb();
        if (!pcb) {
            printfRed("[mremap] Internal error: No current process\n");
            return syscall::SYS_EFAULT;
        }

        uint64 old_start = (uint64)old_address;
        uint64 old_end = old_start + old_size;
        [[maybe_unused]]uint64 new_len = new_size;

        // EFAULT: 查找包含旧地址的VMA
        int vma_index = -1;
        printfYellow("[mremap] Searching for VMA containing range [%p, %p), size=%u\n", 
                     (void*)old_start, (void*)old_end, old_size);
        
        printfYellow("[mremap] NVMA=%d, pcb=%p, pcb->_vma=%p\n", NVMA, pcb, pcb->_vma);
        
        for (int i = 0; i < NVMA; i++) {
            printfYellow("[mremap] Checking VMA[%d]: used=%d\n", i, pcb->_vma->_vm[i].used);
            
            if (!pcb->_vma->_vm[i].used) continue;
            
            uint64 vma_start = pcb->_vma->_vm[i].addr;
            uint64 vma_end = vma_start + pcb->_vma->_vm[i].len;
            
            printfYellow("[mremap] VMA[%d]: [%p, %p), len=%d, used=%d\n", 
                         i, (void*)vma_start, (void*)vma_end, pcb->_vma->_vm[i].len, pcb->_vma->_vm[i].used);
            
            if (old_start >= vma_start && old_end <= vma_end) {
                vma_index = i;
                printfGreen("[mremap] Found matching VMA[%d]: [%p, %p)\n", i, (void*)vma_start, (void*)vma_end);
                break;
            }
        }

        // EFAULT: 地址范围未映射或无效
        if (vma_index == -1) {
            printfRed("[mremap] EFAULT: Address range [%p, %p) not found in valid mappings\n", 
                     (void*)old_start, (void*)old_end);
            return syscall::SYS_EFAULT;
        }

        proc::vma &vma = pcb->_vma->_vm[vma_index];
        printfCyan("[mremap] Found VMA[%d]: addr=%p, len=%d, prot=%d, flags=%d\n", 
                   vma_index, (void*)vma.addr, vma.len, vma.prot, vma.flags);

        // EINVAL: 检查MREMAP_DONTUNMAP的限制（只能用于私有匿名映射）
        if (flags & MREMAP_DONTUNMAP) {
            if (!(vma.flags & MAP_ANONYMOUS) || (vma.flags & MAP_SHARED)) {
                printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP can only be used with private anonymous mappings\n");
                return syscall::SYS_EINVAL;
            }
        }

        // 情况1：缩小映射
        if (new_size < old_size) {
            // 释放多余的页面
            uint64 pages_to_unmap = (old_size - new_size + PGSIZE - 1) / PGSIZE;
            uint64 unmap_start = old_start + new_size;
            
            mem::k_vmm.vmunmap(*pcb->get_pagetable(), unmap_start, pages_to_unmap, 1);
            
            // 更新VMA大小
            if (old_start == vma.addr && (int)old_size == vma.len) {
                // 整个VMA被调整
                vma.len = new_size;
            } else {
                // 部分调整，这里简化处理
                printfYellow("[mremap] Partial VMA resize not fully supported\n");
            }
            
            printfGreen("[mremap] Shrunk mapping from %u to %u bytes at %p\n", 
                       old_size, new_size, old_address);
            *result_addr = old_address;
            return 0;
        }

        // 情况2：扩大映射
        if (new_size > old_size) {
            uint64 additional_size = new_size - old_size;
            uint64 expand_start = old_start + old_size;

            // 检查是否可以就地扩展
            bool can_expand_in_place = true;
            if (!(flags & MREMAP_MAYMOVE)) {
                // 检查扩展区域是否可用
                for (int i = 0; i < NVMA; i++) {
                    if (i == vma_index || !pcb->_vma->_vm[i].used) continue;
                    
                    uint64 other_start = pcb->_vma->_vm[i].addr;
                    uint64 other_end = other_start + pcb->_vma->_vm[i].len;
                    
                    if (!(expand_start >= other_end || expand_start + additional_size <= other_start)) {
                        can_expand_in_place = false;
                        break;
                    }
                }

                // ENOMEM: 不能就地扩展且未指定MREMAP_MAYMOVE
                if (!can_expand_in_place) {
                    printfRed("[mremap] ENOMEM: Cannot expand in place and MREMAP_MAYMOVE not set\n");
                    return syscall::SYS_ENOMEM;
                }
            }

            // 如果可以就地扩展
            if (can_expand_in_place && !(flags & MREMAP_FIXED)) {
                // 分配新的页面
                uint64 prot_flags = 0;
                if (vma.prot & PROT_READ) prot_flags |= PTE_R;
                if (vma.prot & PROT_WRITE) prot_flags |= PTE_W;
                if (vma.prot & PROT_EXEC) prot_flags |= PTE_X;
                prot_flags |= PTE_U;

                uint64 result = mem::k_vmm.uvmalloc(*pcb->get_pagetable(), 
                                                   old_start + old_size, 
                                                   old_start + new_size, 
                                                   prot_flags);
                if (result != old_start + new_size) {
                    // ENOMEM: 内存分配失败
                    printfRed("[mremap] ENOMEM: Failed to allocate additional memory\n");
                    return syscall::SYS_ENOMEM;
                }

                // 更新VMA - 确保类型安全
                if (old_start == vma.addr) {
                    // 总是更新VMA长度，因为我们已经成功分配了内存
                    int old_vma_len = vma.len;
                    
                    // 检查new_size是否超出int范围 (2^31 - 1 = 2147483647)
                    if (new_size > 2147483647U) {
                        printfRed("[mremap] ERROR: new_size %u exceeds INT_MAX, cannot store in VMA.len\n", (uint)new_size);
                        return syscall::SYS_ENOMEM;
                    }
                    
                    vma.len = (int)new_size;
                    printfCyan("[mremap] Updated VMA[%d] length from %d to %d (old_size=%u)\n", 
                               vma_index, old_vma_len, vma.len, (uint)old_size);
                } else {
                    // 即使是部分VMA扩展，我们也需要更新VMA长度
                    int old_vma_len = vma.len;  // 确保在修改前保存
                    printfYellow("[mremap] DEBUG: Before update - VMA[%d].len=%d, new_size=%u\n", 
                                vma_index, old_vma_len, (uint)new_size);
                    
                    // 检查new_size是否超出int范围 (2^31 - 1 = 2147483647)
                    if (new_size > 2147483647U) {
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
            if (flags & MREMAP_MAYMOVE) {
                void *target_addr = new_address;
                
                if (!(flags & MREMAP_FIXED)) {
                    // 寻找合适的地址
                    target_addr = mmap(nullptr, new_size, vma.prot, vma.flags, vma.vfd, vma.offset);
                    if (target_addr == MAP_FAILED) {
                        // ENOMEM: 找不到合适的地址
                        printfRed("[mremap] ENOMEM: Failed to find suitable address for new mapping\n");
                        return syscall::SYS_ENOMEM;
                    }
                } else {
                    // 使用指定的地址
                    // 先取消映射目标区域（如果已映射）
                    munmap(target_addr, new_size);
                    
                    // 在指定地址创建新映射
                    void *mapped_addr = mmap(target_addr, new_size, vma.prot, 
                                           vma.flags | MAP_FIXED, vma.vfd, vma.offset);
                    if (mapped_addr != target_addr) {
                        // ENOMEM: 无法在指定地址映射
                        printfRed("[mremap] ENOMEM: Failed to map at fixed address %p\n", target_addr);
                        return syscall::SYS_ENOMEM;
                    }
                }

                // 复制旧数据到新位置
                // 创建临时缓冲区来中转数据
                void *temp_buffer = new char[old_size];
                if (!temp_buffer) {
                    // ENOMEM: 临时缓冲区分配失败
                    printfRed("[mremap] ENOMEM: Failed to allocate temporary buffer\n");
                    munmap(target_addr, new_size);
                    return syscall::SYS_ENOMEM;
                }

                // 从旧地址读取数据到临时缓冲区
                if (mem::k_vmm.copy_in(*pcb->get_pagetable(), temp_buffer, old_start, old_size) < 0) {
                    // EFAULT: 无法读取旧数据
                    printfRed("[mremap] EFAULT: Failed to read data from old location\n");
                    delete[] (char*)temp_buffer;
                    munmap(target_addr, new_size);
                    return syscall::SYS_EFAULT;
                }

                // 从临时缓冲区写入数据到新地址
                if (mem::k_vmm.copy_out(*pcb->get_pagetable(), (uint64)target_addr, temp_buffer, old_size) < 0) {
                    // EFAULT: 无法写入新数据
                    printfRed("[mremap] EFAULT: Failed to write data to new location\n");
                    delete[] (char*)temp_buffer;
                    munmap(target_addr, new_size);
                    return syscall::SYS_EFAULT;
                }

                delete[] (char*)temp_buffer;

                // 如果不是 MREMAP_DONTUNMAP，则释放旧映射
                if (!(flags & MREMAP_DONTUNMAP)) {
                    munmap(old_address, old_size);
                }

                printfGreen("[mremap] Moved and resized mapping from %p (%u bytes) to %p (%u bytes)\n", 
                           old_address, old_size, target_addr, new_size);
                *result_addr = target_addr;
                return 0;
            }
        }

        // 情况3：大小不变
        if (new_size == old_size) {
            if (flags & MREMAP_FIXED) {
                // 移动到新地址
                if (flags & MREMAP_MAYMOVE) {
                    // 类似上面的移动逻辑
                    munmap(new_address, new_size);
                    void *mapped_addr = mmap(new_address, new_size, vma.prot, 
                                           vma.flags | MAP_FIXED, vma.vfd, vma.offset);
                    if (mapped_addr != new_address) {
                        // ENOMEM: 无法在指定地址映射
                        return syscall::SYS_ENOMEM;
                    }
                    // 创建临时缓冲区用于数据转移
                    void *temp_buffer = new char[old_size];
                    if (!temp_buffer) {
                        munmap(new_address, new_size);
                        return syscall::SYS_ENOMEM;
                    }
                    
                    if (mem::k_vmm.copy_in(*pcb->get_pagetable(), temp_buffer, old_start, old_size) < 0) {
                        delete[] (char*)temp_buffer;
                        munmap(new_address, new_size);
                        return syscall::SYS_EFAULT;
                    }
                    
                    if (mem::k_vmm.copy_out(*pcb->get_pagetable(), (uint64)new_address, temp_buffer, old_size) < 0) {
                        delete[] (char*)temp_buffer;
                        munmap(new_address, new_size);
                        return syscall::SYS_EFAULT;
                    }
                    
                    delete[] (char*)temp_buffer;

                    if (!(flags & MREMAP_DONTUNMAP)) {
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

    /// @brief 从当前工作目录中删除指定路径的文件或目录项。
    /// @param fd 基准目录的文件描述符，若为 -100 表示以当前工作目录为基准（AT_FDCWD）。其他值暂不支持。
    /// @param path 要删除的文件或目录的相对路径，不能为空字符串，支持"./"开头的路径格式。
    /// @param flags 暂未使用的标志位参数，预留以支持 future 的 unlinkat 扩展。
    /// @return 成功返回 0，失败返回 -1。
    int ProcessManager::unlink(int fd, eastl::string path, int flags)
    {

        if (fd == -100)
        {                   // atcwd
            if (path == "") // empty path
                return -1;

            if (path[0] == '.' && path[1] == '/')
                path = path.substr(2);

            return fs::k_file_table.unlink(path);
        }
        else
        {
            return -1; // current not support other dir, only for cwd
        }
    }
    int ProcessManager::pipe(int *fd, int flags)
    {
        fs::pipe_file *rf, *wf;
        rf = nullptr;
        wf = nullptr;

        int fd0, fd1;
        Pcb *p = get_cur_pcb();

        ipc::Pipe *pipe_ = new ipc::Pipe();
        if (pipe_->alloc(rf, wf) < 0)
            return syscall::SYS_ENOMEM;
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

        uint64 old_sz = proc->_sz; // 保存原进程的内存大小
        uint64 sp;                 // 栈指针
        uint64 stackbase;          // 栈基地址
        mem::PageTable new_pt;     // 暂存页表, 防止加载过程中破坏原进程映像
        elf::elfhdr elf;           // ELF 文件头
        elf::proghdr ph = {};      // 程序头
        // fs::dentry *de;            // 目录项
        int i, off;     // 循环变量和偏移量
        u64 new_sz = 0; // 新进程映像的大小
#ifdef LOONGARCH
        u64 elf_start = 0; // ELF 文件的起始地址
#endif

        // 动态链接器相关
        elf::elfhdr interp_elf;
        uint64 interp_base = 0;
        // ========== 第一阶段：路径解析和文件查找 ==========

        // 构建绝对路径
        // TODO :这个解析路径写的太狗屎了，换以下
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
            return -1;
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

        // 创建新的页表，避免在加载过程中破坏原进程映像
        new_pt = k_pm.proc_pagetable(proc);
        TODO(if (new_pt == 0) {
            printfRed("execve: proc_pagetable failed\n");
            return -1;
        })

        // 这个地方不能按着学长的代码写, 因为学长的内存布局和我们的不同
        // 而且他们的proc_pagetable函数是弃用的, 我们的是好的, 直接用这个函数就可以构建基础页表

        // ========== 预第三阶段：初始化程序段记录 ==========
        // 程序段描述符，用于记录加载的程序段信息
        // using psd_t = program_section_desc;
        // int new_sec_cnt = 0;                         // 新程序段计数
        // psd_t new_sec_desc[max_program_section_num]; // 新程序段描述符数组

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
                            printfRed("execve: failed to find riscv64 dynamic linker\n");
                            return -1;
                        }
                        interpreter_path = "/glibc/lib/ld-linux-riscv64-lp64d.so.1";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-linux-loongarch64.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch64 dynamic linker\n");
                        if (vfs_is_file_exist("/glibc/lib/ld-linux-loongarch-lp64d.so.1") != 1)
                        {
                            printfRed("execve: failed to find loongarch64 dynamic linker\n");
                            return -1;
                        }
                        interpreter_path = "/glibc/lib/ld-linux-loongarch-lp64d.so.1";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib64/ld-musl-loongarch-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            printfRed("execve: failed to find loongarch musl linker\n");
                            return -1;
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64-sf.so.1") == 0)
                    {
                        printfBlue("execve: using riscv64 sf dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            printfRed("execve: failed to find riscv64 musl linker\n");
                            return -1;
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64.so.1") == 0)
                    {
                        // TODO: 这个可不是sf了, 那怎么办呢
                        printfBlue("execve: using riscv64 sf dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            printfRed("execve: failed to find riscv64 musl linker\n");
                            return -1;
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else
                    {
                        panic("execve: unknown dynamic linker: %s\n", interpreter_path.c_str());
                        return -1; // 不支持的动态链接器
                    }
                    break;
                }
            }
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
                    printfRed("execve: memsz < ph.filesz\n");
                    load_bad = true;
                    break;
                }
                if (ph.vaddr + ph.memsz < ph.vaddr) // 检查地址溢出
                {
                    printfRed("execve: vaddr + memsz < vaddr\n");
                    load_bad = true;
                    break;
                }
#ifdef LOONGARCH
                // printf("elf_start: %p, ph.vaddr: %p, ph.memsz: %p\n", (void *)elf_start, (void *)ph.vaddr, (void *)ph.memsz);
                if (elf_start == 0 || elf_start > ph.vaddr) // 记录第一个LOAD段的起始地址
                {
                    if (elf_start != 0)
                    {
                        // printf("eld_start: %p, ph.vaddr: %p\n", (void *)elf_start, (void *)ph.vaddr);
                        panic("execve: this LOAD segment is below the first LOAD segment, which is not allowed");
                    }
                    elf_start = ph.vaddr; // 记录第一个LOAD段的起始地址
                    new_sz = elf_start;
                    printfGreen("execve: start_vaddr set to %p\n", (void *)elf_start);
                }
#endif

                // 分配虚拟内存空间
                uint64 sz1;
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
#ifdef RISCV
                // printf("[exec] map from %p to %p new_pt base %p\n", (void *)(new_sz), (void *)(ph.vaddr + ph.memsz), new_pt.get_base());
                if ((sz1 = mem::k_vmm.vmalloc(new_pt, new_sz, ph.vaddr + ph.memsz, seg_flag)) == 0)
                {
                    printfRed("execve: uvmalloc\n");
                    load_bad = true;
                    break;
                }
                new_sz = sz1; // 更新新进程映像的大小
#elif defined(LOONGARCH)
                // printfRed("execve: loading segment %d, type: %d, vaddr: %p, memsz: %p, filesz: %p, flags: %d\n",
                //   i, ph.type, (void *)ph.vaddr, (void *)ph.memsz, (void *)ph.filesz, seg_flag);
                if ((sz1 = mem::k_vmm.vmalloc(new_pt, new_sz, ph.vaddr + ph.memsz, seg_flag)) == 0)
                {
                    printfRed("execve: uvmalloc\n");
                    load_bad = true;
                    break;
                }
                new_sz = sz1; // 更新新进程映像的大小
#endif

                // // 用于处理elf文件中给出的段起始地址没有对其到页面首地址的情况(弃用, 我们的load_seg函数已经处理了这个问题)
                // uint margin_size = 0;
                // if ((ph.vaddr % PGSIZE) != 0)
                // {
                //     margin_size = ph.vaddr % PGSIZE;
                // }

                // 从文件加载段内容到内存
                if (load_seg(new_pt, ph.vaddr, ab_path, ph.off, ph.filesz) < 0)
                {
                    printf("execve: load_icode\n");
                    load_bad = true;
                    break;
                }
            }
            // 如果加载过程中出错，清理已分配的资源
            if (load_bad)
            {
                // printfRed("execve: load segment failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }

            if (is_dynamic)
            {
                if (interpreter_path.length() == 0)
                {
                    printfRed("execve: cannot find dynamic linker: %s\n", interpreter_path.c_str());
                    k_pm.proc_freepagetable(new_pt, new_sz);
                    return -1;
                }

                // 读取动态链接器的ELF头
                vfs_read_file(interpreter_path.c_str(), reinterpret_cast<uint64>(&interp_elf), 0, sizeof(interp_elf));

                if (interp_elf.magic != elf::elfEnum::ELF_MAGIC)
                {
                    printfRed("execve: invalid dynamic linker ELF\n");
                    k_pm.proc_freepagetable(new_pt, new_sz);
                    return -1;
                }
                printfCyan("execve: dynamic linker ELF magic: %x\n", interp_elf.magic);
                // 选择动态链接器的加载基址（通常在高地址）
                interp_base = PGROUNDUP(new_sz); // 在新进程映像的末尾分配空间

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

                    uint64 sz1;
                    if ((sz1 = mem::k_vmm.uvmalloc(new_pt, PGROUNDUP(new_sz), load_addr + interp_ph.memsz, seg_flag)) == 0)
                    {
                        printfRed("execve: load dynamic linker failed\n");
                        k_pm.proc_freepagetable(new_pt, new_sz);
                        return -1;
                    }
                    new_sz = sz1;

                    // 加载动态链接器段内容
                    printfCyan("execve: loading dynamic linker segment %d, vaddr: %p, memsz: %p, offset: %p\n",
                               j, (void *)interp_ph.vaddr, (void *)interp_ph.memsz, (void *)interp_ph.off);
                    if (load_seg(new_pt, load_addr, interpreter_path, interp_ph.off, interp_ph.filesz) < 0)
                    {
                        printfRed("execve: load dynamic linker segment failed\n");
                        k_pm.proc_freepagetable(new_pt, new_sz);
                        return -1;
                    }
                }

                interp_entry = interp_base + interp_elf.entry;
                printfCyan("execve: dynamic linker loaded at base: %p, entry: %p\n",
                           (void *)interp_base, (void *)interp_entry);
            }
        }

        // ========== 第五阶段：分配用户栈空间 ==========

        { // 按照内存布局分配用户栈空间
            int stack_pgnum = 32;
            new_sz = PGROUNDUP(new_sz); // 将大小对齐到页边界
            uint64 sz1;
#ifdef RISCV
            if ((sz1 = mem::k_vmm.uvmalloc(new_pt, new_sz, new_sz + stack_pgnum * PGSIZE, PTE_W | PTE_X | PTE_R | PTE_U)) == 0)
            {
                printfRed("execve: load user stack failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
#elif defined(LOONGARCH)
            if ((sz1 = mem::k_vmm.uvmalloc(new_pt, new_sz, new_sz + stack_pgnum * PGSIZE, PTE_P | PTE_W | PTE_PLV | PTE_MAT | PTE_D)) == 0)
            {
                printfRed("execve: load user stack failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
#endif

            new_sz = sz1;                                                     // 更新新进程映像的大小
            mem::k_vmm.uvmclear(new_pt, new_sz - (stack_pgnum - 1) * PGSIZE); // 设置guardpage
            sp = new_sz;                                                      // 栈指针从顶部开始
            stackbase = sp - (stack_pgnum - 1) * PGSIZE;                      // 计算栈底地址
            sp -= sizeof(uint64);                                             // 为返回地址预留空间
        }

        // ========== 第六阶段：准备glibc所需的用户栈数据 ==========
        // 为了兼容glibc，需要在用户栈中按照特定顺序压入：
        // 栈顶 -> 栈底：argc, argv[], envp[], auxv[], 字符串数据, 随机数据

        sp -= 32;
        uint64_t random[4] = {0x0, -0x114514FF114514UL, 0x2UL << 60, 0x3UL << 60};
        if (sp < stackbase || mem::k_vmm.copy_out(new_pt, sp, (char *)random, 32) < 0)
        {
            printfRed("execve: copy random data failed\n");
            k_pm.proc_freepagetable(new_pt, new_sz);
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
                printfRed("execve: too many envs\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            sp -= envs[envc].size() + 1; // 为环境变量字符串预留空间(包括null)
            sp -= sp % 16;               // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                printfRed("execve: stack overflow\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, envs[envc].c_str(), envs[envc].size() + 1) < 0)
            {
                printfRed("execve: copy envs failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
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
                printfRed("execve: too many args\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            sp -= argv[argc].size() + 1; // 为参数字符串预留空间(包括null)
            sp -= sp % 16;               // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                printfRed("execve: stack overflow\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, argv[argc].c_str(), argv[argc].size() + 1) < 0)
            {
                printfRed("execve: copy args failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            uargv[argc] = sp; // 记录字符串地址

            // printfRed("[execve] argv[%d] = \"%s\", user_stack_addr = 0x%p\n", argc, argv[argc].c_str(), sp);
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
                printfRed("execve: copy auxv failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
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
                printfRed("execve: stack overflow\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, uenvp, (envc + 1) * sizeof(uint64)) < 0)
            {
                printfRed("execve: copy envp failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
        }
        proc->_trapframe->a2 = sp; // 设置栈指针到trapframe

        // 6. 压入命令行参数指针数组（argv）
        // if (uargv[0])
        {
            sp -= (argc + 1) * sizeof(uint64); // 为argv数组预留空间
            // sp -= sp % 16;                     // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                printfRed("execve: stack overflow\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, uargv, (argc + 1) * sizeof(uint64)) < 0)
            {
                printfRed("execve: copy argv failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
            // // 新增：打印压入的 argv 指针及其内容
            // for (uint64 i = 0; i <= argc; ++i)
            // {
            //     printf("[execve] argv_ptr[%d] = 0x%p -> \"%s\"\n", i, uargv[i], argv[i].c_str());
            // }
        }

        proc->_trapframe->a1 = sp; // 设置argv指针到trapframe

        // 7. 压入参数个数（argc）
        sp -= sizeof(uint64);
        // printfGreen("execve: argc: %d, sp: %p\n", argc, (void *)sp);
        if (mem::k_vmm.copy_out(new_pt, sp, (char *)&argc, sizeof(uint64)) < 0)
        {
            printfRed("execve: copy argc failed\n");
            k_pm.proc_freepagetable(new_pt, new_sz);
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

        // 使用safestrcpy将文件名安全地拷贝到进程的_name成员变量中
        safestrcpy(proc->_name, filename.c_str(), sizeof(proc->_name));

        // printfGreen("execve: process name set to '%s'\n", proc->_name);

        // ========== 第七阶段：配置进程资源限制 ==========
        // 设置栈大小限制
        proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_cur =
            proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_max = sp - stackbase;
        // 处理F_DUPFD_CLOEXEC标志位，关闭设置了该标志的文件描述符
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
        mem::PageTable old_pt;
        old_pt = *proc->get_pagetable(); // 获取当前进程的页表
        proc->_sz = PGROUNDUP(new_sz);   // 更新进程大小
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
        proc->_trapframe->epc = entry_point;
#elif defined(LOONGARCH)
        proc->_trapframe->era = entry_point;
        proc->elf_base = elf_start; // 保存ELF文件的起始地址
#endif
        proc->_pt = new_pt;        // 替换为新的页表
        proc->_trapframe->sp = sp; // 设置栈指针

        // printf("execve: new process size: %p, new pagetable: %p\n", proc->_sz, proc->_pt);
        k_pm.proc_freepagetable(old_pt, old_sz);

        printf("execve succeed, new process size: %p\n", proc->_sz);

        // 写成0为了适配glibc的rtld_fini需求
        return 0; // 返回参数个数，表示成功执行
    }
}; // namespace proc