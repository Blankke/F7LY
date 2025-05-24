#include "signal.hh"
#include "proc_manager.hh"

namespace proc
{
    namespace ipc
    {
        namespace signal
        {

            int sigAction(int flag, sigaction *newact, sigaction *oldact)
            {
                if (flag <= 0 || flag >= SIGRTMAX || flag == SIGKILL || flag == SIGQUIT)
                    return -1;
                proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
                if (oldact != nullptr)
                    *oldact = *(cur_proc->_sigactions[flag - 1]);
                if (newact != nullptr)
                    cur_proc->_sigactions[flag - 1] = newact;
                return 0;
            }

            int sigprocmask(int how, sigset_t *newset, sigset_t *oldset, size_t sigsize)
            {
                if (sigsize != sizeof(sigset_t))
                    return -22;

                proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
                if (oldset != nullptr)
                    oldset->sig[0] = cur_proc->sigmask;
                if (newset == nullptr)
                    return 0;

                switch (how)
                {
                case SIG_BLOCK:
                    cur_proc->sigmask |= newset->sig[0];
                    break;
                case SIG_UNBLOCK:
                    cur_proc->sigmask &= ~(newset->sig[0]);
                    break;
                case SIG_SETMASK:
                    cur_proc->sigmask = newset->sig[0];
                    break;
                default:
                    return -22;
                }

                // 永远不能屏蔽这两个信号
                cur_proc->sigmask &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
                return 0;
            }

        } // namespace signal
    } // namespace ipc
} // namespace proc
