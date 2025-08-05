#include "signal.hh"
#include "proc_manager.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "klib.hh"

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
                if (flag <= 0 || flag > signal::SIGRTMAX || flag == signal::SIGKILL || flag == signal::SIGSTOP)
                    return -1;
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
                        *oldact = {nullptr, 0, 0, {{0}}}; // 正确初始化所有字段，包括 sa_mask
                }
                if (newact != nullptr)
                {
                    if (!cur_proc->_sigactions->actions[flag])
                    {
                        cur_proc->_sigactions->actions[flag] = new sigaction;
                        if (cur_proc->_sigactions->actions[flag] == nullptr)
                            return -1; // 内存分配失败
                    }
                    else
                    {
                        // 如果已经存在，先释放旧的
                        delete cur_proc->_sigactions->actions[flag];
                        cur_proc->_sigactions->actions[flag] = new sigaction;
                        if (cur_proc->_sigactions->actions[flag] == nullptr)
                            return -1; // 内存分配失败
                    }
                    // printf("[sigAction] Setting handler for signal %d: enter %p flags: %p mask: %p\n", flag, newact->sa_handler, newact->sa_flags, newact->sa_mask.sig[0]);
                    *(cur_proc->_sigactions->actions[flag]) = *newact;
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

                int debugsig = proc::ipc::signal::SIGSEGV; // 你可以修改这个变量来指定要查看的信号号
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
                uint64 unmaskable_signals = (1UL << (signal::SIGKILL - 1)) |
                                            (1UL << (signal::SIGSTOP - 1));
                cur_proc->_sigmask &= ~unmaskable_signals;

                return 0;
            }

            void default_handle(proc::Pcb *p, int signum)
            {
                switch (signum)
                {
                case signal::SIGKILL:
                    p->_killed = true;
                    break;
                case signal::SIGSEGV:
                    // SIGSEGV 默认行为：终止进程并生成核心转储
                    printf("[default_handle] SIGSEGV: Segmentation fault, terminating process %d\n", p->_pid);
                    p->_killed = true;
                    break;
                case signal::SIGCHLD:
                    panic("[default_handle] SIGCHLD not implemented");
                    break;
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
                    printf("[handle_signal] Handling signal %d\n", signum);
                    if (is_ignored(p, signum))
                    {
                        printf("[handle_signal] Signal %d is ignored, sigmask=0x%x\n", signum, p->_sigmask);
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
                        printf("[handle_signal] No user handler for signal %d\n", signum);
                    }

                    if (act == nullptr || act->sa_handler == nullptr)
                    {
                        printf("[handle_signal] Signal %d has no handler, using default handler\n", signum);
                        default_handle(p, signum);
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

            void add_signal(proc::Pcb *p, int sig)
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
            }

            void do_handle(proc::Pcb *p, int signum, sigaction *act)
            {
                if (act == NULL)
                {
                    panic("[do_handle] act is NULL");
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
                frame->mask.sig[0] = p->_sigmask; // 保存当前信号掩码

                // 处理 sa_mask：在信号处理期间临时阻塞额外的信号
                [[maybe_unused]] uint64 old_sigmask = p->_sigmask;
                p->_sigmask |= act->sa_mask.sig[0]; // 添加 sa_mask 中指定的信号到当前掩码

                // 根据 SA_NODEFER 标志决定是否阻塞当前信号
                if (!(act->sa_flags & (uint64)SigActionFlags::NODEFER))
                {
                    p->_sigmask |= (1UL << (signum - 1)); // 默认阻塞当前信号
                }

                // 永远不能屏蔽 SIGKILL, SIGSTOP
                p->_sigmask &= ~((1UL << (signal::SIGKILL - 1)) |
                                 (1UL << (signal::SIGSTOP - 1))
                                );

                // printf("[do_handle] Signal mask updated: old=0x%x, new=0x%x, sa_mask=0x%x\n",
                //        old_sigmask, p->_sigmask, act->sa_mask.sig[0]);

                if (frame == nullptr)
                {
                    panic("[do_handle] Failed to allocate memory for signal frame");
                    return;
                }
                frame->tf = *(p->_trapframe);
#ifdef RISCV
                p->_trapframe->ra = (uint64)(SIG_TRAMPOLINE + ((uint64)sig_handler - (uint64)sig_trampoline));
#elif LOONGARCH
                p->_trapframe->ra = (uint64)SIG_TRAMPOLINE;
                printf("sig: %p\n", SIG_TRAMPOLINE);
#endif

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

                    // 计算用户栈上的地址
                    uint64 usercontext_sp = p->_trapframe->sp - PGSIZE - sizeof(usercontext);
                    uint64 linuxinfo_sp = usercontext_sp - sizeof(LinuxSigInfo);
                    uint64 sig_size = 5 * PGSIZE; // 预留空间，确保足够大(TODO: 需要根据实际情况调整大小)

                    // 构造 ustack 结构
                    usercontext uctx;
                    memset(&uctx, 0, sizeof(usercontext)); // 全部初始化为0
                    uctx.flags = 0;
                    uctx.link = 0;
                    uctx.stack = {linuxinfo_sp, 0, sig_size};
                    uctx.sigmask = p->_sigmask;
#ifdef RISCV
                    uctx.mcontext.gp.x[17] = p->_trapframe->epc; // epc(TODO)
#elif LOONGARCH
                    uctx.mcontext.gp.x[17] = p->_trapframe->era;
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
                    LinuxSigInfo siginfo = {
                        .si_signo = (uint32)signum,
                        .si_errno = 0,
                        .si_code = 0,
                        ._pad = {0},
                        ._align = 0};
                    printf("[do_handle] LinuxSigInfo constructed: sp: %p usercontext_sp=%p, linuxinfo_sp=%p\n",
                           p->_trapframe->sp, usercontext_sp, linuxinfo_sp);
                    // 将结构写入用户空间
                    if (mem::k_vmm.copy_out(*p->get_pagetable(), usercontext_sp, &uctx, sizeof(usercontext)) < 0)
                    {
                        panic("[do_handle] Failed to copy ustack to user space");
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
                    p->_trapframe->sp = linuxinfo_sp;
                    p->_trapframe->sp -= sizeof(uint64); // 为返回地址预留空间

                    // 在栈顶写入返回地址标记
                    uint64 ret_marker = UINT64_MAX;
                    if (mem::k_vmm.copy_out(*p->get_pagetable(), p->get_trapframe()->sp, &ret_marker, sizeof(uint64)) < 0)
                    {
                        panic("[do_handle] Failed to write return marker to user stack");
                        return;
                    }

                    printf("[do_handle] SA_SIGINFO setup complete: sp=%p, a1=%p, a2=%p\n",
                           p->_trapframe->sp, linuxinfo_sp, usercontext_sp);
                }
                else
                {
                    // 原有的单参数处理逻辑
                    p->_trapframe->sp -= PGSIZE;
                    p->_trapframe->a0 = signum;
                }

#ifdef RISCV
                p->_trapframe->epc = (uint64)(act->sa_handler);
#elif LOONGARCH
                p->_trapframe->era = (uint64)(act->sa_handler);
#endif
                // 哨兵
                p->_trapframe->sp -= sizeof(uint64); // 为返回地址预留空间

                if (mem::k_vmm.copy_out(*p->get_pagetable(), p->get_trapframe()->sp, &guard, sizeof(guard)) < 0)
                {
                    panic("[do_handle] Failed to write return marker to user stack");
                    return;
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
                    p->_sigmask = frame->mask.sig[0];                        // 恢复信号掩码
                    memmove(p->_trapframe, &(frame->tf), sizeof(TrapFrame)); // 恢复陷阱帧
                    p->sig_frame = frame->next;                              // 移除当前信号帧
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
                    p->_sigmask = frame->mask.sig[0];                        // 恢复信号掩码
                    memmove(p->_trapframe, &(frame->tf), sizeof(TrapFrame)); // 恢复陷阱帧
                    p->sig_frame = frame->next;                              // 移除当前信号帧
                    mem::k_pmm.free_page(frame);
#ifdef RISCV
                    p->_trapframe->epc = uctx.mcontext.gp.x[17];
#elif LOONGARCH
                    p->_trapframe->era = uctx.mcontext.gp.x[17];
#endif
                }
            }

            // tool
            bool is_valid(int sig)
            {
                return (sig <= proc::ipc::signal::SIGRTMAX && sig >= 1);
            }

            bool sig_is_member(const uint64 set, int n_sig)
            {
                return (bool)(1 & (set >> (n_sig - 1)));
            }

            bool is_ignored(Pcb *now_p, int sig)
            {
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
            }

        } // namespace signal
    } // namespace ipc
} // namespace proc
