#include "signal.hh"
#include "proc_manager.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "devs/spinlock.hh"
#include "sys/syscall_defs.hh"
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
                        *oldact = {SIG_DFL, 0, 0, {{0}}}; // 正确初始化所有字段，包括 sa_mask
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
                uint64 unmaskable_signals = (1UL << (signal::SIGKILL - 1)) |
                                            (1UL << (signal::SIGSTOP - 1));
                cur_proc->_sigmask &= ~unmaskable_signals;

                return 0;
            }

            int sigsuspend(const sigset_t *mask)
            {
                if (mask == nullptr)
                {
                    return syscall::SYS_EINVAL; // EINVAL
                }

                proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
                
                // 保存当前的信号掩码
                uint64 old_sigmask = cur_proc->_sigmask;

                // 设置新的信号掩码，但不能阻塞SIGKILL和SIGSTOP
                uint64 unmaskable_signals = (1UL << (signal::SIGKILL - 1)) |
                                            (1UL << (signal::SIGSTOP - 1));
                cur_proc->_sigmask = mask->sig[0] & ~unmaskable_signals;

                // 检查是否已经有未被阻塞的待处理信号
                uint64 pending_unblocked = cur_proc->_signal & ~cur_proc->_sigmask;
                
                if (pending_unblocked != 0)
                {
                    // 有未被阻塞的待处理信号，恢复原掩码并立即返回
                    cur_proc->_sigmask = old_sigmask;
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
                
                // 当从sleep返回时，说明有信号到达
                // 恢复原来的信号掩码
                cur_proc->_sigmask = old_sigmask;

                // sigsuspend总是返回-1并设置errno为EINTR
                return syscall::SYS_EINTR; // EINTR
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
                } else {
                    // 信号被忽略
                    const char* signal_name = "";
                    switch (signum) {
                        case signal::SIGCONT: signal_name = "SIGCONT"; break;
                        case signal::SIGCHLD: signal_name = "SIGCHLD"; break;
                        case signal::SIGWINCH: signal_name = "SIGWINCH"; break;
                        case signal::SIGURG: signal_name = "SIGURG"; break;
                        case signal::SIGPWR: signal_name = "SIGPWR"; break;
                        default: signal_name = "Unknown"; break;
                    }
                    printf("[default_handle] %s (%d): Ignored for process %d\n", signal_name, signum, p->_pid);
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

                    if (act == nullptr || act->sa_handler == nullptr || act->sa_handler == SIG_DFL)
                    {
                        printf("[handle_signal] Signal %d has no handler or SIG_DFL, using default handler\n", signum);
                        default_handle(p, signum);
                    }
                    else if (act->sa_handler == SIG_IGN)
                    {
                        printf("[handle_signal] Signal %d is ignored (SIG_IGN)\n", signum);
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
                        printf("[handle_sync_signal] Found handler for sync signal %d: %p\n", signum, act->sa_handler);
                    }

                    if (act == nullptr || act->sa_handler == nullptr || act->sa_handler == SIG_DFL)
                    {
                        printf("[handle_sync_signal] Sync signal %d has no handler or SIG_DFL, using default handler\n", signum);
                        default_handle(p, signum);
                    }
                    else if (act->sa_handler == SIG_IGN)
                    {
                        printf("[handle_sync_signal] Sync signal %d is ignored (SIG_IGN)\n", signum);
                        // 对于同步信号，通常不应该被忽略，但仍然尊重用户设置
                    }
                    else
                    {
                        printf("[handle_sync_signal] Calling do_handle for sync signal %d\n", signum);
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

                    if (act == nullptr || act->sa_handler == nullptr || act->sa_handler == SIG_DFL)
                    {
                        printf("[handle_signal] Signal %d has no handler or SIG_DFL, using default handler\n", signum);
                        default_handle(p, signum);
                    }
                    else if (act->sa_handler == SIG_IGN)
                    {
                        printf("[handle_signal] Signal %d is ignored (SIG_IGN)\n", signum);
                        // 直接清除信号，不做任何处理
                    }
                    clear_signal(p, signum);
                    // printf("[handle_signal] Cleared signal %d, _signal now 0x%x\n", signum, p->_signal);
                }
                
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
                
                // 如果进程正在sigsuspend中等待，并且这个信号没有被阻塞，则唤醒它
                uint64 sig_mask = (1UL << (sig - 1));
                if ((p->_sigmask & sig_mask) == 0) { // 信号没有被阻塞
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
