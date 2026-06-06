#pragma once

#include "types.hh"

namespace proc
{
    class Pcb;

    constexpr int k_capability_word_count = 2;
    constexpr uint32 k_capability_max = 40;

    struct CapabilityUserHeader
    {
        uint32 version;
        int pid;
    };

    struct CapabilityUserData
    {
        uint32 effective;
        uint32 permitted;
        uint32 inheritable;
    };

    class CapabilityManager
    {
    public:
        static constexpr uint32 k_version_1 = 0x19980330;
        static constexpr uint32 k_version_2 = 0x20071026;
        static constexpr uint32 k_version_3 = 0x20080522;

        int user_data_words(uint32 version) const;
        bool is_valid_capability(uint32 capability) const;
        bool has_bit(const uint32 words[k_capability_word_count], uint32 capability) const;
        bool has_effective(const Pcb *task, uint32 capability) const;

        void clear_all(Pcb &task) const;
        void init_root(Pcb &task) const;
        void update_after_uid_change(Pcb *task,
                                     uint32 old_uid,
                                     uint32 old_euid,
                                     uint32 old_suid) const;

        Pcb *find_live_task_by_pid_or_tid(int id) const;
        bool may_inspect(const Pcb *current, const Pcb *target) const;
    };

    extern CapabilityManager k_capability;
}
