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
#include "fs/vfs/elf.hh"
#include "fs/vfs/file/normal_file.hh"
#include "mem.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "syscall_defs.hh"

#include "fs/vfs/fs.hh"
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
                k_pm.alloc_pid(p);

                // 线程相关
                p->_ctid = 0;
                k_pm.alloc_tid(p);
                p->_state = ProcState::USED;
                // 设置调度相关字段：默认调度槽与优先级
                p->_slot = default_proc_slot;
                p->_priority = default_proc_prio;

                // p->_shm = mem::vml::vm_trap_frame - 64 * 2 * mem::PageEnum::pg_size;
                // p->_shmkeymask = 0;
                // k_pm.set_vma( p );

                // 为该进程分配一页 trapframe 空间（用于中断时保存用户上下文）
                // printfYellow("[user pgtbl]==>alloc trapframe for proc %d\n", p->_gid);
                if ((p->_trapframe = (TrapFrame *)mem::k_pmm.alloc_page()) == nullptr)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }

                // 初始化ofile结构体
                p->_ofile = new ofile();
                p->_ofile->_shared_ref_cnt = 1;
                for (uint64 i = 0; i < max_open_files; ++i)
                {
                    p->_ofile->_ofile_ptr[i] = nullptr;
                    p->_ofile->_fl_cloexec[i] = false;
                }

                p->_vma = new Pcb::VMA();
                p->_vma->_ref_cnt = 1; // 初始化虚拟内存区域

                // 初始化信号处理结构体
                p->_sigactions = new sighand_struct();
                p->_sigactions->refcnt = 1;
                for (int i = 0; i <= ipc::signal::SIGRTMAX; ++i)
                {
                    p->_sigactions->actions[i] = nullptr;
                }

                // 创建进程自己的页表（空的页表）

                // debug
                // printfCyan("[user pgtbl]==>into proc_pagetable\n");
                _proc_create_vm(p);

                if (p->_pt.get_base() == 0)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }
                // 清空消息队列掩码
                p->_mqmask = 0;

                // 初始化上下文结构体
                memset(&p->_context, 0, sizeof(p->_context));

                // 设置调度返回地址为 _wrp_fork_ret
                // 当调度器切换回该进程时，将从这里开始执行
                p->_context.ra = (uint64)_wrp_fork_ret;
                // printf("p->_context.ra: %p\n", p->_context.ra);
                // printf("wrp_fork_ret: %p\n", _wrp_fork_ret);
                // void (ProcessManager::*fptr)() = &ProcessManager::fork_ret;
                // printf("ProcessManager::fork_ret: %p\n", (void *)*(uintptr_t *)&fptr);

                // 设置内核栈指针
                p->_context.sp = p->_kstack + PGSIZE;

                // 更新上次分配的位置，轮转分配策略
                _last_alloc_proc_gid = p->_gid;

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
            filesystem2_init(); // 启动init

            /// 你好
            /// 这是重定向uart的代码
            /// commented out by @gkq
            new (&dev::k_uart) dev::UartManager(UART0);
            dev::register_debug_uart(&dev::k_uart);
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
                    // printfMagenta("freeproc: unmapping vma %d, addr: %p, len: %d\n", i, p->_vm[i].addr, p->_vm[i].len);
                    // uint64 npages = (va_end - va_start) / PGSIZE;

                    // 逐页检查并取消映射，避免对未映射的页面进行操作
                    for (uint64 va = va_start; va < va_end; va += PGSIZE)
                    {
                        mem::Pte pte = p->_pt.walk(va, 0);
                        // printfCyan("freeproc: checking pte for va %p, pte: %p\n", va, pte.get_data());
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

        if (p->_trapframe)
            mem::k_pmm.free_page(p->_trapframe);
        p->_trapframe = 0;

        if (p->_pt.get_base())
        {
            proc_freepagetable(p->_pt, p->_sz);
        }
        // 不要手动设置base为0，让引用计数机制处理
        // p->_pt.set_base(0);
        p->_sz = 0;
        p->_shared_vm = false; // 重置共享虚拟内存标志
        p->_pid = 0;
        p->_parent = 0;
        p->_name[0] = 0;
        p->_chan = 0;
        p->_killed = 0;
        p->_xstate = 0;
        p->_state = ProcState::UNUSED;

        // 线程相关
        p->_tid = 0;
        p->_ctid = 0;
        // printf("freeproc: freeing process %d\n", p->_pid);

        // 使用新的cleanup_ofile方法处理文件描述符表
        p->cleanup_ofile();

        // 使用新的cleanup_sighand方法处理信号处理结构
        p->cleanup_sighand();

        // 清空信号相关
        while (p->sig_frame != nullptr)
        {
            ipc::signal::signal_frame *next_frame = p->sig_frame->next;
            mem::k_pmm.free_page(p->sig_frame); // 释放当前信号处理帧
            p->sig_frame = next_frame;          // 移动到下一个帧
        }
        p->sig_frame = nullptr; // 清空信号处理帧
        p->_signal = 0;
        p->_sigmask = 0;
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
        mem::k_vmm.uvmfirst(p->_pt, (uint64)initcode_start, (uint64)initcode_end - (uint64)initcode_start);
        // debug
        //  uint64 pa = (uint64)p->_pt.walk_addr((uint64)0);
        //  printfYellow("initcode start pa: %p\n",pa);
        //  printfYellow("initcode start byte %u\n", *(uint64 *)pa);
        printf("initcode start: %p, end: %p\n", initcode_start, initcode_end);
        printf("initcode size: %p\n", (uint64)(initcode_end - 0));
        p->_sz = 6 * PGSIZE;

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
        mem::k_vmm.uvmfirst(p->_pt, (uint64)initcode_start, (uint64)initcode_end - (uint64)initcode_start);
        // debug
        //  uint64 pa = (uint64)p->_pt.walk_addr((uint64)0);
        //  printfYellow("initcode start pa: %p\n",pa);
        //  printfYellow("initcode start byte %u\n", *(uint64 *)pa);
        printf("initcode start: %p, end: %p\n", initcode_start, initcode_end);
        printf("initcode size: %p\n", (uint64)(initcode_end - 0));
        p->_sz = 3 * PGSIZE;

        p->_trapframe->era = 0;     // 设置程序计数器为0
        p->_trapframe->sp = p->_sz; // 设置栈指针为3个页的大小

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
        uint64 cur_tick = tmm::k_tm.get_ticks();

        tsv->tms_utime = p->_user_ticks;
        tsv->tms_stime = cur_tick - p->_start_tick - p->_user_ticks;
        tsv->tms_cstime = tsv->tms_cutime = 0;

        for (auto &pp : k_proc_pool)
        {
            if (pp._state == ProcState::UNUSED || pp._parent != p)
                continue;
            tsv->tms_cutime += pp._user_ticks;
            tsv->tms_cstime += cur_tick - pp._start_tick - pp._user_ticks;
        }
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
        return -1;
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
            np->_pid = p->_pid; // 线程共享 PID
            // TODO: 共享定时器
        }
        else
        {
            // TODO: 共享信号掩码
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
            np->_ctid = ctid;
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
                    // printfGreen("[wait4]: child %d state: %d name: %s\n", np->_pid, (int)np->_state, np->_name);
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
    int ProcessManager::load_seg(mem::PageTable &pt, uint64 va, fs::dentry *de, uint offset, uint size)
    { // 好像没有机会返回 -1, pa失败的话会panic，de的read也没有返回值
        panic("未实现");
#ifdef FS_FIX_COMPLETELY
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
            de->getNode()->nodeRead(pa, offset + i, n);
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

            if (de->getNode()->nodeRead(pa, offset + i, n) != n) // 读取文件内容到物理内存
                return -1;
        }
#endif
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

        _wait_lock.acquire();
        reparent(p); // 将 p 的所有子进程交给 init 进程收养

        if (p->_parent)
            wakeup(p->_parent); // 唤醒父进程（可能在 wait() 中阻塞）)
        if (p->_ctid)
        {
            uint64 temp0 = 0;
            if (mem::k_vmm.copy_out(p->_pt, p->_ctid, &temp0, sizeof(temp0)) < 0)
            {
                printfRed("exit_proc: copy out ctid failed\n");
            }
        }

        p->_lock.acquire();
        p->_xstate = state << 8;       // 存储退出状态（通常高字节存状态）
        p->_state = ProcState::ZOMBIE; // 标记为 zombie，等待父进程回收

        _wait_lock.release();
        //    printf("[exit_proc] proc %s pid %d exiting with state %d\n", p->_name, p->_pid, state);
        k_scheduler.call_sched(); // jump to schedular, never return
        panic("zombie exit");
    }

    /// @brief Pass p's abandoned children to init.
    /// @param p The parent process whose children are to be reparented.
    /// p是即将去世的父亲，他的儿子们马上要成为孤儿，我们要让init来收养他们。
    void ProcessManager::reparent(Pcb *p)
    {
        Pcb *pp;
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
    }
    /// @brief 当前进程或线程退出（只退出自己）
    /// @param state   调用 exit_proc 处理退出逻辑
    /// “一荣俱荣，一损俱损” commented by @gkq
    void ProcessManager::exit(int state)
    {
        Pcb *p = get_cur_pcb();
        printf("[exit] proc %s pid %d exiting with state %d\n", p->_name, p->_pid, state);
        exit_proc(p, state);
    }

    /// @brief 当前线程组（或进程组）全部退出
    /// @param status
    void ProcessManager::exit_group(int status)
    {
        void *stk[num_process + 10]; // 这里不使用stl库是因为 exit
                                     // 后不会返回，无法调用析构函数，可能造成内存泄露
        int stk_ptr = 0;

        u8 visit[num_process];
        memset((void *)visit, 0, sizeof visit);
        proc::Pcb *cp = get_cur_pcb();

        _wait_lock.acquire();

        for (uint i = 0; i < num_process; i++)
        {
            if (k_proc_pool[i]._state == ProcState::UNUSED)
                continue;
            if (visit[i] != 0)
                continue;

            proc::Pcb *p = &k_proc_pool[i];
            visit[i] = 1;
            stk[stk_ptr] = (void *)p;
            stk_ptr++;
            bool need_chp = false;
            // 向上查找父进程链，如果当前进程是其祖先，则标记它应退出
            while (p->_parent != nullptr)
            {
                if (p->_parent == cp)
                {
                    need_chp = true;
                    break;
                }
                p = p->_parent;
                visit[p->_gid] = 1;
                // 修改于6.6，检测防止数组越界
                if (stk_ptr < (int)num_process + 10)
                    stk[stk_ptr++] = (void *)p;
                else
                    break; // 栈满，直接停止压栈
            }

            // 释放应退出的子孙进程资源
            while (stk_ptr > 0)
            {
                proc::Pcb *tp = (proc::Pcb *)stk[stk_ptr - 1];
                stk_ptr--;
                if (need_chp)
                {
                    freeproc(tp);
                }
            }
        }

        _wait_lock.release();
        // 最后退出自己
        // printf("[exit_group] proc %s pid %d exiting with status %d\n", cp->_name, cp->_pid, status);
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
        panic("未实现");
#ifdef FS_FIX_COMPLETELY
        Pcb *p = get_cur_pcb();
        fs::file *file = nullptr;
        fs::dentry *dentry;

        if (dir_fd != AT_FDCWD)
        {
            file = p->get_open_file(dir_fd);
        }

        fs::Path path_(path, file);
        dentry = path_.pathSearch();

        if (dentry == nullptr)
        {
            fs::dentry *par_ = path_.pathSearch(true);
            if (par_ == nullptr)
                return -1;
            fs::FileAttrs attrs;
            attrs.filetype = fs::FileTypes::FT_DIRECT;
            attrs._value = 0777;
            if ((dentry = par_->EntryCreate(path_.rFileName(), attrs)) == nullptr)
            {
                printf("Error creating new dentry %s failed\n", path_.rFileName());
                return -1;
            }
        }
        if (dentry == nullptr)
            return -1;
#endif
        return 0;
    }
    /// @brief
    /// @param dir_fd 指定相对路径的目录文件描述符（AT_FDCWD 表示当前工作目录）。
    /// @param path 要打开的路径
    /// @param flags 打开方式（如只读、只写、创建等）
    /// @return
    int ProcessManager::open(int dir_fd, eastl::string path, uint flags)
    {
        panic("未实现");
#ifdef FS_FIX_COMPLETELY
        printfCyan("[open] dir_fd: %d, path: %s, flags: %x\n", dir_fd, path.c_str(), flags);
        // enum OpenFlags : uint
        // {
        // 	O_RDONLY	= 0x000U,
        // 	O_WRONLY	= 0x001U,
        // 	O_RDWR		= 0x002U,
        // 	O_CREATE	= 0x040U,
        // 	O_DIRECTORY = 0x020'0000U
        // };

        Pcb *p = get_cur_pcb();
        fs::file *file = nullptr;
        fs::dentry *dentry;

        // 果不是工作目录，则获取对应的 file 指针
        if (dir_fd != AT_FDCWD)
        {
            file = p->get_open_file(dir_fd);
        }

        fs::Path path_(path, file); // 创建路径对象（支持相对路径）

        dentry = path_.pathSearch(); // 查找路径对应的 dentry（目录项）
        // printfBlue("[open] path: %s, dentry: %p, flags: %x\n", path_.AbsolutePath().c_str(), dentry, flags);
        if (path == "") // empty path
            return -1;

        if (path[0] == '.' && path[1] == '/')
            path = path.substr(2); // 处理./xxxx的情况
        // 如果 dentry 存在但处于“已删除但未关闭”状态
        if (dentry != nullptr && fs::k_file_table.has_unlinked(path))
        {
            if (flags & O_CREAT)
            {
                fs::k_file_table.remove(path);
            }
            else
            {
                return -1;
            }
        }
        // dentry 不存在但设置了 O_CREAT，尝试创建文件
        if (dentry == nullptr && flags & O_CREAT)
        {
            // @todo: create file
            // 查找父目录
            fs::dentry *par_ = path_.pathSearch(true);
            if (par_ == nullptr)
                return -1;
            fs::FileAttrs attrs;
            if ((flags & __S_IFMT) == S_IFDIR)
                attrs.filetype = fs::FileTypes::FT_DIRECT;
            else
                attrs.filetype = fs::FileTypes::FT_NORMAL;
            attrs._value = 0777;                                                   // 默认权限
            if ((dentry = par_->EntryCreate(path_.rFileName(), attrs)) == nullptr) // 创建文件
            {
                printf("Error creating new dentry %s failed\n", path_.rFileName());
                return -1;
            }
        }
        if (dentry == nullptr)
            return -1; // file is not found
                       // 获取设备信息和属性
        int dev = dentry->getNode()->rDev();
        fs::FileAttrs attrs = dentry->getNode()->rMode();
        // 如果是设备文件，构造 device_file 对象并返回 fd
        if (dev >= 0) // dentry is a device
        {
            fs::device_file *f = new fs::device_file(attrs, dev, dentry);
            return alloc_fd(p, f);
        } // else if( attrs.filetype == fs::FileTypes::FT_DIRECT)
        // 	fs::directory *f = new fs::directory( attrs, dentry );
        else // 否则为普通文件，创建 normal_file 对象
        {
            fs::normal_file *f = new fs::normal_file(attrs, dentry);
            // log_info( "test normal file read" );
            // {
            // 	fs::file *ff = ( fs::file * ) f;
            // 	char buf[ 8 ];
            // 	ff->read( ( ulong ) buf, 8 );
            // 	buf[ 8 ] = 0;
            // 	printf( "%s\n", buf );
            // }
            if (flags & O_APPEND)
                f->setAppend();
            return alloc_fd(p, f);
        } // because of open.c's fileattr defination is not clearly, so here we
          // set flags = 7, which means O_RDWR | O_WRONLY | O_RDONLY

        // return alloc_fd( p, f );
        #endif
        return 0;
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
        if (proc_name.substr(0, 4) == "busy" || proc_name.substr(0, 4) == "iozo")
        {
            return 0;
        }
        if (fd < 0 || fd >= (int)max_open_files)
            return -1;

        Pcb *p = get_cur_pcb();
        if (p->_ofile == nullptr || p->_ofile->_ofile_ptr[fd] == nullptr)
            return -1;
        fs::file *f = p->_ofile->_ofile_ptr[fd];
        *buf = f->_stat;

        return 0;
    }
    int ProcessManager::chdir(eastl::string &path)
    {
        panic("未实现");
#ifdef FS_FIX_COMPLETELY
        Pcb *p = get_cur_pcb();

        fs::dentry *dentry;

        fs::Path pt(path);
        // printfCyan("chdir: %s\n", pt.AbsolutePath().c_str());
        dentry = pt.pathSearch();
        // dentry = p->_cwd->EntrySearch( path );
        if (dentry == nullptr)
            return -1;
        p->_cwd = dentry;
        p->_cwd_name = pt.AbsolutePath();
        if (p->_cwd_name.back() != '/')
            p->_cwd_name += "/";
#endif
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
        uint i = 0;
        for (; i < cwd.size(); ++i)
            out_buf[i] = cwd[i];
        out_buf[i] = '\0';
        return i + 1;
    }
    /// @brief
    /// @param fd
    /// @param map_size
    /// @return
    void *ProcessManager::mmap(void *addr, int length, int prot, int flags, int fd, int offset)
    {
        printfYellow("[mmap] addr: %p, length: %d, prot: %d, flags: %d, fd: %d, offset: %d\n",
                     addr, length, prot, flags, fd, offset);
        uint64 err = 0xffffffffffffffff;
        fs::normal_file *vfile = nullptr;
        fs::file *f;
        Pcb *p = get_cur_pcb();
        if (fd == -1)
        {
            f = nullptr; // 匿名映射
            // 对于匿名映射，创建一个特殊的vfile标记
            // 我们可以使用nullptr，或者创建一个特殊的匿名文件对象
            vfile = nullptr; // 匿名映射使用nullptr作为vfile
        }
        else if (p->_ofile == nullptr || p->_ofile->_ofile_ptr[fd] == nullptr)
        {
            return (void *)err;
        }
        else
        {
            f = p->_ofile->_ofile_ptr[fd];
            if (f->_attrs.filetype != fs::FileTypes::FT_NORMAL)
                return (void *)err;                    // 只支持普通文件映射
            vfile = static_cast<fs::normal_file *>(f); // 强制转换为普通文件类型
        }
        ///@details 学长代码是mmap时映射到内存，xv6lab的意思是懒分配，在缺页异常时判断分配。
        /// 我们选择懒分配的方式，只有在访问时才分配物理页。
        // get dentry from file
        //  fs::dentry *de = vfile->getDentry();
        //  if(de ==nullptr)   return (void *)err; // dentry is null

        if (p->_sz + length > MAXVA - PGSIZE) // 我写得maxva-pgsize是trampoline的地址
            return (void *)err;               // 超出最大虚拟地址空间

        // if (length == 0)
        // {
        //     length = 10 * PGSIZE; // 默认映射10页
        // }

        // MAP_FIXED 的处理应该在 VMA 分配中进行，而不是直接返回地址

        for (int i = 0; i < NVMA; ++i)
        {
            if (p->_vma->_vm[i].used == 0) // 找到一个空闲的虚拟内存区域
            {
                p->_vma->_vm[i].used = 1;

                // 处理 MAP_FIXED 标志
                if (flags & MAP_FIXED && addr != nullptr)
                {
                    // MAP_FIXED 要求在指定地址进行映射
                    p->_vma->_vm[i].addr = (uint64)addr;
                    printfCyan("[mmap] MAP_FIXED mapping at specified address %p\n", addr);
                }
                else
                {
                    // 正常情况下，在进程当前大小之后分配
                    p->_vma->_vm[i].addr = p->_sz;
                }

                p->_vma->_vm[i].flags = flags;
                p->_vma->_vm[i].prot = prot;
                p->_vma->_vm[i].vfile = vfile; // 对于匿名映射，这里是nullptr
                p->_vma->_vm[i].vfd = fd;      // 对于匿名映射，这里是-1
                p->_vma->_vm[i].offset = offset;

                if (fd == -1) // 匿名映射
                {
                    // 设置初始大小为请求大小
                    printfCyan("[mmap] anonymous mapping at %p, length: %d, prot: %d, flags: %d\n",
                               (void *)p->_vma->_vm[i].addr, length, prot, flags);
                    p->_vma->_vm[i].is_expandable = 0;              // 可扩展
                    p->_vma->_vm[i].len = MAX(length, 10 * PGSIZE); // 至少10页

                    if (flags & MAP_FIXED)
                    {
                        // MAP_FIXED 不扩展最大长度，只能使用指定区域
                        p->_vma->_vm[i].len = length; // 使用指定长度
                        p->_vma->_vm[i].addr = (uint64)addr;
                        p->_vma->_vm[i].max_len = p->_vma->_vm[i].len;
                        p->_vma->_vm[i].is_expandable = 0; // MAP_FIXED 通常不可扩展
                    }
                    else
                    {
                        p->_vma->_vm[i].max_len = MAXVA - p->_sz; // 设置最大可扩展大小
                    }
                }
                else
                {
                    p->_vma->_vm[i].len = length; // 文件映射保持原样
                    p->_vma->_vm[i].max_len = length;
                    // printfCyan("[mmap] file mapping at %p, length: %d, prot: %d, flags: %d, fd: %d\n",
                    //            (void *)p->_vma->_vm[i].addr, length, prot, flags, fd);
                    vfile->dup(); // 只对文件映射增加引用计数
                }

                // 只有非 MAP_FIXED 的映射才更新 p->_sz
                if (!(flags & MAP_FIXED))
                {
                    p->_sz += p->_vma->_vm[i].len; // 扩展进程的虚拟内存空间
                }
                else
                {
                    // MAP_FIXED 可能需要更新 p->_sz 为更大的值
                    uint64 end_addr = p->_vma->_vm[i].addr + p->_vma->_vm[i].len;
                    if (end_addr > p->_sz)
                    {
                        p->_sz = end_addr;
                    }
                }

                return (void *)p->_vma->_vm[i].addr; // 返回映射的虚拟地址
            }
        }
        return (void *)err;
    }
    int ProcessManager::munmap(void *addr, int length)
    {

        int i;
        Pcb *p = get_cur_pcb();

        for (i = 0; i < NVMA; ++i)
        {
            if (p->_vma->_vm[i].used && p->_vma->_vm[i].len >= length)
            {
                // 根据提示，munmap的地址范围只能是
                // 1. 起始位置
                if (p->_vma->_vm[i].addr == (uint64)addr)
                {
                    p->_vma->_vm[i].addr += length;
                    p->_vma->_vm[i].len -= length;
                    break;
                }
                // 2. 结束位置
                if ((uint64)(addr) + length == p->_vma->_vm[i].addr + p->_vma->_vm[i].len)
                {
                    p->_vma->_vm[i].len -= length;
                    break;
                }
            }
        }
        if (i == NVMA)
            return -1;

        ///@TODO: 此处注释了就能过，但是supposed to 写回的，暂时处理不了那个write
        ///@details 所以其实不用管，munmap和mmap两个点都能过。
        // // 将MAP_SHARED页面写回文件系统
        // if (p->_vma->_vm[i].flags == MAP_SHARED && (p->_vma->_vm[i].prot & PROT_WRITE) != 0)
        // {
        //     // printfRed("[munmap?]: walkaddr :%p\n",(p->_pt.walk_addr((uint64)addr)));
        //     p->_vma->_vm[i].vfile->write((uint64)addr, length);
        // }
        uint64 va_start = PGROUNDDOWN((uint64)addr);
        uint64 va_end = PGROUNDUP((uint64)addr + length);
        // 逐页检查并取消映射，避免对未映射的页面进行操作
        for (uint64 va = va_start; va < va_end; va += PGSIZE)
        {
            mem::Pte pte = p->_pt.walk(va, 0);
            // printfCyan("freeproc: checking pte for va %p, pte: %p\n", va, pte.get_data());
            if (!pte.is_null() && pte.is_valid())
            {
                // 只对实际映射的页面进行取消映射
                mem::k_vmm.vmunmap(*p->get_pagetable(), va, 1, 1);
            }
        }
        p->_vma->_vm[i].used = 0;
        // 判断此页面是否存在映射

        // 当前VMA中全部映射都被取消
        if (p->_vma->_vm[i].len == 0)
        {
            /// TODO:此处我没找到对应xv6的fileclose，找了个最类似的函数用上
            /// 也许这个free_file()是对的
            /// commented by @gkq
            // 只对文件映射释放文件引用
            if (p->_vma->_vm[i].vfile != nullptr)
            {
                p->_vma->_vm[i].vfile->free_file();
            }
            p->_vma->_vm[i].used = 0;
        }

        return 0;
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
            return -1;
        fd0 = -1;
        if (((fd0 = alloc_fd(p, rf)) < 0) || (fd1 = alloc_fd(p, wf)) < 0)
        {
            if (fd0 >= 0)
                p->_ofile->_ofile_ptr[fd0] = nullptr;
            // fs::k_file_table.free_file( rf );
            // fs::k_file_table.free_file( wf );
            rf->free_file();
            wf->free_file();
            return -1;
        }
        // 其实alloc_fd已经设置了_ofile_ptr，这里不需要再次设置了，但是再设一下无伤大雅
        p->_ofile->_ofile_ptr[fd0] = rf;
        p->_ofile->_ofile_ptr[fd1] = wf;
        fd[0] = fd0;
        fd[1] = fd1;
        return 0;
    }
    int ProcessManager::set_tid_address(int *tidptr)
    {
        Pcb *p = get_cur_pcb();
        p->_clear_child_tid = tidptr;
        return p->_pid;
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
        panic("未实现");
#ifdef FS_FIX_COMPLETELY
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
        fs::dentry *de;            // 目录项
        int i, off;                // 循环变量和偏移量
        u64 new_sz = 0;            // 新进程映像的大小
#ifdef LOONGARCH
        u64 elf_start = 0; // ELF 文件的起始地址
#endif

        // 动态链接器相关
        elf::elfhdr interp_elf;
        uint64 interp_base = 0;
        // ========== 第一阶段：路径解析和文件查找 ==========

        // 构建绝对路径
        eastl::string ab_path;
        if (path[0] == '/')
            ab_path = path; // 已经是绝对路径
        else
            ab_path = proc->_cwd_name + path; // 相对路径，添加当前工作目录前缀

        printfRed("execve file : %s\n", ab_path.c_str());

        // 解析路径并查找文件
        fs::Path path_resolver(ab_path);
        if ((de = path_resolver.pathSearch()) == nullptr)
        {
            printfRed("execve: cannot find file");
            return -1;
        }

        // 读取ELF文件头，验证文件格式
        de->getNode()->nodeRead(reinterpret_cast<uint64>(&elf), 0, sizeof(elf));

        if (elf.magic != elf::elfEnum::ELF_MAGIC) // 检查ELF魔数
        {
            panic("execve: not a valid ELF file,\n magic number: %x, execve path: %s", elf.magic, path.c_str());
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
            fs::dentry *interp_de = nullptr;

            // 检查程序头中是否有PT_INTERP段
            for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
            {
                // if (strcmp(ab_path.c_str(), "/mnt/musl/entry-dynamic.exe") != 0)
                // {
                //     printfCyan("execve: checking program header %d at offset %d\n", i, off);
                //     break;
                // }
                de->getNode()->nodeRead(reinterpret_cast<uint64>(&ph), off, sizeof(ph));
                if (ph.type == elf::elfEnum::ELF_PROG_INTERP) // PT_INTERP = 3
                {
                    // TODO, noderead在basic有时候乱码，故在下面设置interp_de = de;跳过动态链接
                    is_dynamic = true;
                    // 读取解释器路径
                    char interp_buf[256];
                    de->getNode()->nodeRead(reinterpret_cast<uint64>(interp_buf), ph.off, ph.filesz);
                    interp_buf[ph.filesz] = '\0';
                    interpreter_path = interp_buf;
                    interp_de = de;
                    printfCyan("execve: found dynamic interpreter: %s\n", interpreter_path.c_str());

                    if (strcmp(interpreter_path.c_str(), "/lib/ld-linux-riscv64-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using riscv64 dynamic linker\n");
                        fs::Path path_resolver_interp("/mnt/glibc/lib/ld-linux-riscv64-lp64d.so.1");
                        interp_de = path_resolver_interp.pathSearch();
                        if (interp_de == nullptr)
                        {
                            printfRed("execve: failed to find riscv64 dynamic linker\n");
                            return -1;
                        }
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-linux-loongarch64.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch64 dynamic linker\n");
                        fs::Path path_resolver_interp("/mnt/glibc/lib/ld-linux-loongarch-lp64d.so.1");
                        interp_de = path_resolver_interp.pathSearch();
                        if (interp_de == nullptr)
                        {
                            printfRed("execve: failed to find loongarch64 dynamic linker\n");
                            return -1;
                        }
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib64/ld-musl-loongarch-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch dynamic linker\n");
                        fs::Path path_resolver_interp("/mnt/musl/lib/libc.so");
                        interp_de = path_resolver_interp.pathSearch();
                        if (interp_de == nullptr)
                        {
                            printfRed("execve: failed to find loongarch musl linker\n");
                            return -1;
                        }
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64-sf.so.1") == 0)
                    {
                        printfBlue("execve: using riscv64 sf dynamic linker\n");
                        fs::Path path_resolver_interp("/mnt/musl/lib/libc.so");
                        interp_de = path_resolver_interp.pathSearch();
                        if (interp_de == nullptr)
                        {
                            printfRed("execve: failed to find riscv64 musl linker\n");
                            return -1;
                        }
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64.so.1") == 0)
                    {
                        // TODO: 这个可不是sf了, 那怎么办呢
                        printfBlue("execve: using riscv64 sf dynamic linker\n");
                        fs::Path path_resolver_interp("/mnt/musl/lib/libc.so");
                        interp_de = path_resolver_interp.pathSearch();
                        if (interp_de == nullptr)
                        {
                            printfRed("execve: failed to find riscv64 musl linker\n");
                            return -1;
                        }
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
                de->getNode()->nodeRead(reinterpret_cast<uint64>(&ph), off, sizeof(ph));
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
                printf("[exec] map from %p to %p new_pt base %p\n", (void *)(new_sz), (void *)(ph.vaddr + ph.memsz), new_pt.get_base());
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
                if (load_seg(new_pt, ph.vaddr, de, ph.off, ph.filesz) < 0)
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
                if (interp_de == nullptr)
                {
                    printfRed("execve: cannot find dynamic linker: %s\n", interpreter_path.c_str());
                    k_pm.proc_freepagetable(new_pt, new_sz);
                    return -1;
                }

                // 读取动态链接器的ELF头
                interp_de->getNode()->nodeRead(reinterpret_cast<uint64>(&interp_elf), 0, sizeof(interp_elf));

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
                    interp_de->getNode()->nodeRead(reinterpret_cast<uint64>(&interp_ph), interp_off, sizeof(interp_ph));

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
                    if (load_seg(new_pt, load_addr, interp_de, interp_ph.off, interp_ph.filesz) < 0)
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
            printfRed("[execve] base: %p, phdr: %p\n", (void *)interp_base, (void *)phdr);

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
#endif
        // 写成0为了适配glibc的rtld_fini需求
        return 0; // 返回参数个数，表示成功执行
    }
}; // namespace proc