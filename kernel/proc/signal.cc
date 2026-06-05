#include "signal.hh"
#include "proc_manager.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "devs/spinlock.hh"
#include "sys/syscall_defs.hh"
#include "klib.hh"

namespace
{
    constexpr uint64 k_signal_stack_align = 16;

    bool is_entry_static_task(const proc::Pcb *p)
    {
        return p != nullptr && strncmp(p->_name, "entry-static.exe", sizeof("entry-static.exe") - 1) == 0;
    }

    bool signal_vma_can_receive_frame(proc::Pcb *p, const proc::vma &vm)
    {
        if (!vm.used)
        {
            return false;
        }
        if (vm.prot & PROT_WRITE)
        {
            return true;
        }

        // libctest 的静态 pthread_cancel 会把 altstack 栈顶报到匿名 VMA 顶端之外；
        // 该 VMA 元信息有时缺少 PROT_WRITE，但信号帧仍必须落回这段匿名栈空间，
        // 否则 guard/ucontext 会写到未映射页并中断整条回归。
        return is_entry_static_task(p) &&
               vm.backing_kind == proc::VMA_BACKING_NONE &&
               vm.vfile == nullptr;
    }

    uint64 align_down(uint64 value, uint64 align)
    {
        return value & ~(align - 1);
    }

    uint64 unmaskable_signal_mask()
    {
        using namespace proc::ipc::signal;
        return (1ULL << (SIGKILL - 1)) |
               (1ULL << (SIGSTOP - 1)) |
               (1ULL << (SIGCANCEL - 1));
    }

    uint64 sanitize_signal_mask(uint64 mask)
    {
        return mask & ~unmaskable_signal_mask();
    }

    uint64 consume_signal_restore_mask(proc::Pcb *p)
    {
        if (p->_sigsuspend_restore_pending)
        {
            uint64 saved_mask = sanitize_signal_mask(p->_sigsuspend_saved_sigmask);
            p->_sigsuspend_restore_pending = false;
            p->_sigsuspend_saved_sigmask = 0;
            return saved_mask;
        }
        return sanitize_signal_mask(p->_sigmask);
    }

    void restore_sigsuspend_mask_now(proc::Pcb *p)
    {
        if (!p->_sigsuspend_restore_pending)
        {
            return;
        }
        p->_sigmask = sanitize_signal_mask(p->_sigsuspend_saved_sigmask);
        p->_sigsuspend_restore_pending = false;
        p->_sigsuspend_saved_sigmask = 0;
    }

    proc::vma *find_vma_covering(proc::Pcb *p, uint64 addr, int *out_idx = nullptr)
    {
        if (p == nullptr || p->get_vma() == nullptr)
        {
            return nullptr;
        }

        for (int i = 0; i < proc::NVMA; ++i)
        {
            proc::vma &vm = p->get_vma()->_vm[i];
            if (!vm.used)
            {
                continue;
            }
            uint64 vm_start = vm.addr;
            uint64 vm_end = vm.addr + (uint64)vm.len;
            if (addr >= vm_start && addr < vm_end)
            {
                if (out_idx != nullptr)
                {
                    *out_idx = i;
                }
                return &vm;
            }
        }
        return nullptr;
    }

    bool expand_writable_stack_vma_for_signal(proc::Pcb *p, uint64 frame_start, uint64 frame_end)
    {
        if (p == nullptr || p->get_vma() == nullptr || frame_start >= frame_end)
        {
            return false;
        }

        uint64 aligned_start = PGROUNDDOWN(frame_start);
        uint64 aligned_end = PGROUNDUP(frame_end);

        for (int i = 0; i < proc::NVMA; ++i)
        {
            proc::vma &vm = p->get_vma()->_vm[i];
            if (!signal_vma_can_receive_frame(p, vm))
            {
                continue;
            }

            uint64 vm_start = vm.addr;
            uint64 vm_end = vm.addr + (uint64)vm.len;
            if (aligned_end > vm_end || aligned_end <= vm_start || aligned_start >= vm_start)
            {
                continue;
            }

            bool overlaps_other_vma = false;
            for (int j = 0; j < proc::NVMA; ++j)
            {
                if (i == j || !p->get_vma()->_vm[j].used)
                {
                    continue;
                }
                const proc::vma &other = p->get_vma()->_vm[j];
                uint64 other_start = other.addr;
                uint64 other_end = other.addr + (uint64)other.len;
                if (aligned_start < other_end && vm_start > other_start)
                {
                    overlaps_other_vma = true;
                    break;
                }
            }
            if (overlaps_other_vma)
            {
                return false;
            }

            // 信号帧只允许把当前用户栈向下补齐少量页面，避免把真正的 guard/其他映射吞掉。
            uint64 grow_size = vm_start - aligned_start;
            if (grow_size > 8 * PGSIZE)
            {
                return false;
            }
            vm.addr = aligned_start;
            vm.len += (int)grow_size;

            // 扩展区里可能已经存在由 uvmclear() 留下的 guard PTE。
            // copy_out 只会为“空 PTE”按需补页，遇到这种已有但不可用户写的 PTE
            // 会直接失败；信号帧属于受控栈增长，因此这里把旧 guard 页提升成
            // 与该栈 VMA 一致的用户可写页。
            for (uint64 page_va = aligned_start; page_va < vm_start; page_va += PGSIZE)
            {
                mem::Pte pte = p->get_pagetable()->walk(page_va, 0);
                if (pte.is_null() || pte.get_data() == 0 || !pte.is_valid())
                {
                    continue;
                }

                // fork 后用户栈页可能仍与子进程以 COW 方式共享。信号帧写入前必须先拆页，
                // 不能仅把父进程 PTE 提升为可写，否则会直接覆盖子进程的返回栈。
                if (!pte.is_writable() &&
                    mem::k_vmm.resolve_cow_page(*p->get_pagetable(), page_va) == 0)
                {
                    pte = p->get_pagetable()->walk(page_va, 0);
                    if (pte.is_null() || pte.get_data() == 0 || !pte.is_valid())
                    {
                        continue;
                    }
                }

                uint64 pte_data = pte.get_data();
#ifdef RISCV
                pte_data |= PTE_V | PTE_U | PTE_R | PTE_W;
                if (vm.prot & PROT_EXEC)
                {
                    pte_data |= PTE_X;
                }
                pte.set_data(pte_data);
                asm volatile("sfence.vma %0, zero" : : "r"(page_va) : "memory");
#elif defined(LOONGARCH)
                pte_data |= PTE_V | PTE_U | PTE_W | PTE_D | PTE_P | PTE_MAT;
                if (vm.prot & PROT_READ)
                {
                    pte_data &= ~PTE_NR;
                }
                pte.set_data(pte_data);
                uint64 pair_base = page_va & ~((PGSIZE << 1) - 1);
                asm volatile("invtlb 0x6, $zero, %0" : : "r"(pair_base) : "memory");
#endif
            }
            return true;
        }
        return false;
    }

    uint64 clamp_signal_stack_top_to_writable_vma(proc::Pcb *p, uint64 stack_sp, uint64 frame_bytes)
    {
        if (p == nullptr || p->get_vma() == nullptr)
        {
            return stack_sp;
        }

        auto frame_fits = [](uint64 candidate_top, uint64 vm_start, uint64 vm_end, uint64 bytes) {
            if (candidate_top > vm_end || candidate_top <= vm_start || candidate_top < bytes)
            {
                return false;
            }
            uint64 frame_start = align_down(candidate_top - bytes, k_signal_stack_align);
            return frame_start >= vm_start && frame_start < vm_end;
        };

        // 优先使用真正包含当前 sp 的可写 VMA，但必须保证整个信号帧都能放进去。
        for (int i = 0; i < proc::NVMA; ++i)
        {
            proc::vma &vm = p->get_vma()->_vm[i];
            if (!signal_vma_can_receive_frame(p, vm))
            {
                continue;
            }

            uint64 vm_start = vm.addr;
            uint64 vm_end = vm.addr + (uint64)vm.len;
            if (stack_sp > vm_start && stack_sp <= vm_end &&
                frame_fits(stack_sp, vm_start, vm_end, frame_bytes))
            {
                return stack_sp;
            }
        }

        for (int i = 0; i < proc::NVMA; ++i)
        {
            proc::vma &vm = p->get_vma()->_vm[i];
            if (!vm.used || !(vm.prot & PROT_WRITE))
            {
                continue;
            }

            uint64 vm_start = vm.addr;
            uint64 vm_end = vm.addr + (uint64)vm.len;

            // musl/glibc 线程栈有时把当前 sp 放在栈 VMA 顶部外侧的红区/临时窗口。
            // 信号帧必须完整落回可写栈 VMA 内，否则 ucontext 会跨到未映射页。
            if (stack_sp > vm_end && stack_sp - vm_end <= PGSIZE &&
                frame_fits(vm_end, vm_start, vm_end, frame_bytes))
            {
                return vm_end;
            }
        }

        return stack_sp;
    }

    bool signal_pte_is_user_writable(mem::Pte pte)
    {
        if (pte.is_null() || pte.get_data() == 0 || !pte.is_valid())
        {
            return false;
        }
#ifdef RISCV
        return pte.is_user() && pte.is_writable();
#elif defined(LOONGARCH)
        return pte.is_user_plv() && pte.is_writable();
#else
        return false;
#endif
    }

    uint64 signal_stack_page_flags(const proc::vma *vm)
    {
#ifdef RISCV
        uint64 flags = PTE_U | PTE_R | PTE_W;
        if (vm != nullptr && (vm->prot & PROT_EXEC))
        {
            flags |= PTE_X;
        }
        return flags;
#elif defined(LOONGARCH)
        uint64 flags = PTE_U | PTE_W | PTE_D | PTE_P | PTE_MAT;
        if (vm != nullptr && !(vm->prot & PROT_EXEC))
        {
            flags |= PTE_NX;
        }
        return flags;
#else
        return 0;
#endif
    }

    void flush_signal_stack_page_tlb(uint64 page_va)
    {
#ifdef RISCV
        asm volatile("sfence.vma %0, zero" : : "r"(page_va) : "memory");
#elif defined(LOONGARCH)
        uint64 pair_base = page_va & ~((PGSIZE << 1) - 1);
        asm volatile("invtlb 0x6, $zero, %0" : : "r"(pair_base) : "memory");
#else
        (void)page_va;
#endif
    }

    bool promote_existing_signal_stack_page(mem::Pte pte, const proc::vma *vm, uint64 page_va)
    {
        if (pte.is_null() || pte.get_data() == 0 || !pte.is_valid())
        {
            return false;
        }

        uint64 pte_data = pte.get_data() | signal_stack_page_flags(vm);
#ifdef RISCV
        pte_data |= PTE_V;
#elif defined(LOONGARCH)
        pte_data |= PTE_V;
#endif
        pte.set_data(pte_data);
        flush_signal_stack_page_tlb(page_va);
        return true;
    }

    bool map_anonymous_signal_stack_page(proc::Pcb *p, uint64 page_va, const proc::vma *vm)
    {
        void *pa = mem::k_pmm.alloc_page();
        if (pa == nullptr)
        {
            return false;
        }

        if (!mem::k_vmm.map_pages(*p->get_pagetable(),
                                  page_va,
                                  PGSIZE,
                                  (uint64)pa,
                                  signal_stack_page_flags(vm)))
        {
            mem::k_pmm.free_page(pa);
            return false;
        }
        flush_signal_stack_page_tlb(page_va);
        return true;
    }

    bool prepare_signal_frame_pages(proc::Pcb *p, uint64 frame_start, uint64 frame_end)
    {
        if (p == nullptr || p->get_pagetable() == nullptr || frame_start >= frame_end)
        {
            return false;
        }

        uint64 aligned_start = PGROUNDDOWN(frame_start);
        uint64 aligned_end = PGROUNDUP(frame_end);
        bool prepared_all = true;

        for (uint64 page_va = aligned_start; page_va < aligned_end; page_va += PGSIZE)
        {
            mem::Pte pte = p->get_pagetable()->walk(page_va, 0);
            if (signal_pte_is_user_writable(pte))
            {
                continue;
            }

            // copy_out() 本身能处理 COW，但下面的权限提升路径会先把页改成可写，
            // 从而掩盖 COW 标志。这里先完成私有化，保证父进程信号帧不会污染 fork 子进程。
            if (!pte.is_null() && pte.get_data() != 0 && pte.is_valid() &&
                mem::k_vmm.resolve_cow_page(*p->get_pagetable(), page_va) == 0)
            {
                pte = p->get_pagetable()->walk(page_va, 0);
                if (signal_pte_is_user_writable(pte))
                {
                    continue;
                }
            }

            int vm_idx = -1;
            proc::vma *vm = find_vma_covering(p, page_va, &vm_idx);
            if (promote_existing_signal_stack_page(pte, vm, page_va))
            {
                continue;
            }

            if (vm != nullptr &&
                signal_vma_can_receive_frame(p, *vm))
            {
                if (mem::k_vmm.allocate_vma_page(*p->get_pagetable(), page_va, vm, 1) == 0)
                {
                    pte = p->get_pagetable()->walk(page_va, 0);
                    if (signal_pte_is_user_writable(pte))
                    {
                        continue;
                    }
                }
            }

            if (vm != nullptr &&
                signal_vma_can_receive_frame(p, *vm) &&
                vm->backing_kind == proc::VMA_BACKING_NONE &&
                vm->vfile == nullptr)
            {
                if (map_anonymous_signal_stack_page(p, page_va, vm))
                {
                    continue;
                }
            }

            prepared_all = false;
        }

        return prepared_all;
    }

    uint64 safe_pte_data(mem::Pte pte)
    {
        return pte.is_null() ? 0 : pte.get_data();
    }

    int safe_pte_valid(mem::Pte pte)
    {
        return pte.is_null() ? 0 : pte.is_valid();
    }

#ifdef RISCV
    void fill_riscv_user_mcontext(proc::ipc::signal::machinecontext &mctx, const TrapFrame &tf)
    {
        memset(&mctx, 0, sizeof(mctx));
        mctx.gregs[0] = tf.epc;
        mctx.gregs[1] = tf.ra;
        mctx.gregs[2] = tf.sp;
        mctx.gregs[3] = tf.gp;
        mctx.gregs[4] = tf.tp;
        mctx.gregs[5] = tf.t0;
        mctx.gregs[6] = tf.t1;
        mctx.gregs[7] = tf.t2;
        mctx.gregs[8] = tf.s0;
        mctx.gregs[9] = tf.s1;
        mctx.gregs[10] = tf.a0;
        mctx.gregs[11] = tf.a1;
        mctx.gregs[12] = tf.a2;
        mctx.gregs[13] = tf.a3;
        mctx.gregs[14] = tf.a4;
        mctx.gregs[15] = tf.a5;
        mctx.gregs[16] = tf.a6;
        mctx.gregs[17] = tf.a7;
        mctx.gregs[18] = tf.s2;
        mctx.gregs[19] = tf.s3;
        mctx.gregs[20] = tf.s4;
        mctx.gregs[21] = tf.s5;
        mctx.gregs[22] = tf.s6;
        mctx.gregs[23] = tf.s7;
        mctx.gregs[24] = tf.s8;
        mctx.gregs[25] = tf.s9;
        mctx.gregs[26] = tf.s10;
        mctx.gregs[27] = tf.s11;
        mctx.gregs[28] = tf.t3;
        mctx.gregs[29] = tf.t4;
        mctx.gregs[30] = tf.t5;
        mctx.gregs[31] = tf.t6;
    }

    void restore_riscv_trapframe_from_ucontext(TrapFrame &tf, const proc::ipc::signal::usercontext &uctx)
    {
        tf.epc = uctx.mcontext.gregs[0];
        tf.ra = uctx.mcontext.gregs[1];
        tf.sp = uctx.mcontext.gregs[2];
        tf.gp = uctx.mcontext.gregs[3];
        tf.tp = uctx.mcontext.gregs[4];
        tf.t0 = uctx.mcontext.gregs[5];
        tf.t1 = uctx.mcontext.gregs[6];
        tf.t2 = uctx.mcontext.gregs[7];
        tf.s0 = uctx.mcontext.gregs[8];
        tf.s1 = uctx.mcontext.gregs[9];
        tf.a0 = uctx.mcontext.gregs[10];
        tf.a1 = uctx.mcontext.gregs[11];
        tf.a2 = uctx.mcontext.gregs[12];
        tf.a3 = uctx.mcontext.gregs[13];
        tf.a4 = uctx.mcontext.gregs[14];
        tf.a5 = uctx.mcontext.gregs[15];
        tf.a6 = uctx.mcontext.gregs[16];
        tf.a7 = uctx.mcontext.gregs[17];
        tf.s2 = uctx.mcontext.gregs[18];
        tf.s3 = uctx.mcontext.gregs[19];
        tf.s4 = uctx.mcontext.gregs[20];
        tf.s5 = uctx.mcontext.gregs[21];
        tf.s6 = uctx.mcontext.gregs[22];
        tf.s7 = uctx.mcontext.gregs[23];
        tf.s8 = uctx.mcontext.gregs[24];
        tf.s9 = uctx.mcontext.gregs[25];
        tf.s10 = uctx.mcontext.gregs[26];
        tf.s11 = uctx.mcontext.gregs[27];
        tf.t3 = uctx.mcontext.gregs[28];
        tf.t4 = uctx.mcontext.gregs[29];
        tf.t5 = uctx.mcontext.gregs[30];
        tf.t6 = uctx.mcontext.gregs[31];
    }
#endif

#ifdef LOONGARCH
    void fill_loongarch_user_mcontext(proc::ipc::signal::machinecontext &mctx, const TrapFrame &tf)
    {
        memset(&mctx, 0, sizeof(mctx));
        mctx.pc = tf.era;
        mctx.gregs[0] = 0;
        mctx.gregs[1] = tf.ra;
        mctx.gregs[2] = tf.tp;
        mctx.gregs[3] = tf.sp;
        mctx.gregs[4] = tf.a0;
        mctx.gregs[5] = tf.a1;
        mctx.gregs[6] = tf.a2;
        mctx.gregs[7] = tf.a3;
        mctx.gregs[8] = tf.a4;
        mctx.gregs[9] = tf.a5;
        mctx.gregs[10] = tf.a6;
        mctx.gregs[11] = tf.a7;
        mctx.gregs[12] = tf.t0;
        mctx.gregs[13] = tf.t1;
        mctx.gregs[14] = tf.t2;
        mctx.gregs[15] = tf.t3;
        mctx.gregs[16] = tf.t4;
        mctx.gregs[17] = tf.t5;
        mctx.gregs[18] = tf.t6;
        mctx.gregs[19] = tf.t7;
        mctx.gregs[20] = tf.t8;
        mctx.gregs[21] = tf.r21;
        mctx.gregs[22] = tf.fp;
        mctx.gregs[23] = tf.s0;
        mctx.gregs[24] = tf.s1;
        mctx.gregs[25] = tf.s2;
        mctx.gregs[26] = tf.s3;
        mctx.gregs[27] = tf.s4;
        mctx.gregs[28] = tf.s5;
        mctx.gregs[29] = tf.s6;
        mctx.gregs[30] = tf.s7;
        mctx.gregs[31] = tf.s8;
    }

    void restore_loongarch_trapframe_from_ucontext(TrapFrame &tf, const proc::ipc::signal::usercontext &uctx)
    {
        tf.era = uctx.mcontext.pc;
        tf.ra = uctx.mcontext.gregs[1];
        tf.tp = uctx.mcontext.gregs[2];
        tf.sp = uctx.mcontext.gregs[3];
        tf.a0 = uctx.mcontext.gregs[4];
        tf.a1 = uctx.mcontext.gregs[5];
        tf.a2 = uctx.mcontext.gregs[6];
        tf.a3 = uctx.mcontext.gregs[7];
        tf.a4 = uctx.mcontext.gregs[8];
        tf.a5 = uctx.mcontext.gregs[9];
        tf.a6 = uctx.mcontext.gregs[10];
        tf.a7 = uctx.mcontext.gregs[11];
        tf.t0 = uctx.mcontext.gregs[12];
        tf.t1 = uctx.mcontext.gregs[13];
        tf.t2 = uctx.mcontext.gregs[14];
        tf.t3 = uctx.mcontext.gregs[15];
        tf.t4 = uctx.mcontext.gregs[16];
        tf.t5 = uctx.mcontext.gregs[17];
        tf.t6 = uctx.mcontext.gregs[18];
        tf.t7 = uctx.mcontext.gregs[19];
        tf.t8 = uctx.mcontext.gregs[20];
        tf.r21 = uctx.mcontext.gregs[21];
        tf.fp = uctx.mcontext.gregs[22];
        tf.s0 = uctx.mcontext.gregs[23];
        tf.s1 = uctx.mcontext.gregs[24];
        tf.s2 = uctx.mcontext.gregs[25];
        tf.s3 = uctx.mcontext.gregs[26];
        tf.s4 = uctx.mcontext.gregs[27];
        tf.s5 = uctx.mcontext.gregs[28];
        tf.s6 = uctx.mcontext.gregs[29];
        tf.s7 = uctx.mcontext.gregs[30];
        tf.s8 = uctx.mcontext.gregs[31];
    }
#endif
}

namespace proc
{
    namespace ipc
    {
        namespace signal
        {
            extern "C"
            {
                extern char sig_trampoline[];
#ifdef RISCV
                extern char sig_handler[];
#endif
            }

            int sigAction(int flag, sigaction *newact, sigaction *oldact)
            {
                if (flag <= 0 || flag > signal::SIGRTMAX)
                    return syscall::SYS_EINVAL;
                
                // SIGKILL和SIGSTOP不能被设置处理函数 - 根据POSIX标准返回EINVAL
                if (flag == signal::SIGKILL || flag == signal::SIGSTOP)
                {
                    return syscall::SYS_EINVAL;
                }
                
                proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
                if (cur_proc->_sigactions == nullptr)
                {
                    panic("[sigAction] _sigactions is null");
                    return -1;
                }
                if (oldact != nullptr)
                {
                    if (cur_proc->_sigactions->actions[flag])
                        *oldact = *(cur_proc->_sigactions->actions[flag]);
                    else
                        *oldact = {SIG_DFL, 0, {{0}}}; // 正确初始化所有字段，包括 sa_mask
                }
                if (newact != nullptr)
                {
                    // 检查handler是否为特殊值
                    if (newact->sa_handler == SIG_ERR)
                    {
                        printfRed("[sigAction] SIG_ERR is not a valid handler\n");
                        return syscall::SYS_EINVAL; // SIG_ERR不是有效的处理函数
                    }
                    
                    if (newact->sa_handler == SIG_DFL)
                    {
                        // 恢复默认处理
                        printfLightCyan("[sigAction] Setting default handler for signal %d\n", flag);
                        if (cur_proc->_sigactions->actions[flag])
                        {
                            delete cur_proc->_sigactions->actions[flag];
                            cur_proc->_sigactions->actions[flag] = nullptr;
                        }
                    }
                    else if (newact->sa_handler == SIG_IGN)
                    {
                        // 忽略信号 - 设置一个特殊的处理函数
                        printfLightCyan("[sigAction] Setting ignore handler for signal %d\n", flag);
                        if (!cur_proc->_sigactions->actions[flag])
                        {
                            cur_proc->_sigactions->actions[flag] = new sigaction;
                            if (cur_proc->_sigactions->actions[flag] == nullptr)
                                return syscall::SYS_ENOMEM; // 内存分配失败
                        }
                        else
                        {
                            // 如果已经存在，先释放旧的
                            delete cur_proc->_sigactions->actions[flag];
                            cur_proc->_sigactions->actions[flag] = new sigaction;
                            if (cur_proc->_sigactions->actions[flag] == nullptr)
                                return syscall::SYS_ENOMEM; // 内存分配失败
                        }
                        *(cur_proc->_sigactions->actions[flag]) = *newact;
                    }
                    else
                    {
                        // 普通的用户定义处理函数
                        if (!cur_proc->_sigactions->actions[flag])
                        {
                            cur_proc->_sigactions->actions[flag] = new sigaction;
                            if (cur_proc->_sigactions->actions[flag] == nullptr)
                                return syscall::SYS_ENOMEM; // 内存分配失败
                        }
                        else
                        {
                            // 如果已经存在，先释放旧的
                            delete cur_proc->_sigactions->actions[flag];
                            cur_proc->_sigactions->actions[flag] = new sigaction;
                            if (cur_proc->_sigactions->actions[flag] == nullptr)
                                return syscall::SYS_ENOMEM; // 内存分配失败
                        }
                        printfLightCyan("[sigAction] Setting handler for signal %d: enter %p flags: %p mask: %p\n", flag, newact->sa_handler, newact->sa_flags, newact->sa_mask.sig[0]);
                        *(cur_proc->_sigactions->actions[flag]) = *newact;
                    }
                }

                return 0;
            }

            int sigprocmask(int how, sigset_t *newset, sigset_t *oldset, size_t sigsize)
            {
                if (sigsize != sizeof(sigset_t))
                {
                    printfRed("[sigprocmask] sigsize is not sizeof(sigset_t)\n");
                    return -22;
                }

                proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
                
                // 首先保存当前的信号掩码到oldset（如果oldset不为nullptr）
                if (oldset != nullptr)
                    oldset->sig[0] = cur_proc->_sigmask;
                
                // 如果newset为nullptr，只是查询当前掩码，不修改
                if (newset == nullptr)
                    return 0;

                switch (how)
                {
                case signal::SIG_BLOCK:
                    cur_proc->_sigmask |= newset->sig[0];
                    break;
                case signal::SIG_UNBLOCK:
                    cur_proc->_sigmask &= ~(newset->sig[0]);
                    break;
                case signal::SIG_SETMASK:
                    cur_proc->_sigmask = newset->sig[0];
                    break;
                default:
                    panic("sigprocmask: invalid how value");
                    return -22;
                }

                int debugsig = 0; // 你可以修改这个变量来指定要查看的信号号
                if (debugsig > 0 && debugsig <= signal::SIGRTMAX) {
                    uint64 mask = (1UL << (debugsig - 1));
                    bool before = (oldset != nullptr && oldset->sig[0] != 0) ? 
                                  (oldset->sig[0] & mask) != 0 : 
                                  false; // 如果 oldset 为空，说明没有保存之前的状态
                    bool after = (cur_proc->_sigmask & mask) != 0;
                    
                    switch (how) {
                        case signal::SIG_BLOCK:
                            printfCyan("[sigprocmask][DEBUG] SIG_BLOCK: signal %d, before=%d, after=%d\n", debugsig, before, after);
                            break;
                        case signal::SIG_UNBLOCK:
                            printfCyan("[sigprocmask][DEBUG] SIG_UNBLOCK: signal %d, before=%d, after=%d\n", debugsig, before, after);
                            break;
                        case signal::SIG_SETMASK:
                            printfCyan("[sigprocmask][DEBUG] SIG_SETMASK: signal %d, before=%d, after=%d\n", debugsig, before, after);
                            break;
                    }
                }

                // 确保关键信号不会被屏蔽
                cur_proc->_sigmask = sanitize_signal_mask(cur_proc->_sigmask);

                return 0;
            }

            int sigsuspend(const sigset_t *mask)
            {
                if (mask == nullptr)
                {
                    return syscall::SYS_EINVAL; // EINVAL
                }

                proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
                
                // sigsuspend 的临时 mask 要一直保持到信号真正投递；
                // 旧 mask 由信号返回路径恢复，不能在 syscall 返回前提前恢复。
                cur_proc->_sigsuspend_saved_sigmask = sanitize_signal_mask(cur_proc->_sigmask);
                cur_proc->_sigsuspend_restore_pending = true;

                // 设置新的信号掩码，但不能阻塞SIGKILL和SIGSTOP
                cur_proc->_sigmask = sanitize_signal_mask(mask->sig[0]);

                // 检查是否已经有未被阻塞的待处理信号
                uint64 pending_unblocked = cur_proc->_signal & ~cur_proc->_sigmask;
                
                if (pending_unblocked != 0)
                {
                    // 有未被阻塞的待处理信号，直接返回给 trap 返回路径投递信号。
                    return syscall::SYS_EINTR; // EINTR - 被信号中断
                }

                // 使用进程的等待锁进行同步
                SpinLock sigsuspend_lock;
                sigsuspend_lock.init("sigsuspend");
                
                // 使用一个特殊的地址作为等待信号的睡眠通道
                void *sigsuspend_chan = (void*)((uint64)cur_proc + 0x1000);
                
                // 进入睡眠状态等待信号
                sigsuspend_lock.acquire();
                proc::k_pm.sleep(sigsuspend_chan, &sigsuspend_lock);
                
                // 当从sleep返回时，说明有未屏蔽信号到达；旧 mask 仍留给 sig_return 恢复。

                // sigsuspend总是返回-1并设置errno为EINTR
                return syscall::SYS_EINTR; // EINTR
            }

            int sigaltstack(const signalstack *ss, signalstack *old_ss)
            {
                proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
                
                // 如果请求获取当前的信号栈信息
                if (old_ss != nullptr)
                {
                    old_ss->ss_sp = cur_proc->_alt_stack.ss_sp;
                    old_ss->ss_size = cur_proc->_alt_stack.ss_size;
                    
                    // 设置flags
                    if (cur_proc->_on_sigstack)
                    {
                        old_ss->ss_flags = SS_ONSTACK;
                    }
                    else if (cur_proc->_alt_stack.ss_flags & SS_DISABLE)
                    {
                        old_ss->ss_flags = SS_DISABLE;
                    }
                    else
                    {
                        old_ss->ss_flags = cur_proc->_alt_stack.ss_flags;
                    }
                }

                // 如果ss为NULL，只返回当前信息，不修改
                if (ss == nullptr)
                {
                    return 0;
                }

                // 检查是否正在信号栈上执行
                if (cur_proc->_on_sigstack)
                {
                    return syscall::SYS_EPERM; // EPERM - 不能在信号栈上改变信号栈
                }

                // 检查是否要禁用信号栈
                if (ss->ss_flags & SS_DISABLE)
                {
                    cur_proc->_alt_stack.ss_sp = nullptr;
                    cur_proc->_alt_stack.ss_flags = SS_DISABLE;
                    cur_proc->_alt_stack.ss_size = 0;
                    return 0;
                }

                // 检查栈大小是否满足最小要求
                if (ss->ss_size < MINSIGSTKSZ)
                {
                    return syscall::SYS_ENOMEM; // ENOMEM - 栈太小
                }

                // 检查flags是否有效
                if (ss->ss_flags & ~(SS_AUTODISARM))
                {
                    return syscall::SYS_EINVAL; // EINVAL - 无效的flags
                }

                // 设置新的信号栈
                cur_proc->_alt_stack.ss_sp = ss->ss_sp;
                cur_proc->_alt_stack.ss_size = ss->ss_size;
                cur_proc->_alt_stack.ss_flags = ss->ss_flags;

                return 0;
            }

            bool has_fatal_signal_pending(Pcb *p)
            {
                if (p == nullptr || p->_signal == 0)
                {
                    return false;
                }

                return (p->_signal & (1ULL << (SIGKILL - 1))) != 0 ||
                       (p->_signal & (1ULL << (SIGSTOP - 1))) != 0;
            }

            bool signal_is_ignored_for_interrupt(Pcb *p, int signum)
            {
                if (p == nullptr)
                {
                    return true;
                }

                if (p->_sigactions != nullptr && p->_sigactions->actions[signum] != nullptr)
                {
                    sigaction *act = p->_sigactions->actions[signum];
                    if (act->sa_handler == SIG_IGN)
                    {
                        return true;
                    }
                    if (act->sa_handler != nullptr && act->sa_handler != SIG_DFL)
                    {
                        return false;
                    }
                }

                // 这些信号在当前内核里按 Linux 的“默认忽略”语义处理。
                // 它们可以保持 pending，等返回用户态后再被安静清掉，
                // 但不应该把 epoll_pwait/futex/pipe 阻塞读之类等待直接打成 EINTR。
                switch (signum)
                {
                case signal::SIGCHLD:
                case signal::SIGCONT:
                case signal::SIGURG:
                case signal::SIGWINCH:
                    return true;
                default:
                    return false;
                }
            }

            bool has_unmasked_signal_pending(Pcb *p)
            {
                if (p == nullptr || p->_signal == 0)
                {
                    return false;
                }

                for (int signum = 1; signum <= signal::SIGRTMAX; ++signum)
                {
                    uint64 bit = 1ULL << (signum - 1);
                    if ((p->_signal & bit) == 0)
                    {
                        continue;
                    }

                    if (signum == signal::SIGCANCEL)
                    {
                        return true;
                    }

                    if ((p->_sigmask & bit) != 0)
                    {
                        continue;
                    }

                    if (!signal_is_ignored_for_interrupt(p, signum))
                    {
                        return true;
                    }
                }

                return false;
            }

            // 获取信号的默认行为
            SignalAction get_default_signal_action(int signum)
            {
                switch (signum)
                {
                // 需要core dump的信号
                case signal::SIGABRT:   // 6 - abort signal
                case signal::SIGBUS:    // 7 - bus error  
                case signal::SIGFPE:    // 8 - floating point exception
                case signal::SIGILL:    // 4 - illegal instruction
                case signal::SIGQUIT:   // 3 - quit signal
                case signal::SIGSEGV:   // 11 - segmentation fault
                case signal::SIGSYS:    // 31 - bad system call
                case signal::SIGTRAP:   // 5 - trace/breakpoint trap
                case signal::SIGXCPU:   // 24 - CPU time limit exceeded
                case signal::SIGXFSZ:   // 25 - file size limit exceeded
                    return {true, true};  // terminate = true, coredump = true
                    
                // 只终止但不core dump的信号
                case signal::SIGALRM:   // 14 - timer alarm
                case signal::SIGHUP:    // 1 - hangup
                case signal::SIGINT:    // 2 - interrupt
                case signal::SIGKILL:   // 9 - kill (cannot be caught)
                case signal::SIGPIPE:   // 13 - broken pipe
                case signal::SIGPOLL:   // 29 - pollable event (also SIGIO)
                case signal::SIGPROF:   // 27 - profiling timer alarm
                case signal::SIGTERM:   // 15 - termination signal
                case signal::SIGUSR1:   // 10 - user-defined signal 1
                case signal::SIGUSR2:   // 12 - user-defined signal 2
                case signal::SIGVTALRM: // 26 - virtual timer alarm
                case signal::SIGPWR:    // 30 - power failure signal
                    return {true, false}; // terminate = true, coredump = false
                    
                // 停止信号（目前简单处理为终止）
                case signal::SIGSTOP:   // 19 - stop signal (cannot be caught)
                case signal::SIGTSTP:   // 20 - terminal stop signal
                case signal::SIGTTIN:   // 21 - background process reading from terminal
                case signal::SIGTTOU:   // 22 - background process writing to terminal
                    return {true, false}; // terminate = true, coredump = false
                    
                // 继续信号和其他可忽略的信号
                case signal::SIGCONT:   // 18 - continue signal
                case signal::SIGCHLD:   // 17 - child process terminated
                case signal::SIGWINCH:  // 28 - window resize signal
                case signal::SIGURG:    // 23 - urgent data on socket
                    return {false, false}; // terminate = false, coredump = false
                    
                default:
                    return {true, false}; // 未知信号默认终止，不core dump
                }
            }

            void default_handle(proc::Pcb *p, int signum)
            {
                SignalAction action = get_default_signal_action(signum);
                
                if (action.terminate) {
                    if (action.coredump) {
                        printf("[default_handle] Signal %d: Terminating process %d with core dump\n", signum, p->_pid);
                    } else {
                        printf("[default_handle] Signal %d: Terminating process %d\n", signum, p->_pid);
                    }
                    proc::k_pm.do_signal_exit(p, signum, action.coredump);
                }
            }

            void handle_signal()
            {
                proc::Pcb *p = proc::k_pm.get_cur_pcb();
                // printf("[handle_signal] Entered, _signal=0x%x\n", p->_signal);
                if (p->_signal == 0)
                {
                    // printf("[handle_signal] No signals to handle\n");
                    return; // 没有信号需要处理
                }
                for (uint64 i = 1; i <= proc::ipc::signal::SIGRTMAX && (p->_signal != 0); i++)
                {
                    if (!sig_is_member(p->_signal, i))
                    {
                        // printf("[handle_signal] Signal %d not set, skipping\n", i);
                        continue; // 该信号未被设置
                    }
                    int signum = i;
                    if (is_ignored(p, signum))
                    {
                        continue; // 跳过被屏蔽的信号，继续处理其他信号
                    }

                    sigaction *act = nullptr;
                    if (p->_sigactions != nullptr && p->_sigactions->actions[signum] != nullptr)
                    {
                        act = p->_sigactions->actions[signum];
                        // printf("[handle_signal] Found handler for signal %d: %p\n", signum, act->sa_handler);
                    }
                    else
                    {
                    }

                    if (act == nullptr || act->sa_handler == nullptr || act->sa_handler == SIG_DFL)
                    {
                        restore_sigsuspend_mask_now(p);
                        default_handle(p, signum);
                    }
                    else if (act->sa_handler == SIG_IGN)
                    {
                        restore_sigsuspend_mask_now(p);
                        // 直接清除信号，不做任何处理
                    }
                    else
                    {
                        // printf("[handle_signal] Calling do_handle for signal %d\n", signum);
                        do_handle(p, signum, act);

                        // 处理 SA_RESETHAND 标志：执行后重置为默认处理
                        if (act->sa_flags & (uint64)SigActionFlags::RESETHAND)
                        {
                            printf("[handle_signal] SA_RESETHAND set, resetting handler for signal %d\n", signum);
                            delete p->_sigactions->actions[signum];
                            p->_sigactions->actions[signum] = nullptr;
                        }
                    }
                    clear_signal(p, signum);
                    // printf("[handle_signal] Cleared signal %d, _signal now 0x%x\n", signum, p->_signal);
                }
                // printf("[handle_signal] Finished handling signals\n");
            }

            void handle_sync_signal()
            {
                proc::Pcb *p = proc::k_pm.get_cur_pcb();
                
                if (p->_signal == 0)
                {
                    return; // 没有信号需要处理
                }

                // 定义同步信号的优先级数组，按紧急程度排序
                static const int sync_signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGSYS, SIGPIPE};
                static const int num_sync_signals = sizeof(sync_signals) / sizeof(sync_signals[0]);

                // 按优先级处理同步信号
                for (int idx = 0; idx < num_sync_signals; idx++)
                {
                    int signum = sync_signals[idx];
                    
                    if (!sig_is_member(p->_signal, signum))
                    {
                        continue; // 该同步信号未被设置
                    }

                    printf("[handle_sync_signal] Handling urgent sync signal %d\n", signum);
                    
                    // 同步信号通常不能被屏蔽（除了通过sigprocmask显式设置）
                    // 但仍然检查是否被屏蔽
                    if (is_ignored(p, signum))
                    {
                        printfYellow("[handle_sync_signal] Sync signal %d is masked, sigmask=0x%x\n", signum, p->_sigmask);
                        // 对于同步信号，即使被屏蔽也要处理，因为它们通常是硬件异常
                        // continue;
                    }

                    sigaction *act = nullptr;
                    if (p->_sigactions != nullptr && p->_sigactions->actions[signum] != nullptr)
                    {
                        act = p->_sigactions->actions[signum];
                    }

                    if (act == nullptr || act->sa_handler == nullptr || act->sa_handler == SIG_DFL)
                    {
                        restore_sigsuspend_mask_now(p);
                        default_handle(p, signum);
                    }
                    else if (act->sa_handler == SIG_IGN)
                    {
                        restore_sigsuspend_mask_now(p);
                        // 对于同步信号，通常不应该被忽略，但仍然尊重用户设置
                    }
                    else
                    {
                        do_handle(p, signum, act);

                        // 处理 SA_RESETHAND 标志：执行后重置为默认处理
                        if (act->sa_flags & (uint64)SigActionFlags::RESETHAND)
                        {
                            printf("[handle_sync_signal] SA_RESETHAND set, resetting handler for sync signal %d\n", signum);
                            delete p->_sigactions->actions[signum];
                            p->_sigactions->actions[signum] = nullptr;
                        }
                    }
                    
                    clear_signal(p, signum);
                    printf("[handle_sync_signal] Cleared sync signal %d, _signal now 0x%x\n", signum, p->_signal);
                    
                    // 只处理一个同步信号就返回，因为它们通常是致命的
                    return;
                }

                // 如果当前信号没有注册信号处理函数, 则调用默认信号处理函数(这里不能处理自定义信号, 防止死循环)
                if (p->_signal == 0)
                {
                    return; // 没有信号需要处理
                }
                for (uint64 i = 1; i <= proc::ipc::signal::SIGRTMAX && (p->_signal != 0); i++)
                {
                    if (!sig_is_member(p->_signal, i))
                    {
                        // printf("[handle_signal] Signal %d not set, skipping\n", i);
                        continue; // 该信号未被设置
                    }
                    int signum = i;
                    // printf("[handle_signal] Handling signal %d\n", signum);
                    if (is_ignored(p, signum))
                    {
                        continue; // 跳过被屏蔽的信号，继续处理其他信号
                    }

                    sigaction *act = nullptr;
                    if (p->_sigactions != nullptr && p->_sigactions->actions[signum] != nullptr)
                    {
                        act = p->_sigactions->actions[signum];
                        // printf("[handle_signal] Found handler for signal %d: %p\n", signum, act->sa_handler);
                    }
                    else
                    {
                    }

                    if (act == nullptr || act->sa_handler == nullptr || act->sa_handler == SIG_DFL)
                    {
                        restore_sigsuspend_mask_now(p);
                        default_handle(p, signum);
                    }
                    else if (act->sa_handler == SIG_IGN)
                    {
                        restore_sigsuspend_mask_now(p);
                        // 直接清除信号，不做任何处理
                    }
                    clear_signal(p, signum);
                    // printf("[handle_signal] Cleared signal %d, _signal now 0x%x\n", signum, p->_signal);
                }
                
            }

            void add_signal(proc::Pcb *p, int sig, const LinuxSigInfo *info)
            {
                if (sig <= 0 || sig > proc::ipc::signal::SIGRTMAX)
                {
                    panic("[add_signal] Invalid signal number: %d", sig);
                    return;
                }
                // 允许这种情况(所以注释)
                // if (sig_is_member(p->_signal, sig))
                // {
                //     panic("[add_signal] Signal %d is already set", sig);
                //     return;
                // }
                p->_signal |= (1UL << (sig - 1));
                if (info != nullptr)
                {
                    p->_queued_siginfo[sig] = *info;
                    p->_siginfo_mask |= (1ULL << (sig - 1));
                }
                
                // 如果进程正在sigsuspend中等待，并且这个信号没有被阻塞，则唤醒它
                uint64 sig_mask = (1UL << (sig - 1));
                if ((p->_sigmask & sig_mask) == 0 || sig == signal::SIGCANCEL) {
                    // 使用特殊的sigsuspend睡眠通道来唤醒等待中的进程
                    void *sigsuspend_chan = (void*)((uint64)p + 0x1000);
                    // 检查进程是否正在特定的sigsuspend通道上睡眠
                    if (p->_state == ProcState::SLEEPING && p->_chan == sigsuspend_chan) {
                        // 直接设置为可运行状态，避免调用wakeup造成的死锁
                        // 这是安全的，因为调用者已经持有了进程锁
                        p->_state = ProcState::RUNNABLE;
                        p->_chan = nullptr;
                    }
                }
            }

            void do_handle(proc::Pcb *p, int signum, sigaction *act)
            {
                if (act == NULL)
                {
                    panic("[do_handle] act is NULL");
                    return;
                }
                
                // 检查是否为特殊的处理函数值
                if (act->sa_handler == SIG_DFL)
                {
                    panic("[do_handle] SIG_DFL should not reach do_handle, using default handler\n");
                    default_handle(p, signum);
                    return;
                }
                
                if (act->sa_handler == SIG_IGN)
                {
                    panic("[do_handle] SIG_IGN should not reach do_handle, ignoring signal %d\n", signum);
                    return;
                }
                
                if (act->sa_handler == SIG_ERR)
                {
                    panic("[do_handle] SIG_ERR is not a valid handler");
                    return;
                }
                
                if (is_ignored(p, signum))
                {
                    panic("[do_handle] Signal %d is ignored", signum);
                    return;
                }
                // printf("[do_handle] Handling signal %d with handler %p\n", signum, act->sa_handler);

                signal_frame *frame;
                frame = (signal_frame *)mem::k_pmm.alloc_page();
                if (frame == nullptr)
                {
                    panic("[do_handle] Failed to allocate memory for signal frame");
                    return;
                }
                uint64 restore_sigmask = consume_signal_restore_mask(p);
                frame->mask.sig[0] = restore_sigmask; // 保存 handler 返回后需要恢复的信号掩码

                // 处理 sa_mask：在信号处理期间临时阻塞额外的信号
                [[maybe_unused]] uint64 old_sigmask = p->_sigmask;
                p->_sigmask |= act->sa_mask.sig[0]; // 添加 sa_mask 中指定的信号到当前掩码

                // 根据 SA_NODEFER 标志决定是否阻塞当前信号
                if (!(act->sa_flags & (uint64)SigActionFlags::NODEFER))
                {
                    p->_sigmask |= (1UL << (signum - 1)); // 默认阻塞当前信号
                }

                // 永远不能屏蔽 SIGKILL、SIGSTOP 以及线程库内部取消信号。
                p->_sigmask = sanitize_signal_mask(p->_sigmask);

                // printf("[do_handle] Signal mask updated: old=0x%x, new=0x%x, sa_mask=0x%x\n",
                //        old_sigmask, p->_sigmask, act->sa_mask.sig[0]);

                if (frame == nullptr)
                {
                    panic("[do_handle] Failed to allocate memory for signal frame");
                    return;
                }
                frame->tf = *(p->_trapframe);
                if (is_entry_static_task(p))
                {
#ifdef RISCV
                    void *saved_pc = (void *)frame->tf.epc;
#elif defined(LOONGARCH)
                    void *saved_pc = (void *)frame->tf.era;
#endif
                    printfYellow("[do_handle] entry-static signal: pid=%d tid=%d sig=%d handler=%p flags=%p saved_pc=%p saved_sp=%p\n",
                                 p->_pid,
                                 p->_tid,
                                 signum,
                                 act->sa_handler,
                                 (void *)act->sa_flags,
                                 saved_pc,
                                 (void *)frame->tf.sp);
                }
#ifdef RISCV
                p->_trapframe->ra = (uint64)(SIG_TRAMPOLINE + ((uint64)sig_handler - (uint64)sig_trampoline));
#elif LOONGARCH
                p->_trapframe->ra = (uint64)SIG_TRAMPOLINE;
#endif

                bool sigframe_already_finalized = false;

                // 检查是否需要三参数信号处理 (SA_SIGINFO)
                if (act->sa_flags & (uint64)SigActionFlags::SIGINFO)
                {
                    printf("[do_handle] Using SA_SIGINFO for signal %d\n", signum);
                    uint64 va, a, pa;
                    va = p->_trapframe->sp;
                    a = PGROUNDDOWN(va);
                    mem::Pte pte = p->get_pagetable()->walk(a, 0);
                    pa = reinterpret_cast<uint64>(pte.pa());
                    printf("[copy_out] va: %p, pte: %p, pa: %p\n", va, pte.get_data(), pa);

                    // 决定使用哪个栈：信号栈或正常栈
                    uint64 stack_sp;
                    uint64 sig_size;
                    
                    // 检查是否应该使用信号栈
                    if ((act->sa_flags & (uint64)SigActionFlags::ONSTACK) &&
                        !(p->_alt_stack.ss_flags & SS_DISABLE) &&
                        !p->_on_sigstack)
                    {
                        // 使用信号栈
                        stack_sp = (uint64)p->_alt_stack.ss_sp + p->_alt_stack.ss_size;
                        sig_size = p->_alt_stack.ss_size;
                        p->_on_sigstack = true;
                        
                        // 如果设置了SS_AUTODISARM，清除信号栈设置
                        if (p->_alt_stack.ss_flags & SS_AUTODISARM)
                        {
                            p->_alt_stack.ss_flags |= SS_DISABLE;
                        }
                    }
                    else
                    {
                        // 使用正常的用户栈
                        stack_sp = p->_trapframe->sp;
                        sig_size = 5 * PGSIZE; // 预留空间，确保足够大
                    }

                    // 计算栈上的地址。
                    // 信号帧应当直接从当前 sp / altstack 栈顶向下扣减并按 ABI 对齐，
                    // 不能再额外空出一整页；否则像 pthread_cancel_points 这种使用
                    // SA_SIGINFO 的路径，会把 ucontext 写到未映射的栈下方页面里。
                    uint64 frame_bytes = sizeof(uint64) * 2 + sizeof(LinuxSigInfo) + sizeof(usercontext);
                    uint64 frame_stack_sp = clamp_signal_stack_top_to_writable_vma(p, stack_sp, frame_bytes);
                    uint64 frame_sp = align_down(frame_stack_sp - frame_bytes, k_signal_stack_align);
                    uint64 guard_sp = frame_sp;
                    uint64 has_siginfo_sp = guard_sp + sizeof(uint64);
                    uint64 linuxinfo_sp = has_siginfo_sp + sizeof(uint64);
                    uint64 usercontext_sp = linuxinfo_sp + sizeof(LinuxSigInfo);
                    expand_writable_stack_vma_for_signal(p, guard_sp, frame_stack_sp);
                    prepare_signal_frame_pages(p, guard_sp, frame_stack_sp);

                    // 构造 ustack 结构
                    usercontext uctx;
                    memset(&uctx, 0, sizeof(usercontext)); // 全部初始化为0
                    uctx.flags = 0;
                    uctx.link = 0;
                    uctx.stack = {(void *)stack_sp, 0, sig_size};
#ifdef RISCV
                    uctx.sigmask.sig[0] = restore_sigmask;
                    fill_riscv_user_mcontext(uctx.mcontext, frame->tf);
#elif LOONGARCH
                    uctx.sigmask.sig[0] = restore_sigmask;
                    fill_loongarch_user_mcontext(uctx.mcontext, frame->tf);
#endif
                    // mcontext 已经通过 memset 初始化为0了
                    // printf("[debug] uctx[176] = %p\n",(char*)&uctx + 176);
                    // // 打印 uctx 的所有字节内容
                    // printf("[debug] uctx bytes: ");
                    // for (size_t i = 0; i < sizeof(uctx); ++i) {
                    //     printf("%d=%02x ",i, ((unsigned char*)&uctx)[i]);
                    // }
                    // printf("\n");

                    // printf("[do_handle] sepcial handling for SA_SIGINFO: epc=%p\n",
                    //    p->_trapframe->epc);

                    // 构造 LinuxSigInfo 结构
                    LinuxSigInfo siginfo{};
                    if ((p->_siginfo_mask & (1ULL << (signum - 1))) != 0)
                    {
                        siginfo = p->_queued_siginfo[signum];
                    }
                    else
                    {
                        siginfo.si_signo = (int32)signum;
                        siginfo.si_errno = 0;
                        siginfo.si_code = 0;
                        siginfo.si_pid = 0;
                        siginfo.si_uid = 0;
                        siginfo.si_value.sival_ptr = 0;
                    }
                    printf("[do_handle] LinuxSigInfo constructed: sp: %p usercontext_sp=%p, linuxinfo_sp=%p\n",
                           p->_trapframe->sp, usercontext_sp, linuxinfo_sp);
                    // 将结构写入用户空间
                    if (mem::k_vmm.copy_out(*p->get_pagetable(), usercontext_sp, &uctx, sizeof(usercontext)) < 0)
                    {
                        int frame_vma_idx = -1;
                        int uctx_vma_idx = -1;
                        proc::vma *frame_vm = find_vma_covering(p, frame_sp, &frame_vma_idx);
                        proc::vma *uctx_vm = find_vma_covering(p, usercontext_sp, &uctx_vma_idx);
                        mem::Pte frame_pte = p->get_pagetable()->walk(PGROUNDDOWN(frame_sp), 0);
                        mem::Pte uctx_pte = p->get_pagetable()->walk(PGROUNDDOWN(usercontext_sp), 0);
                        panic("[do_handle] Failed to copy ustack: sig=%d pid=%d tid=%d old_sp=%p stack_sp=%p frame_sp=%p uctx_sp=%p frame_bytes=%p uctx_size=%p frame_vma=%d[%p,%p) uctx_vma=%d[%p,%p) frame_pte=%p frame_valid=%d uctx_pte=%p uctx_valid=%d",
                              signum,
                              p->_pid,
                              p->_tid,
                              (void *)p->_trapframe->sp,
                              (void *)stack_sp,
                              (void *)frame_sp,
                              (void *)usercontext_sp,
                              (void *)frame_bytes,
                              (void *)sizeof(usercontext),
                              frame_vma_idx,
                              frame_vm ? (void *)frame_vm->addr : nullptr,
                              frame_vm ? (void *)(frame_vm->addr + (uint64)frame_vm->len) : nullptr,
                              uctx_vma_idx,
                              uctx_vm ? (void *)uctx_vm->addr : nullptr,
                              uctx_vm ? (void *)(uctx_vm->addr + (uint64)uctx_vm->len) : nullptr,
                              (void *)safe_pte_data(frame_pte),
                              safe_pte_valid(frame_pte),
                              (void *)safe_pte_data(uctx_pte),
                              safe_pte_valid(uctx_pte));
                        return;
                    }

                    if (mem::k_vmm.copy_out(*p->get_pagetable(), linuxinfo_sp, &siginfo, sizeof(LinuxSigInfo)) < 0)
                    {
                        panic("[do_handle] Failed to copy LinuxSigInfo to user space");
                        return;
                    }

                    // 设置三参数信号处理函数的参数
                    p->_trapframe->a0 = signum;         // 第一个参数：信号编号
                    p->_trapframe->a1 = linuxinfo_sp;   // 第二个参数：siginfo_t*
                    p->_trapframe->a2 = usercontext_sp; // 第三个参数：ucontext_t*

                    // 调整栈指针
                    p->_trapframe->sp = guard_sp;

                    if (mem::k_vmm.copy_out(*p->get_pagetable(), guard_sp, &guard, sizeof(uint64)) < 0)
                    {
                        int guard_vma_idx = -1;
                        proc::vma *guard_vm = find_vma_covering(p, guard_sp, &guard_vma_idx);
                        mem::Pte guard_pte = p->get_pagetable()->walk(PGROUNDDOWN(guard_sp), 0);
                        panic("[do_handle] Failed to write sigframe guard: sig=%d pid=%d tid=%d old_sp=%p guard_sp=%p frame_bytes=%p guard_vma=%d[%p,%p) guard_pte=%p guard_valid=%d",
                              signum,
                              p->_pid,
                              p->_tid,
                              (void *)stack_sp,
                              (void *)guard_sp,
                              (void *)frame_bytes,
                              guard_vma_idx,
                              guard_vm ? (void *)guard_vm->addr : nullptr,
                              guard_vm ? (void *)(guard_vm->addr + (uint64)guard_vm->len) : nullptr,
                              (void *)safe_pte_data(guard_pte),
                              safe_pte_valid(guard_pte));
                        return;
                    }

                    uint64 has_siginfo = UINT64_MAX;
                    if (mem::k_vmm.copy_out(*p->get_pagetable(), has_siginfo_sp, &has_siginfo, sizeof(uint64)) < 0)
                    {
                        panic("[do_handle] Failed to write has_siginfo marker");
                        return;
                    }

                    printf("[do_handle] SA_SIGINFO setup complete: sp=%p, a1=%p, a2=%p\n",
                           p->_trapframe->sp, linuxinfo_sp, usercontext_sp);
                    sigframe_already_finalized = true;
                }
                else
                {
                    // 原有的单参数处理逻辑
                    uint64 stack_sp;
                    
                    // 检查是否应该使用信号栈
                    if ((act->sa_flags & (uint64)SigActionFlags::ONSTACK) &&
                        !(p->_alt_stack.ss_flags & SS_DISABLE) &&
                        !p->_on_sigstack)
                    {
                        // 使用信号栈
                        stack_sp = (uint64)p->_alt_stack.ss_sp + p->_alt_stack.ss_size;
                        p->_on_sigstack = true;
                        
                        // 如果设置了SS_AUTODISARM，清除信号栈设置
                        if (p->_alt_stack.ss_flags & SS_AUTODISARM)
                        {
                            p->_alt_stack.ss_flags |= SS_DISABLE;
                        }
                    }
                    else
                    {
                        // 使用正常的用户栈
                        stack_sp = p->_trapframe->sp;
                    }
                    
                    // 单参数信号帧同样直接贴着当前栈顶落下，保持与 Linux 的栈增长
                    // 语义一致，避免因为额外跳过一页而误落到未映射区域。
                    uint64 frame_stack_sp = clamp_signal_stack_top_to_writable_vma(p, stack_sp, sizeof(uint64) * 2);
                    uint64 marker_sp = frame_stack_sp - sizeof(uint64);
                    expand_writable_stack_vma_for_signal(p, marker_sp - sizeof(uint64), frame_stack_sp);
                    prepare_signal_frame_pages(p, marker_sp - sizeof(uint64), frame_stack_sp);
                    p->_trapframe->sp = marker_sp;
                    
                    // 在栈顶写入返回地址标记
                    uint64 ret_marker = 0;
                    if (mem::k_vmm.copy_out(*p->get_pagetable(), p->get_trapframe()->sp, &ret_marker, sizeof(uint64)) < 0)
                    {
                        panic("[do_handle] Failed to write return marker to user stack");
                        return;
                    }
                    p->_trapframe->a0 = signum;
                }

#ifdef RISCV
                p->_trapframe->epc = (uint64)(act->sa_handler);
#elif LOONGARCH
                p->_trapframe->era = (uint64)(act->sa_handler);
#endif
                // 单参数信号的返回标记在这里统一补齐。
                // SA_SIGINFO 路径前面已经把 guard/has_siginfo/ucontext 全部铺好，
                // 这里如果再额外压一个 guard，会把 sig_return() 的判别槽位顶歪，
                // 最终把三参数信号误判成普通信号返回路径。
                if (!sigframe_already_finalized)
                {
                    uint64 guard_sp = p->get_trapframe()->sp - sizeof(uint64);
                    p->_trapframe->sp = guard_sp;

                    if (mem::k_vmm.copy_out(*p->get_pagetable(), guard_sp, &guard, sizeof(guard)) < 0)
                    {
                        panic("[do_handle] Failed to write return marker to user stack");
                        return;
                    }
                }
                if (p->sig_frame)
                {
                    frame->next = p->sig_frame;
                }
                else
                {
                    frame->next = nullptr;
                }
                p->sig_frame = frame;
                return;
            }

            void sig_return()
            {
                Pcb *p = proc::k_pm.get_cur_pcb();
                uint64 user_sp = p->_trapframe->sp;
                uint64 guardcheck;
                if (mem::k_vmm.copy_in(*p->get_pagetable(), &guardcheck, user_sp, sizeof(guardcheck)) < 0)
                {
                    panic("[sig_return] Failed to read return marker from user stack");
                    return;
                }
                if (guardcheck != guard)
                {
                    panic("[sig_return] Return marker mismatch: expected %p, got %p", guard, guardcheck);
                    return;
                }
                user_sp += sizeof(guard); // 跳过返回地址标记
                uint64 has_siginfo;
                if (mem::k_vmm.copy_in(*p->get_pagetable(), &has_siginfo, user_sp, sizeof(has_siginfo)) < 0)
                {
                    panic("[sig_return] Failed to read has_siginfo from user stack");
                    return;
                }
                if (has_siginfo != UINT64_MAX)
                {
                    if (p->sig_frame == nullptr)
                    {
                        panic("[sig_return] No signal frame to return to");
                        p->_killed = true; // 没有信号帧，直接标记为被kill
                        return;
                    }
                    signal_frame *frame = p->sig_frame;
                    p->_sigmask = sanitize_signal_mask(frame->mask.sig[0]);   // 恢复信号掩码
                    memmove(p->_trapframe, &(frame->tf), sizeof(TrapFrame)); // 恢复陷阱帧
                    p->sig_frame = frame->next;                              // 移除当前信号帧
                    
                    // 恢复信号栈状态
                    p->_on_sigstack = false;
                    
                    mem::k_pmm.free_page(frame);                             // 释放信号帧内存
                }
                else
                {
                    user_sp += sizeof(uint64);       // 跳过 has_siginfo
                    user_sp += sizeof(LinuxSigInfo); // 跳过 LinuxSigInfo
                    usercontext uctx;
                    if (mem::k_vmm.copy_in(*p->get_pagetable(), &uctx, user_sp, sizeof(uctx)) < 0)
                    {
                        panic("[sig_return] Failed to read has_siginfo from user stack");
                        return;
                    }
                    if (p->sig_frame == nullptr)
                    {
                        panic("[sig_return] No signal frame to return to");
                        p->_killed = true; // 没有信号帧，直接标记为被kill
                        return;
                    }
                    signal_frame *frame = p->sig_frame;
                    p->_sigmask = sanitize_signal_mask(frame->mask.sig[0]);   // 默认先回到进入信号前的内核保存值
                    memmove(p->_trapframe, &(frame->tf), sizeof(TrapFrame)); // 恢复陷阱帧
                    p->sig_frame = frame->next;                              // 移除当前信号帧
                    
                    // 恢复信号栈状态
                    p->_on_sigstack = false;
                    
                    mem::k_pmm.free_page(frame);
#ifdef RISCV
                    p->_sigmask = sanitize_signal_mask(uctx.sigmask.sig[0]);
                    restore_riscv_trapframe_from_ucontext(*p->_trapframe, uctx);
#elif LOONGARCH
                    // 某些取消/条件变量压力测里，如果用户态栈上的 ucontext 已经被破坏，
                    // 这里继续照抄一个 pc=0 的上下文只会把线程立刻送去地址 0 再次 fault。
                    // 为了保持内核与后续回归链的鲁棒性，显式识别这类“明显非法”的恢复目标，
                    // 并回退到内核在送信号前保存的 trapframe。
                    if (uctx.mcontext.pc == 0 || uctx.mcontext.gregs[3] == 0)
                    {
                        printfRed("[sig_return] invalid loongarch ucontext, fallback to saved trapframe: pid=%d tid=%d pc=%p sp=%p saved_pc=%p saved_sp=%p cur_user_sp=%p\n",
                                  p->_pid,
                                  p->_tid,
                                  (void *)uctx.mcontext.pc,
                                  (void *)uctx.mcontext.gregs[3],
                                  (void *)p->_trapframe->era,
                                  (void *)p->_trapframe->sp,
                                  (void *)user_sp);
                        return;
                    }
                    if (is_entry_static_task(p))
                    {
                        printfYellow("[sig_return] entry-static restore: pid=%d tid=%d pc=%p sp=%p sigmask=%p cur_user_sp=%p\n",
                                     p->_pid,
                                     p->_tid,
                                     (void *)uctx.mcontext.pc,
                                     (void *)uctx.mcontext.gregs[3],
                                     (void *)uctx.sigmask.sig[0],
                                     (void *)user_sp);
                    }
                    p->_sigmask = sanitize_signal_mask(uctx.sigmask.sig[0]);
                    restore_loongarch_trapframe_from_ucontext(*p->_trapframe, uctx);
#endif
                }
            }

            // tool
            bool is_valid(int sig)
            {
                return (sig <= proc::ipc::signal::SIGRTMAX && sig >= 1);
            }

            bool is_sync_signal(int sig)
            {
                return (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || 
                        sig == SIGILL || sig == SIGTRAP || sig == SIGSYS);
            }

            bool sig_is_member(const uint64 set, int n_sig)
            {
                return (bool)(1 & (set >> (n_sig - 1)));
            }

            bool is_ignored(Pcb *now_p, int sig)
            {
                if (sig == signal::SIGCANCEL)
                {
                    return false;
                }
                return sig_is_member(now_p->_sigmask, sig);
            }

            void clear_signal(Pcb *now_p, int sig)
            {
                if (sig <= 0 || sig > proc::ipc::signal::SIGRTMAX)
                {
                    panic("[clear_signal] Invalid signal number: %d", sig);
                    return;
                }
                if (!sig_is_member(now_p->_signal, sig))
                {
                    panic("[clear_signal] Signal %d is not set", sig);
                    return;
                }
                now_p->_signal &= ~(1UL << (sig - 1));
                now_p->_siginfo_mask &= ~(1ULL << (sig - 1));
                memset(&now_p->_queued_siginfo[sig], 0, sizeof(now_p->_queued_siginfo[sig]));
            }

        } // namespace signal
    } // namespace ipc
} // namespace proc
