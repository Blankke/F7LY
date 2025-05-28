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
#include "kernel/fs/vfs/elf.hh"
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
        printfGreen("[pm] Process Manager Init\n");
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
                printfCyan("[user pgtbl]==>into proc_pagetable\n");
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
                printf("p->_context.ra: %p\n", p->_context.ra);
                printf("wrp_fork_ret: %p\n", _wrp_fork_ret);
                void (ProcessManager::*fptr)() = &ProcessManager::fork_ret;
                printf("ProcessManager::fork_ret: %p\n", (void *)*(uintptr_t *)&fptr);

                // 设置内核栈指针
                p->_context.sp = p->_kstack + PGSIZE;

                p->_lock.release();

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
        static int first = 1;
        proc::Pcb *proc = get_cur_pcb();
        proc->_lock.release();

        if (first)
        {
            first = 0;
            TODO(
                // 这个TODO已经完成如下
                // printf("sp: %x\n", r_sp());
                filesystem_init();
                filesystem2_init(); // 启动init
            )

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
            assert(f_in != nullptr, "pm: alloc stdin file fail while user init.");

            fs::Path pathout("/dev/stdout");
            fs::FileAttrs fAttrsout = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_out =
                new fs::device_file(fAttrsout, DEV_STDOUT_NUM, pathout.pathSearch());
            assert(f_out != nullptr, "pm: alloc stdout file fail while user init.");

            fs::Path patherr("/dev/stderr");
            fs::FileAttrs fAttrserr = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_err =
                new fs::device_file(fAttrserr, DEV_STDERR_NUM, patherr.pathSearch());
            assert(f_err != nullptr, "pm: alloc stderr file fail while user init.");

            fs::ramfs::k_ramfs.getRoot()->printAllChildrenInfo();
            proc->_ofile[0] = f_in;
            proc->_ofile[1] = f_out;
            proc->_ofile[2] = f_err;
            /// @todo 这里暂时修改进程的工作目录为fat的挂载点
            proc->_cwd = fs::ramfs::k_ramfs.getRoot()->EntrySearch("mnt");
            proc->_cwd_name = "/mnt/";
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
        p->_pt = 0;
        p->_sz = 0;
        p->_pid = 0;
        p->_parent = 0;
        p->_name[0] = 0;
        p->_chan = 0;
        p->_killed = 0;
        p->_xstate = 0;
        p->_state = ProcState::UNUSED;
    }
    void ProcessManager::exit(int state)
    {
        // TODO
    }
    // int ProcessManager::wait(int child_pid, uint64 addr)
    // {
    //     //TODO
    // }
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
        printfGreen("trampoline: %p\n", trampoline);
        printfGreen("TRAMPOLINE: %p\n", TRAMPOLINE);
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
    void ProcessManager::proc_freepagetable(mem::PageTable pt, uint64 sz)
    {
#ifdef RISCV
        mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
        mem::k_vmm.vmfree(pt, 0);
#elif defined(LOONGARCH)
// TODO
#endif
    }

    void ProcessManager::user_init()
    {
#ifdef RISCV
        Pcb *p = alloc_proc();
        _init_proc = p;
        // 传入initcode的地址
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
    void ProcessManager::sleep(void *chan, SpinLock *lock)
    {
        Pcb *p = get_cur_pcb();
        // Must acquire p->lock in order to
        // change p->state and then call sched.
        // Once we hold p->lock, we can be
        // guaranteed that we won't miss any wakeup
        // (wakeup locks p->lock),
        // so it's okay to release lk.
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
        int i, pid;
        Pcb *np;                // new proc
        Pcb *p = get_cur_pcb(); // current proc

        // Allocate process.
        if ((np = alloc_proc()) == nullptr)
        {
            return -1;
        }

        np->_lock.acquire();

        // Copy user memory from parent to child.

        mem::PageTable *curpt, *newpt;
        curpt = p->get_pagetable();
        newpt = np->get_pagetable();

        if (mem::k_vmm.vm_copy(*newpt, *curpt, p->_sz) < 0)
        {
            freeproc(np);
            np->_lock.release();
            return -1;
        }

        ///@todo 栈指针在哪里定义的？我们的sp应该设定到哪里
        /// 学长的默认fork带参数是0，		tf->sp		  = usp;然后就是会把栈指针换位置。

        np->_sz = p->_sz;
        *np->_trapframe = *p->_trapframe; // 拷贝父进程的陷阱值，而不是直接指向
        np->_trapframe->a0 = 0;           // fork 返回值为 0

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
        np->_cwd = p->_cwd;           // 继承当前工作目录
        np->_cwd_name = p->_cwd_name; // 继承当前工作目录名称
        strncpy(np->_name, p->_name, sizeof(p->_name));

        pid = np->_pid;

        np->_lock.release();

        np->_lock.acquire();
        np->_state = ProcState::RUNNABLE;
        np->_start_tick = tmm::k_tm.get_ticks();
        np->_user_ticks = 0;
        np->_lock.release();

        return pid;
    }
    long ProcessManager::brk(long n)
    {
        // Pcb *p = get_cur_pcb(); // 输入参数	：期望的堆大小

        // 	if ( n <= 0 ) // get current heap size
        // 		return p->_heap_ptr;

        // 	// uint64 sz = p->_sz;		// 输出  	：实际的堆大小
        // 	uint64		   oldhp  = p->_heap_ptr;
        // 	uint64		   newhp  = n;
        // 	mm::PageTable &pt	  = p->_pt;
        // 	long		   differ = (long) newhp - (long) oldhp;

        // 	if ( differ < 0 ) // shrink
        // 	{
        // 		if ( mm::k_vmm.vm_dealloc( pt, oldhp, newhp ) < 0 ) { return -1; }
        // 	}
        // 	else if ( differ > 0 )
        // 	{
        // 		if ( mm::k_vmm.vm_alloc( pt, oldhp, newhp ) == 0 ) return -1;
        // 	}

        // 	// log_info( "brk: newsize%d, oldsize%d", newhp, oldhp );
        // 	p->_heap_ptr = newhp;
        // 	return newhp; // 返回堆的大小
        return 0;
    }

    int ProcessManager::execve(eastl::string path, eastl::vector<eastl::string> argv,
                               eastl::vector<eastl::string> envs)
    {
        // Pcb *proc = get_cur_pcb();
        // uint64 sp;
        // uint64 stackbase;
        // mem::PageTable pt;
        // elf::elfhdr elf;
        // elf::proghdr ph = {};
        // // fs::fat::Fat32DirInfo dir_;
        // fs::dentry *de;
        // int i, off;

        // // proc->_pt.freewalk();
        // // mm::k_vmm.vmfree( proc->_pt, proc->_sz );

        // // if ( ( de = fs::fat::k_fatfs.get_root_dir()->EntrySearch( path ) )
        // // 							== nullptr )
        // eastl::string ab_path;
        // if (path[0] == '/')
        //     ab_path = path;
        // else
        //     ab_path = proc->_cwd_name + path;

        // Info("execve file : %s", ab_path.c_str());

        // fs::Path path_resolver(ab_path);
        // if ((de = path_resolver.pathSearch()) == nullptr)
        // // if ( ( de = fs::ramfs::k_ramfs.getRoot()->EntrySearch( "mnt"
        // // )->EntrySearch( path ) ) == nullptr )
        // {
        //     printfRed("execve: cannot find file");
        //     return -1; // 拿到文件夹信息
        // }

        // /// @todo check ELF header
        // de->getNode()->nodeRead(reinterpret_cast<uint64>(&elf), 0, sizeof(elf));

        // if (elf.magic != elf::elfEnum::ELF_MAGIC) // check magicnum
        // {
        //     printfRed("execve: not a valid ELF file");
        //     return -1;
        // }

        // /// @todo 这里有bug，如果后面的代码失败，
        // ///       那么，原来的进程映像就全被释放掉了
        // // proc->_pt.freewalk_mapped();
        // // _proc_create_vm( proc );

        // /// @todo 应当先创建一个新的
        // mem::PageTable new_pt = mem::k_vmm.vm_create();
        // u64 new_sz = 0;

        // // create user pagetable for given process
        // // if((pt = proc_pagetable(proc)).is_null()){
        // // 	printfRed("execve: cannot create pagetable");
        // // 	return -1;
        // // }
        // using psd_t = program_section_desc;
        // int new_sec_cnt = 0;
        // psd_t new_sec_desc[max_program_section_num];
        // {
        //     bool load_bad = false;

        //     for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
        //     {
        //         de->getNode()->nodeRead(reinterpret_cast<uint64>(&ph), off, sizeof(ph));

        //         if (ph.type != elf::elfEnum::ELF_PROG_LOAD)
        //             continue;
        //         if (ph.memsz < ph.filesz)
        //         {
        //             printfRed("execve: memsz < filesz");
        //             load_bad = true;
        //             break;
        //         }
        //         if (ph.vaddr + ph.memsz < ph.vaddr)
        //         {
        //             printfRed("execve: vaddr + memsz < vaddr");
        //             load_bad = true;
        //             break;
        //         }
        //         uint64 sz1;
        //         bool executable = (ph.flags & 0x1); // 段是否可执行？
        //         uint64 flag = executable ? riscv::PteEnum::pte_executable_m : 0;
        //         ulong pva = PGROUNDDOWN(ph.vaddr);
        //         if ((sz1 = mem::k_vmm.vmalloc(new_pt, pva, ph.vaddr + ph.memsz, flag)) ==
        //             0)
        //         {
        //             printfRed("execve: uvmalloc");
        //             load_bad = true;
        //             break;
        //         }
        //         new_sz += PGROUNDUP(ph.vaddr + ph.memsz) - pva;
        //         // if ( ( ph.vaddr % hsai::page_size ) != 0 )
        //         // {
        //         // 	printfRed( "execve: vaddr not aligned" );
        //         // 	proc_freepagetable( proc->_pt, sz );
        //         // 	return -1;
        //         // }

        //         if (load_seg(new_pt, ph.vaddr, de, ph.off, ph.filesz) < 0)
        //         {
        //             printfRed("execve: load_icode");
        //             load_bad = true;
        //             break;
        //         }

        //         // 记录程序段

        //         new_sec_desc[new_sec_cnt]._sec_start = (void *)ph.vaddr;
        //         new_sec_desc[new_sec_cnt]._sec_size = ph.memsz;
        //         new_sec_desc[new_sec_cnt]._debug_name = "LOAD";
        //         new_sec_cnt++;
        //     }

        //     if (load_bad) // load 阶段出错，释放页表
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         return -1;
        //     }
        // }

        // // 为程序映像转储 elf 程序头

        // // sz		 = hsai::page_round_up(sz);
        // u64 phdr = 0; // for AT_PHDR
        // {
        //     ulong phsz = elf.phentsize * elf.phnum;
        //     u64 sz1;
        //     u64 load_end = 0;

        //     for (auto &sec : new_sec_desc) // 搜索load段末尾地址
        //     {
        //         u64 end = (ulong)sec._sec_start + sec._sec_size;
        //         if (end > load_end)
        //             load_end = end;
        //     }

        //     load_end = PGROUNDUP(load_end);
        //     if ((sz1 = mem::k_vmm.vm_alloc(new_pt, load_end, load_end + phsz)) == 0)
        //     {
        //         printfRed("execve: vaalloc");
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         return -1;
        //     }

        //     u8 *tmp = new u8[phsz + 8];
        //     if (tmp == nullptr)
        //     {
        //         printfRed("execve: no mem");
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         return -1;
        //     }

        //     if (de->getNode()->nodeRead((ulong)tmp, elf.phoff, phsz) != phsz)
        //     {
        //         printfRed("execve: node read");
        //         delete[] tmp;
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         return -1;
        //     }

        //     if (mm::k_vmm.copyout(new_pt, load_end, (void *)tmp, phsz) < 0)
        //     {
        //         printfRed("execve: copy out");
        //         delete[] tmp;
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         return -1;
        //     }

        //     delete[] tmp;

        //     phdr = load_end;

        //     // 将该段作为程序段记录下来
        //     psd_t &ph_psd = new_sec_desc[new_sec_cnt];
        //     ph_psd._sec_start = (void *)phdr;
        //     ph_psd._sec_size = phsz;
        //     ph_psd._debug_name = "program headers";
        //     new_sec_cnt++;

        //     new_sz += hsai::page_round_up(phsz);
        // }

        // // allocate two pages , the second is used for the user stack

        // // 此处分配栈空间遵循 memlayout
        // // 进程的用户虚拟空间占用地址低 64GiB，内核虚拟空间从 0xFFF0_0000_0000
        // // 开始 分配栈空间大小为 32 个页面，开头的 1 个页面用作保护页面

        // int stack_page_cnt = default_proc_ustack_pages;
        // stackbase = mm::vml::vm_ustack_end - stack_page_cnt * hsai::page_size;
        // sp = mm::vml::vm_ustack_end;

        // if (mm::k_vmm.vm_alloc(new_pt, stackbase - hsai::page_size, sp) == 0)
        // {
        //     printfRed("execve: vmalloc when allocating stack");
        //     _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //     return -1;
        // }

        // log_trace("execve set stack-base = %p", new_pt.walk_addr(stackbase));
        // log_trace("execve set page containing sp is %p",
        //           new_pt.walk_addr(sp - hsai::page_size));

        // if (mm::k_vmm.vm_set_super(new_pt, stackbase - hsai::page_size, 1) < 0)
        // {
        //     printfRed("execve: set stack protector fail");
        //     _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //     return -1;
        // }

        // mm::k_vmm.vm_set(new_pt, (void *)stackbase, 0, stack_page_cnt);

        // new_sz += (stack_page_cnt + 1) * hsai::page_size;

        // // >>>> 此后的代码用于支持 glibc，包括将 auxv, envp, argv, argc
        // // 压到用户栈中由glibc解析

        // mm::UserstackStream ustack((void *)stackbase, stack_page_cnt * hsai::page_size, &new_pt);
        // ustack.open();

        // // 1. 使用0标记栈底，压入一个用于glibc的伪随机数，并以16字节对齐

        // u64 rd_pos = 0;
        // {
        //     ulong data;
        //     data = 0;
        //     ustack << data;
        //     data = -0x11'4514'FF11'4514UL;
        //     ustack << data;
        //     // data = 0x0050'4D4F'4353'4F43UL;
        //     data = 0x2UL << 60;
        //     ustack << data;
        //     // data = 0x4249'4C47'4B43'5546UL; // "FUCKGLIBCOSCOMP\0"
        //     data = 0x3UL << 60;
        //     ustack << data;

        //     rd_pos = ustack.sp(); // 伪随机数的位置
        // }

        // // 2. 压入 env string

        // ulong uenvp[MAXARG];
        // ulong envc;
        // for (envc = 0; envc < envs.size(); envc++)
        // {
        //     if (envc >= MAXARG)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: too many arguments");
        //         return -1;
        //     }

        //     sp = ustack.sp();
        //     ustack -= (sp - envs[envc].length() - 1) % 16;

        //     if (sp < stackbase)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: sp < stackbase");
        //         return -1;
        //     }
        //     ustack << envs[envc].c_str();
        //     if (ustack.errno() != ustack.rc_ok)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: push into stack");
        //         return -1;
        //     }

        //     uenvp[envc] = ustack.sp();
        // }
        // uenvp[envc] = 0; // envp[end] = nullptr

        // // 3. 压入 arg string

        // uint64 uargv[MAXARG];
        // ulong argc = 0;
        // for (; argc < argv.size(); argc++)
        // {
        //     if (argc >= MAXARG)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: too many arguments");
        //         return -1;
        //     }

        //     sp = ustack.sp();
        //     ustack -= (sp - argv[argc].length() - 1) % 16;

        //     if (sp < stackbase)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: sp < stackbase");
        //         return -1;
        //     }

        //     ustack << argv[argc].c_str();
        //     if (ustack.errno() != ustack.rc_ok)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: push into stack");
        //         return -1;
        //     }

        //     uargv[argc] = ustack.sp();
        // }
        // uargv[argc] = 0; // argv[end] = nullptr

        // sp = ustack.sp();
        // ustack -= sp % 16;

        // // 4. 压入 auxv
        // {
        //     elf::Elf64_auxv_t aux;
        //     // auxv[end] = AT_NULL
        //     aux.a_type = elf::AT_NULL;
        //     aux.a_un.a_val = 0;
        //     ustack << aux;
        //     if (ustack.errno() != ustack.rc_ok)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: push into stack");
        //         return -1;
        //     }

        //     if (phdr != 0)
        //     {
        //         // auxy[4] = AT_PAGESZ
        //         aux.a_type = elf::AT_PAGESZ;
        //         aux.a_un.a_val = hsai::page_size;
        //         ustack << aux;
        //         if (ustack.errno() != ustack.rc_ok)
        //         {
        //             _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //             printfRed("execve: push into stack");
        //             return -1;
        //         }
        //         // auxv[3] = AT_PHNUM
        //         aux.a_type = elf::AT_PHNUM;
        //         aux.a_un.a_val = elf.phnum;
        //         ustack << aux;
        //         if (ustack.errno() != ustack.rc_ok)
        //         {
        //             _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //             printfRed("execve: push into stack");
        //             return -1;
        //         }
        //         // auxv[2] = AT_PHENT
        //         aux.a_type = elf::AT_PHENT;
        //         aux.a_un.a_val = elf.phentsize;
        //         ustack << aux;
        //         if (ustack.errno() != ustack.rc_ok)
        //         {
        //             _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //             printfRed("execve: push into stack");
        //             return -1;
        //         }
        //         // auxv[1] = AT_PHDR
        //         aux.a_type = elf::AT_PHDR;
        //         aux.a_un.a_val = phdr;
        //         ustack << aux;
        //         if (ustack.errno() != ustack.rc_ok)
        //         {
        //             _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //             printfRed("execve: push into stack");
        //             return -1;
        //         }
        //     }
        //     // auxv[0] = AT_RANDOM
        //     aux.a_type = elf::AT_RANDOM;
        //     aux.a_un.a_val = rd_pos;
        //     ustack << aux;
        //     if (ustack.errno() != ustack.rc_ok)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: push into stack");
        //         return -1;
        //     }
        // }

        // // 5. 压入 envp

        // for (long i = envc; i >= 0; --i)
        // {
        //     ustack << uenvp[i];
        //     if (ustack.errno() != ustack.rc_ok)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: push into stack");
        //         return -1;
        //     }
        // }

        // // 6. 压入 argv

        // for (long i = argc; i >= 0; --i)
        // {
        //     ustack << uargv[i];
        //     if (ustack.errno() != ustack.rc_ok)
        //     {
        //         _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //         printfRed("execve: push into stack");
        //         return -1;
        //     }
        // }

        // // 7. 压入 argc

        // ustack << argc;
        // if (ustack.errno() != ustack.rc_ok)
        // {
        //     _free_pt_with_sec(new_pt, new_sec_desc, new_sec_cnt);
        //     printfRed("execve: push into stack");
        //     return -1;
        // }

        // // arguments to user main(argc, argv)
        // // argc is returned via the system call return
        // // value, which is in a0.
        // // hsai::set_trap_frame_arg( proc->_trapframe, 1, sp );

        // // 配置资源限制

        // proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_cur =
        //     proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_max = ustack.sp() - stackbase;

        // // 处理 F_DUPFD_CLOEXEC 标志位
        // for (auto &f : proc->_ofile)
        // {
        //     if (f != nullptr && f->_fl_cloexec)
        //     {
        //         f->free_file();
        //         f = nullptr;
        //     }
        // }

        // // save program name for debugging.
        // for (uint i = 0; i < sizeof proc->_name; i++)
        // {
        //     if (i < path.size())
        //         proc->_name[i] = path[i];
        //     else
        //         proc->_name[i] = 0;
        // }

        // // commit to the user image.
        // proc->exe = ab_path;
        // proc->_sz = new_sz;
        // // unmap program sections
        // for (int i = 0; i < proc->_prog_section_cnt; i++)
        // {
        //     auto &osc = proc->_prog_sections[i];
        //     ulong sec_start, sec_end;
        //     sec_start = hsai::page_round_down((ulong)osc._sec_start);
        //     sec_end = hsai::page_round_up((ulong)osc._sec_start + osc._sec_size);
        //     mm::k_vmm.vm_unmap(proc->_pt, sec_start, (sec_end - sec_start) / hsai::page_size,
        //                        1);
        // }
        // { // unmap heap
        //     ulong start = hsai::page_round_down(proc->_heap_start);
        //     ulong end = hsai::page_round_up(proc->_heap_ptr);
        //     mm::k_vmm.vm_unmap(proc->_pt, start, (end - start) / hsai::page_size, 1);
        // }
        // { // unmap stack
        //     ulong pgs = default_proc_ustack_pages + 1;
        //     ulong start = mm::vm_ustack_end - pgs * hsai::page_size;
        //     mm::k_vmm.vm_unmap(proc->_pt, start, pgs, 1);
        // }
        // proc->_heap_start = 0;
        // for (int i = 0; i < new_sec_cnt; ++i)
        // {
        //     auto &sec = new_sec_desc[i];
        //     proc->_prog_sections[i] = sec;
        //     ulong sec_end = hsai::page_round_up((ulong)sec._sec_start + sec._sec_size);
        //     if (sec_end > proc->_heap_start)
        //         proc->_heap_start = sec_end;
        // }
        // proc->_prog_section_cnt = new_sec_cnt;
        // mm::k_vmm.vm_unmap(proc->_pt, mm::vml::vm_trap_frame, 1, 0);
        // hsai::proc_free(proc);
        // proc->_pt.freewalk();
        // _proc_create_vm(proc, new_pt);
        // hsai::proc_init(proc);

        // // if(!proc->_kpt.is_null())
        // // {
        // // 	mm::PageTable kpt;
        // // 	kpt = mm::k_vmm.vm_create();
        // // 	memcpy( (void *) kpt.get_base(), (void *) mm::k_pagetable.get_base(),
        // // 			hsai::page_size  );

        // // 	ulong pgs	= default_proc_ustack_pages + 1;
        // // 	ulong start = mm::vm_ustack_end - pgs * hsai::page_size;
        // // 	mm::k_vmm.map_data_pages( kpt, start, pgs * hsai::page_size,
        // // 		phy_stackbase, false );

        // // 	hsai::VirtualCpu * cpu = hsai::get_cpu();
        // // 	cpu->set_mmu( kpt );

        // // 	mm::k_vmm.vm_unmap( proc->_kpt, start, pgs, 0 );
        // // 	proc->_kpt.kfreewalk( stackbase - hsai::page_size );

        // // 	proc->_kpt = kpt;
        // // }

        // proc->_heap_ptr = proc->_heap_start;

        // hsai::set_trap_frame_entry(proc->_trapframe, (void *)elf.entry);
        // hsai::set_trap_frame_user_sp(proc->_trapframe, ustack.sp());
        // hsai::set_trap_frame_arg(proc->_trapframe, 1, ustack.sp());
        // proc->_state = ProcState::runnable;

        // /// @note 此处是为了兼容glibc的需要，详见 how_to_adapt_glibc.md
        return 0x0; // rtld_fini
    }

}; // namespace proc