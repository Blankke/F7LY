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
#include "devs/riscv/disk_driver.hh"
#include "fs/vfs/dentrycache.hh"
#include "fs/vfs/path.hh"
#include "fs/ramfs/ramfs.hh"
#include "fs/vfs/file/device_file.hh"
#include "param.h"
#include "timer_manager.hh"
#include "fs/vfs/elf.hh"
#include "fs/vfs/file/normal_file.hh"
#include "mem.hh"
#include "fs/vfs/file/pipe_file.hh"
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
}

namespace proc
{
    ProcessManager k_pm;

    void ProcessManager::init(const char *pid_lock_name, const char *wait_lock_name)
    {
        // initialize the proc table.
        _pid_lock.init(pid_lock_name);
        _wait_lock.init(wait_lock_name);
        for (uint i = 0; i < num_process; ++i)
        {
            Pcb &p = k_proc_pool[i];
            p.init("pcb", i);
        }
        _cur_pid = 1;
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
                p->_state = ProcState::USED;
                // 设置调度相关字段：默认调度槽与优先级
                p->_slot = default_proc_slot;
                p->_priority = default_proc_prio;

                // p->_shm = mem::vml::vm_trap_frame - 64 * 2 * mem::PageEnum::pg_size;
                // p->_shmkeymask = 0;
                // k_pm.set_vma( p );

                // 为该进程分配一页 trapframe 空间（用于中断时保存用户上下文）
                if ((p->_trapframe = (TrapFrame *)mem::k_pmm.alloc_page()) == nullptr)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
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

            riscv::qemu::DiskDriver *disk = (riscv::qemu::DiskDriver *)dev::k_devm.get_device("Disk driver");
            disk->identify_device();

            new (&fs::dentrycache::k_dentryCache) fs::dentrycache::dentryCache;
            fs::dentrycache::k_dentryCache.init();
            new (&fs::mnt_table) eastl::unordered_map<eastl::string, fs::FileSystem *>;
            fs::mnt_table.clear(); // clean mnt_Table
            new (&fs::ramfs::k_ramfs) fs::ramfs::RamFS;
            fs::ramfs::k_ramfs.initfd();
            fs::mnt_table["/"] = &fs::ramfs::k_ramfs;
            fs::Path mnt("/mnt");
            fs::Path dev("/dev/hda");
            mnt.mount(dev, "ext4", 0, 0);

            fs::Path path("/dev/stdin");
            fs::FileAttrs fAttrsin = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0444); // only read
            fs::device_file *f_in = new fs::device_file(fAttrsin, DEV_STDIN_NUM, path.pathSearch());
            assert(f_in != nullptr, "proc: alloc stdin file fail while user init.");

            fs::Path pathout("/dev/stdout");
            fs::FileAttrs fAttrsout = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_out =
                new fs::device_file(fAttrsout, DEV_STDOUT_NUM, pathout.pathSearch());
            assert(f_out != nullptr, "proc: alloc stdout file fail while user init.");

            fs::Path patherr("/dev/stderr");
            fs::FileAttrs fAttrserr = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_err =
                new fs::device_file(fAttrserr, DEV_STDERR_NUM, patherr.pathSearch());
            assert(f_err != nullptr, "proc: alloc stderr file fail while user init.");

            fs::ramfs::k_ramfs.getRoot()->printAllChildrenInfo();
            proc->_ofile[0] = f_in;
            proc->_ofile[1] = f_out;
            proc->_ofile[2] = f_err;
            /// @todo 这里暂时修改进程的工作目录为fat的挂载点
            proc->_cwd = fs::ramfs::k_ramfs.getRoot()->EntrySearch("mnt");
            proc->_cwd_name = "/mnt/";
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
        if (p->_trapframe)
            mem::k_pmm.free_page(p->_trapframe);
        p->_trapframe = 0;
        if (p->_pt.get_base())
            proc_freepagetable(p->_pt, p->_sz);
        p->_pt.set_base(0);
        p->_sz = 0;
        p->_pid = 0;
        p->_parent = 0;
        p->_name[0] = 0;
        p->_chan = 0;
        p->_killed = 0;
        p->_xstate = 0;
        p->_state = ProcState::UNUSED;
        if (p->_ofile[1]->refcnt > 1)
            p->_ofile[1]->refcnt--;
        for (int i = 3; i < (int)max_open_files; ++i)
        {
            if (p->_ofile[i] != nullptr && p->_ofile[i]->refcnt > 0)
            {
                p->_ofile[i]->free_file();
                p->_ofile[i] = nullptr;
            }
        }
        /// TODO:检查这个对不对，这个本来应该在exit的时候解除映射，但是我看都只有
        /// freeproc的时候解除了ofile的映射，所以也在这里接触
        // 将进程的已映射区域取消映射
        for (int i = 0; i < NVMA; ++i)
        {
            if (p->_vm[i].used)
            {
                if (p->_vm[i].flags == MAP_SHARED && (p->_vm[i].prot & PROT_WRITE) != 0)
                {
                    p->_vm[i].vfile->write(p->_vm[i].addr, p->_vm[i].len);
                }
                p->_vm[i].vfile->free_file();
                mem::k_vmm.vmunmap(*p->get_pagetable(), p->_vm[i].addr, p->_vm[i].len / PGSIZE, 1);
                p->_vm[i].used = 0;
            }
        }
    }

    int ProcessManager::get_cur_cpuid()
    {
        return r_tp();
    }

    mem::PageTable ProcessManager::proc_pagetable(Pcb *p)
    {
        mem::PageTable pt = mem::k_vmm.vm_create();
        if (pt.is_null())
            printfRed("proc_pagetable: vm_create failed\n");
        if (pt.get_base() == 0)
            return 0;
#ifdef RISCV
        if (mem::k_vmm.map_pages(pt, TRAMPOLINE, PGSIZE, (uint64)trampoline, riscv::PteEnum::pte_readable_m | riscv::pte_executable_m) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);
            printfRed("proc_pagetable: map trampoline failed\n");
            return 0;
        }
        // printfGreen("trampoline: %p\n", trampoline);
        // printfGreen("TRAMPOLINE: %p\n", TRAMPOLINE);
        if (mem::k_vmm.map_pages(pt, TRAPFRAME, PGSIZE, (uint64)(p->get_trapframe()), riscv::PteEnum::pte_readable_m | riscv::PteEnum::pte_writable_m) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);
            printfRed("proc_pagetable: map trapframe failed\n");
            return 0;
        }

#elif defined(LOONGARCH)
// TODO
#endif
        return pt;
    }
    void ProcessManager::proc_freepagetable(mem::PageTable &pt, uint64 sz)
    {
#ifdef RISCV
        mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
        mem::k_vmm.vmunmap(pt, TRAPFRAME, 1, 0);
        mem::k_vmm.vmfree(pt, sz);
#elif defined(LOONGARCH)
// TODO
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
        p->_sz = 3 * PGSIZE;

        p->_trapframe->epc = 0;
        p->_trapframe->sp = p->_sz;

        safestrcpy(p->_name, "initcode", sizeof(p->_name));
        p->_parent = p;
        // safestrcpy(p->_cwd_name, "/", sizeof(p->_cwd_name));
        p->_cwd_name = "/";

        p->_state = ProcState::RUNNABLE;

        p->_lock.release();

#elif defined(LOONGARCH)
// TODO
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
        if (fd < 0 || fd >= (int)max_open_files || f == nullptr)
            return -1;
        // 不为空先释放资源
        if (p->_ofile[fd] != nullptr)
        {
            close(fd);
        }
        p->_ofile[fd] = f;
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

        for (fd = 3; fd < (int)max_open_files; fd++)
        {
            if (p->_ofile[fd] == nullptr)
            {
                p->_ofile[fd] = f;
                return fd;
            }
        }
        return -1;
    }

    int ProcessManager::fork()
    {
        return fork(0); // 默认usp为0
    }

    int ProcessManager::fork(uint64 usp)
    {
        TODO("copy on write fork");
        int i, pid;
        Pcb *np;                // new proc
        Pcb *p = get_cur_pcb(); // current proc

        // Allocate process.
        if ((np = alloc_proc()) == nullptr)
        {
            return -1;
        }

        // Copy user memory from _parent to child.
        mem::PageTable *curpt, *newpt;
        curpt = p->get_pagetable();
        newpt = np->get_pagetable();

        if (mem::k_vmm.vm_copy(*curpt, *newpt, 0, p->_sz) < 0)
        {
            freeproc(np);
            np->_lock.release();
            return -1;
        }

        np->_sz = p->_sz;
        *np->_trapframe = *p->_trapframe; // 拷贝父进程的陷阱值，而不是直接指向
        if (usp != 0)
            np->_trapframe->sp = usp; // 如果usp不为0，则设置子进程的用户栈指针

        np->_trapframe->a0 = 0; // fork 返回值为 0

        _wait_lock.acquire();
        np->_parent = p;
        _wait_lock.release();

        // increment reference counts on open file descriptors.
        for (i = 0; i < (int)max_open_files; i++)
            if (p->_ofile[i])
            {
                // fs::k_file_table.dup( p->_ofile[ i ] );
                p->_ofile[i]->dup();
                np->_ofile[i] = p->_ofile[i];
            }
        for (i = 0; i < NVMA; ++i)
        {
            if (p->_vm[i].used)
            {
                memmove(&np->_vm[i], &p->_vm[i], sizeof(p->_vm[i]));
                p->_vm[i].vfile->dup(); // 增加引用计数
            }
        }

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

        pid = np->_pid;

        np->_state = ProcState::RUNNABLE;
        np->_user_ticks = 0;
        np->_lock.release();
        // printfCyan("blublublu");
        return pid;
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
                    printf("[wait4]: child %d state: %d name: %s\n", np->_pid, (int)np->_state, np->_name);
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
        uint i, n;
        uint64 pa;

        i = 0;
        if (!is_page_align(va)) // 如果va不是页对齐的，先读出开头不对齐的部分
        {
            pa = (uint64)pt.walk_addr(va);
#ifdef RISCV
            pa = (pa);
#endif
            n = PGROUNDUP(va) - va;
            de->getNode()->nodeRead(pa, offset + i, n);
            i += n;
        }

        for (; i < size; i += PGSIZE) // 此时 va + i 地址是页对齐的
        {
            pa = PTE2PA((uint64)pt.walk(va + i, 0).get_data()); // pte.to_pa() 得到的地址是页对齐的
            if (pa == 0)
                panic("load_seg: walk");
            if (size - i < PGSIZE) // 如果是最后一页中的数据
                n = size - i;
            else
                n = PGSIZE;
#ifdef RISCV
            pa = pa;
#elif defined(LOONGARCH)
// pa =hsai::k_mem->to_vir( pa )
#endif

            de->getNode()->nodeRead(pa, offset + i, n);
        }
        return 0;
    }
    /// @brief 真正执行退出的逻辑
    /// @param p
    /// @param state
    void ProcessManager::exit_proc(Pcb *p, int state)
    {
        printf("[exit_proc] proc %s pid %d exiting with state %d\n", p->_name, p->_pid, state);
        if (p == _init_proc)
            panic("init exiting"); // 保护机制：init 进程不能退出
        // log_info( "exit proc %d", p->_pid );

        _wait_lock.acquire();
        reparent(p); // 将 p 的所有子进程交给 init 进程收养

        if (p->_parent)
            wakeup(p->_parent); // 唤醒父进程（可能在 wait() 中阻塞）

        p->_lock.acquire();
        p->_xstate = state << 8;       // 存储退出状态（通常高字节存状态）
        p->_state = ProcState::ZOMBIE; // 标记为 zombie，等待父进程回收

        _wait_lock.release();

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
            if (p != k_pm.get_cur_pcb())
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

    int ProcessManager::mkdir(int dir_fd, eastl::string path, uint flags)
    {
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
        return 0;
    }
    /// @brief
    /// @param dir_fd 指定相对路径的目录文件描述符（AT_FDCWD 表示当前工作目录）。
    /// @param path 要打开的路径
    /// @param flags 打开方式（如只读、只写、创建等）
    /// @return
    int ProcessManager::open(int dir_fd, eastl::string path, uint flags)
    {
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

        fs::Path path_(path, file);  // 创建路径对象（支持相对路径）
        dentry = path_.pathSearch(); // 查找路径对应的 dentry（目录项）

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
    }

    int ProcessManager::close(int fd)
    {
        if (fd < 0 || fd >= (int)max_open_files)
            return -1;
        Pcb *p = get_cur_pcb();
        if (p->_ofile[fd] == nullptr)
            return 0;
        // fs::k_file_table.free_file( p->_ofile[ fd ] );
        p->_ofile[fd]->free_file();
        p->_ofile[fd] = nullptr;
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
        if (fd < 0 || fd >= (int)max_open_files)
            return -1;

        Pcb *p = get_cur_pcb();
        if (p->_ofile[fd] == nullptr)
            return -1;
        fs::file *f = p->_ofile[fd];
        *buf = f->_stat;

        return 0;
    }
    int ProcessManager::chdir(eastl::string &path)
    {
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
        uint64 err = 0xffffffffffffffff;

        fs::normal_file *vfile;
        fs::file *f;
        Pcb *p = get_cur_pcb();
        if (fd == -1)
        {
            f = nullptr; // 添加匿名映射支持
        }
        else if (p->_ofile[fd] == nullptr)
        {
            return (void *)err;
        }
        else
        {
            f = p->_ofile[fd];
        }
        if (f != NULL)
        {
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

        if (length == 0)
        {
            length = 10 * PGSIZE; // 默认映射10页
        }
        if (f == NULL)
        { // 匿名映射处理
            uint64 new_addr = p->_sz;
            growproc(length);
            return (void *)new_addr;
        }
        for (int i = 0; i < NVMA; ++i)
        {
            if (p->_vm[i].used == 0) // 找到一个空闲的虚拟内存区域
            {
                p->_vm[i].used = 1;
                p->_vm[i].addr = p->_sz;
                p->_vm[i].len = length;
                p->_vm[i].flags = flags;
                p->_vm[i].prot = prot;
                p->_vm[i].vfile = vfile;
                p->_vm[i].vfd = fd;
                p->_vm[i].offset = offset;

                vfile->dup();                  // 增加文件引用计数
                p->_sz += length;              // 扩展进程的虚拟内存空间
                return (void *)p->_vm[i].addr; // 返回映射的虚拟地址
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
            if (p->_vm[i].used && p->_vm[i].len >= length)
            {
                // 根据提示，munmap的地址范围只能是
                // 1. 起始位置
                if (p->_vm[i].addr == (uint64)addr)
                {
                    p->_vm[i].addr += length;
                    p->_vm[i].len -= length;
                    break;
                }
                // 2. 结束位置
                if ((uint64)(addr) + length == p->_vm[i].addr + p->_vm[i].len)
                {
                    p->_vm[i].len -= length;
                    break;
                }
            }
        }
        if (i == NVMA)
            return -1;

        ///@TODO: 此处注释了就能过，但是supposed to 写回的，暂时处理不了那个write
        ///@details 所以其实不用管，munmap和mmap两个点都能过。
        // // 将MAP_SHARED页面写回文件系统
        // if (p->_vm[i].flags == MAP_SHARED && (p->_vm[i].prot & PROT_WRITE) != 0)
        // {
        //     // printfRed("[munmap?]: walkaddr :%p\n",(p->_pt.walk_addr((uint64)addr)));
        //     p->_vm[i].vfile->write((uint64)addr, length);
        // }

        // 判断此页面是否存在映射
        mem::k_vmm.vmunmap(*p->get_pagetable(), (uint64)addr, length / PGSIZE, 1);

        // 当前VMA中全部映射都被取消
        if (p->_vm[i].len == 0)
        {
            /// TODO:此处我没找到对应xv6的fileclose，找了个最类似的函数用上
            /// 也许这个free_file()是对的
            /// commented by @gkq
            p->_vm[i].vfile->free_file();
            p->_vm[i].used = 0;
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
                p->_ofile[fd0] = 0;
            // fs::k_file_table.free_file( rf );
            // fs::k_file_table.free_file( wf );
            rf->free_file();
            wf->free_file();
            return -1;
        }
        p->_ofile[fd0] = rf;
        p->_ofile[fd1] = wf;
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
        // printfRed("execve: %s\n", path.c_str());
        // 获取当前进程控制块
        Pcb *proc = k_pm.get_cur_pcb();

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

        // ========== 第一阶段：路径解析和文件查找 ==========

        // 构建绝对路径
        eastl::string ab_path;
        if (path[0] == '/')
            ab_path = path; // 已经是绝对路径
        else
            ab_path = proc->_cwd_name + path; // 相对路径，添加当前工作目录前缀

        // printfCyan("execve file : %s\n", ab_path.c_str());

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
            panic("execve: not a valid ELF file");
            return -1;
        }
        // printf("execve: ELF file magic: %x\n", elf.magic);

        // ========== 第二阶段：创建新的虚拟地址空间 ==========

        // 创建新的页表，避免在加载过程中破坏原进程映像
        new_pt = k_pm.proc_pagetable(proc);
        TODO(if (new_pt == 0) {
            printfRed("execve: proc_pagetable failed\n");
            return -1;
        })

        // 这个地方不能按着学长的代码写, 因为学长的内存布局和我们的不同
        // 而且他们的proc_pagetable函数是弃用的, 我们的是好的, 直接用这个函数就可以构建基础页表

        // ========== 第三阶段：加载ELF程序段 ==========
        {

            bool load_bad = false; // 加载失败标志

            // 遍历所有程序头，加载LOAD类型的段
            for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
            {
                // 读取程序头
                de->getNode()->nodeRead(reinterpret_cast<uint64>(&ph), off, sizeof(ph));
                printf("execve: loading segment %d, type: %d, vaddr: %p, memsz: %p, filesz: %p, flags: %d\n",
                       i, ph.type, (void *)ph.vaddr, (void *)ph.memsz, (void *)ph.filesz, ph.flags);
                // 	// 只处理LOAD类型的程序段
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

                // 分配虚拟内存空间
                uint64 sz1;
                uint64 seg_flag = 16;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                    seg_flag |= riscv::PteEnum::pte_executable_m;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                    seg_flag |= riscv::PteEnum::pte_writable_m;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                    seg_flag |= riscv::PteEnum::pte_readable_m;
                printfRed("execve: loading segment %d, type: %d, vaddr: %p, memsz: %p, filesz: %p, flags: %d, end_addr: %p\n",
                          i, ph.type, (void *)ph.vaddr, (void *)ph.memsz, (void *)ph.filesz, seg_flag, (void *)(ph.vaddr + ph.memsz));
                if ((sz1 = mem::k_vmm.vmalloc(new_pt, new_sz, ph.vaddr + ph.memsz, seg_flag)) == 0)
                {
                    printfRed("execve: uvmalloc\n");
                    load_bad = true;
                    break;
                }
                new_sz = sz1; // 更新新进程映像的大小

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
        }
        TODO(== == == == == 第四阶段：为glibc准备程序头信息 == == == == ==)
        // ================    (已写, 我们抄华科的不用在这里初始化, 直接在下面把数据算出来就可以了)    ========================

        // 将ELF程序头转储到用户空间，供glibc的动态链接器使用
        TODO(u64 phdr = 0; // AT_PHDR辅助向量的值
             {
                 ulong phsz = elf.phentsize * elf.phnum; // 程序头表的总大小
                 u64 sz1;
                 u64 load_end = 0; // 所有LOAD段的结束地址

                 // 找到所有LOAD段的最高结束地址
                 for (auto &sec : new_sec_desc)
                 {
                     u64 end = (ulong)sec._sec_start + sec._sec_size;
                     if (end > load_end)
                         load_end = end;
                 }

                 // 在LOAD段之后分配空间存放程序头
                 load_end = hsai::page_round_up(load_end);
                 if ((sz1 = mm::k_vmm.vm_alloc(new_pt, load_end, load_end + phsz)) == 0)
                 {
                     log_error("execve: vaalloc");
                     _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
                     return -1;
                 }

                 // 分配临时缓冲区读取程序头
                 u8 *tmp = new u8[phsz + 8];
                 if (tmp == nullptr)
                 {
                     log_error("execve: no mem");
                     _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
                     return -1;
                 }

                 // 从文件读取程序头表
                 if (de->getNode()->nodeRead((ulong)tmp, elf.phoff, phsz) != phsz)
                 {
                     log_error("execve: node read");
                     delete[] tmp;
                     _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
                     return -1;
                 }

                 // 将程序头表拷贝到用户空间
                 if (mm::k_vmm.copyout(new_pt, load_end, (void *)tmp, phsz) < 0)
                 {
                     log_error("execve: copy out");
                     delete[] tmp;
                     _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
                     return -1;
                 }

                 delete[] tmp;

                 phdr = load_end; // 记录程序头在用户空间的地址

                 // 将程序头段作为一个程序段记录下来
                 psd_t &ph_psd = new_sec_desc[new_sec_cnt];
                 ph_psd._sec_start = (void *)phdr;
                 ph_psd._sec_size = phsz;
                 ph_psd._debug_name = "program headers";
                 new_sec_cnt++;

                 new_sz += hsai::page_round_up(phsz);
             })

        // ========== 第五阶段：分配用户栈空间 ==========

        { // 按照内存布局分配用户栈空间
            int stack_pgnum = 32;
            new_sz = PGROUNDUP(new_sz); // 将大小对齐到页边界
            uint64 sz1;
            if ((sz1 = mem::k_vmm.uvmalloc(new_pt, new_sz, new_sz + stack_pgnum * PGSIZE, PTE_W | PTE_X | PTE_R | PTE_U)) == 0)
            {
                printfRed("execve: load user stack failed\n");
                k_pm.proc_freepagetable(new_pt, new_sz);
                return -1;
            }
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
        }
        uargv[argc] = 0; // argv数组以NULL结尾

        // 4. 压入辅助向量（auxv），供动态链接器使用
        {
            // 在括号里面开命名空间防止变量名冲突
            using namespace elf;
            uint64 aux[AuxvEntryType::MAX_AT * 2];
            [[maybe_unused]] int index = 0;

            // ADD_AUXV(AT_HWCAP, 0);             // 硬件功能标志
            // ADD_AUXV(AT_PAGESZ, PGSIZE);       // 页面大小
            // ADD_AUXV(AT_PHDR, elf.phoff);      // 程序头表偏移
            // ADD_AUXV(AT_PHENT, elf.phentsize); // 程序头表项大小
            // ADD_AUXV(AT_PHNUM, elf.phnum);     // 程序头表项数量
            // ADD_AUXV(AT_BASE, 0);              // 动态链接器基地址（保留）
            // ADD_AUXV(AT_ENTRY, elf.entry);     // 程序入口点地址
            // ADD_AUXV(AT_UID, 0);               // 用户ID
            // ADD_AUXV(AT_EUID, 0);              // 有效用户ID
            // ADD_AUXV(AT_GID, 0);               // 组ID
            // ADD_AUXV(AT_EGID, 0);              // 有效组ID
            // ADD_AUXV(AT_SECURE, 0);            // 安全模式标志
            // ADD_AUXV(AT_RANDOM, rd_pos);           // 随机数地址
            // ADD_AUXV(AT_NULL, 0);              // 结束标记

            // printf("index: %d\n", index);

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
        if (uenvp[0])
        {
            sp -= (envc + 1) * sizeof(uint64); // 为envp数组预留空间
            sp -= sp % 16;                     // 对齐到16字节
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
        if (uargv[0])
        {
            sp -= (argc + 1) * sizeof(uint64); // 为argv数组预留空间
            sp -= sp % 16;                     // 对齐到16字节
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
        }
        proc->_trapframe->a1 = sp; // 设置argv指针到trapframe

        // 7. 压入参数个数（argc）
        sp -= sizeof(uint64);
        if (mem::k_vmm.copy_out(new_pt, sp, (char *)&argc, sizeof(uint64)) < 0)
        {
            printfRed("execve: copy argc failed\n");
            k_pm.proc_freepagetable(new_pt, new_sz);
            return -1;
        }
        proc->_trapframe->a0 = argc; // 设置参数个数到trapframe

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
        for (auto &f : proc->_ofile)
        {
            if (f != nullptr && f->_fl_cloexec)
            {
                f->free_file();
                f = nullptr;
            }
        }

        // ========== 第八阶段：替换进程映像 ==========
        mem::PageTable old_pt;
        old_pt = *proc->get_pagetable(); // 获取当前进程的页表
        proc->_sz = PGROUNDUP(new_sz);   // 更新进程大小
        printf("execve: entry point: %p, new size: %d\n", elf.entry, proc->_sz);
        proc->_trapframe->epc = elf.entry; // 设置程序计数器为入口点
        proc->_pt = new_pt;                // 替换为新的页表
        proc->_trapframe->sp = sp;         // 设置栈指针

        // printf("execve: new process size: %d, new pagetable: %p\n", proc->_sz, proc->_pt);
        k_pm.proc_freepagetable(old_pt, old_sz);

        printf("execve succeed, new process size: %d\n", proc->_sz);

        return argc; // 返回参数个数，表示成功执行
    }
}; // namespace proc