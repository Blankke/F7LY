#include "proc_manager.hh"
#include "futex.hh"

#include "physical_memory_manager.hh"
#include "printer.hh"
#include "proc/posix_timers.hh"
#include "proc/scheduler.hh"
#include "fs/vfs/fs.hh"
#include "sys/syscall_defs.hh"

namespace proc
{
    namespace
    {
        void free_signal_frame_list(Pcb *victim)
        {
            while (victim->sig_frame != nullptr)
            {
                ipc::signal::signal_frame *next_frame = victim->sig_frame->next;
                mem::k_pmm.free_page(victim->sig_frame);
                victim->sig_frame = next_frame;
            }
            victim->sig_frame = nullptr;
        }

        void force_reclaim_task_runtime(Pcb *victim)
        {
            cleanup_posix_timers_for_owner(victim);

            victim->_killed = 1;
            victim->_exiting = true;
            victim->_parent_exit_signal = 0;
            victim->_has_child_tasks = false;
            victim->_vfork_parent = nullptr;
            victim->_chan.store(nullptr, eastl::memory_order_release);

            victim->cleanup_memory_manager();
            victim->cleanup_ofile();
            victim->cleanup_sighand();
            free_signal_frame_list(victim);

            futex_remove_waiter(victim);
            victim->_clear_tid_addr = 0;
            victim->_robust_list = nullptr;
            victim->_robust_list_user_addr = 0;
            k_scheduler.set_task_state(*victim, ProcState::ZOMBIE);
            victim->_exiting = false;
        }
    }

    int ProcessManager::soft_reboot_phase()
    {
#if NUMCPU != 1
        return syscall::SYS_EOPNOTSUPP;
#endif

        Pcb *current = get_cur_pcb();
        if (current == nullptr || current != _init_proc || current->_pid != current->_tid)
        {
            return syscall::SYS_EPERM;
        }

        printfYellow("[soft_reboot_phase] start, preserve init pid=%d tid=%d\n",
                     current->_pid, current->_tid);

        int sync_ret = vfs_sync_all();
        if (sync_ret < 0)
        {
            printfRed("[soft_reboot_phase] pre-sync failed: %d\n", sync_ret);
            return sync_ret;
        }

        int reclaimed = 0;
        for (Pcb *victim = k_proc_pool; victim < &k_proc_pool[num_process]; ++victim)
        {
            if (victim == current || victim->_state == ProcState::UNUSED)
            {
                continue;
            }

            if (victim->_state == ProcState::USED)
            {
                freeproc_creation_failed(victim);
                ++reclaimed;
                continue;
            }

            force_reclaim_task_runtime(victim);
            freeproc(victim);
            ++reclaimed;
        }

        current->_has_child_tasks = false;

        int reclaim_ret = vfs_reclaim_global_caches_for_soft_reset();
        if (reclaim_ret < 0)
        {
            printfRed("[soft_reboot_phase] global cache reclaim failed: %d\n", reclaim_ret);
            return reclaim_ret;
        }

        printfYellow("[soft_reboot_phase] finish, reclaimed_tasks=%d\n", reclaimed);
        return 0;
    }
}
