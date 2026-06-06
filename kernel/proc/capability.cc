#include "capability.hh"

#include "klib.hh"
#include "proc.hh"

namespace proc
{
    CapabilityManager k_capability;

    int CapabilityManager::user_data_words(uint32 version) const
    {
        if (version == k_version_1)
        {
            return 1;
        }
        if (version == k_version_2 || version == k_version_3)
        {
            return k_capability_word_count;
        }
        return 0;
    }

    bool CapabilityManager::is_valid_capability(uint32 capability) const
    {
        return capability <= k_capability_max;
    }

    bool CapabilityManager::has_bit(const uint32 words[k_capability_word_count],
                                    uint32 capability) const
    {
        if (!is_valid_capability(capability))
        {
            return false;
        }
        return (words[capability / 32] & (1U << (capability % 32))) != 0;
    }

    bool CapabilityManager::has_effective(const Pcb *task, uint32 capability) const
    {
        return task != nullptr && has_bit(task->_cap_effective, capability);
    }

    void CapabilityManager::clear_all(Pcb &task) const
    {
        memset(task._cap_effective, 0, sizeof(task._cap_effective));
        memset(task._cap_permitted, 0, sizeof(task._cap_permitted));
        memset(task._cap_inheritable, 0, sizeof(task._cap_inheritable));
        memset(task._cap_ambient, 0, sizeof(task._cap_ambient));
        memset(task._cap_bounding, 0, sizeof(task._cap_bounding));
    }

    void CapabilityManager::init_root(Pcb &task) const
    {
        task._cap_effective[0] = 0xffffffffU;
        task._cap_effective[1] = 0x1ffU;
        task._cap_permitted[0] = 0xffffffffU;
        task._cap_permitted[1] = 0x1ffU;
        task._cap_inheritable[0] = 0;
        task._cap_inheritable[1] = 0;
        task._cap_ambient[0] = 0;
        task._cap_ambient[1] = 0;
        task._cap_bounding[0] = 0xffffffffU;
        task._cap_bounding[1] = 0x1ffU;
    }

    void CapabilityManager::update_after_uid_change(Pcb *task,
                                                    uint32 old_uid,
                                                    uint32 old_euid,
                                                    uint32 old_suid) const
    {
        if (task == nullptr)
        {
            return;
        }

        const bool had_root_identity =
            old_uid == 0 || old_euid == 0 || old_suid == 0;
        const bool has_root_identity =
            task->_uid == 0 || task->_euid == 0 || task->_suid == 0;

        // 当前尚未完整接入 PR_SET_KEEPCAPS/securebits；这里集中保存默认
        // Linux 凭据规则，避免 setuid/setresuid 等调用点各自复制分支。
        if (had_root_identity && !has_root_identity)
        {
            memset(task->_cap_permitted, 0, sizeof(task->_cap_permitted));
            memset(task->_cap_effective, 0, sizeof(task->_cap_effective));
            memset(task->_cap_ambient, 0, sizeof(task->_cap_ambient));
            return;
        }
        if (old_euid == 0 && task->_euid != 0)
        {
            memset(task->_cap_effective, 0, sizeof(task->_cap_effective));
        }
        else if (old_euid != 0 && task->_euid == 0)
        {
            memcpy(task->_cap_effective,
                   task->_cap_permitted,
                   sizeof(task->_cap_effective));
        }
    }

    Pcb *CapabilityManager::find_live_task_by_pid_or_tid(int id) const
    {
        for (Pcb *task = k_proc_pool; task < &k_proc_pool[num_process]; ++task)
        {
            if (task->_state != ProcState::UNUSED &&
                (task->_pid == id || task->_tid == id))
            {
                return task;
            }
        }
        return nullptr;
    }

    bool CapabilityManager::may_inspect(const Pcb *current, const Pcb *target) const
    {
        if (current == nullptr || target == nullptr)
        {
            return false;
        }
        if (current == target || current->get_euid() == 0)
        {
            return true;
        }
        return current->get_uid() == target->get_uid() &&
               current->get_euid() == target->get_euid();
    }
}
