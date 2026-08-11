#include "perf_diag.hh"
#include "perf_diag_algorithms.hh"

#if F7LY_PERF_DIAG

#include "hal/cpu.hh"
#include "mem/memlayout.hh"
#include "proc/proc.hh"
#include "tm/time.hh"
#ifdef RISCV
#include "hal/riscv/sbi.hh"
#include "hal/riscv/rv_csr.hh"
#endif

namespace perfdiag
{
    namespace
    {
        constexpr uint64 k_counter_count = static_cast<uint64>(Counter::Count);
        constexpr uint64 k_initial_epoch = 1;

        struct TaggedValue
        {
            uint64 epoch;
            uint64 value;
        };

        struct FlatEntry
        {
            uint64 epoch;
            uint64 pc;
            uint64 count;
        };

        struct CallchainEntry
        {
            uint64 epoch;
            uint64 pcs[k_callchain_depth];
            uint64 count;
            uint8 depth;
        };

        struct alignas(64) PerCpuStorage
        {
            TaggedValue metrics[k_counter_count]{};
            TaggedValue syscall_count[k_syscall_slots]{};
            TaggedValue syscall_time_ticks[k_syscall_slots]{};
            FlatEntry flat[k_flat_capacity]{};
            CallchainEntry callchains[k_callchain_capacity]{};
            TaggedValue samples{};
            TaggedValue dropped_full{};
            TaggedValue invalid_pc{};
            TaggedValue user_skipped{};
            TaggedValue unwind_failed{};
            uint32 timer_divider{};
            uint64 pmu_generation{};
            uint64 pmu_counter{};
            bool pmu_cycles_known{};
            bool pmu_cycles_supported{};
            bool pmu_instructions_known{};
            bool pmu_instructions_supported{};
        };

        PerCpuStorage g_storage[NUMCPU]{};
        uint64 g_metrics_epoch = k_initial_epoch;
        uint64 g_profile_epoch = k_initial_epoch;
        uint64 g_snapshot_id = 0;
        uint64 g_profile_generation = 1;
        ProfileConfig g_profile{false, ProfileBackend::Auto, ProfileBackend::Timer,
                                ProfileEvent::CpuCycles, 100, 1000000, false};

#define F7LY_PERF_DESC(id, abi_name, metric_kind, metric_unit, aggregate_kind, metric_description) \
        {Counter::id, abi_name, MetricKind::metric_kind, metric_unit, Aggregate::aggregate_kind, metric_description},
        constexpr MetricDescriptor k_descriptors[] = {
            F7LY_PERF_METRIC_TABLE(F7LY_PERF_DESC)
        };
#undef F7LY_PERF_DESC

        extern "C" char etext[];
#ifdef LOONGARCH
        extern "C" char kernel_start[];
#else
        extern "C" char _entry[];
#endif

        struct EmbeddedSymbol
        {
            uint64 address;
            uint32 name_offset;
            uint32 reserved;
        };
        extern "C" const EmbeddedSymbol __f7ly_perf_symbols_start[];
        extern "C" const EmbeddedSymbol __f7ly_perf_symbols_end[];
        extern "C" const char __f7ly_perf_symbol_names_start[];

        uint64 current_cpu_slot()
        {
            const uint64 cpu = Cpu::current_cpu_id();
            return Cpu::is_valid_cpu_id(cpu) ? cpu : 0;
        }

        uint64 load_epoch(const uint64 *epoch)
        {
            return __atomic_load_n(epoch, __ATOMIC_ACQUIRE);
        }

        void reset_tag_if_stale(TaggedValue &slot, uint64 epoch)
        {
            if (__atomic_load_n(&slot.epoch, __ATOMIC_ACQUIRE) == epoch)
                return;
            __atomic_store_n(&slot.value, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&slot.epoch, epoch, __ATOMIC_RELEASE);
        }

        void tagged_add(TaggedValue &slot, uint64 epoch, uint64 value)
        {
            reset_tag_if_stale(slot, epoch);
            __atomic_fetch_add(&slot.value, value, __ATOMIC_RELAXED);
        }

        void tagged_max(TaggedValue &slot, uint64 epoch, uint64 value)
        {
            reset_tag_if_stale(slot, epoch);
            uint64 previous = __atomic_load_n(&slot.value, __ATOMIC_RELAXED);
            while (previous < value &&
                   !__atomic_compare_exchange_n(&slot.value, &previous, value, false,
                                                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        }

        uint64 tagged_read(const TaggedValue &slot, uint64 epoch)
        {
            if (__atomic_load_n(&slot.epoch, __ATOMIC_ACQUIRE) != epoch)
                return 0;
            const uint64 value = __atomic_load_n(&slot.value, __ATOMIC_RELAXED);
            return __atomic_load_n(&slot.epoch, __ATOMIC_ACQUIRE) == epoch ? value : 0;
        }

        bool kernel_pc_valid(uint64 pc)
        {
#ifdef LOONGARCH
            const uint64 begin = reinterpret_cast<uint64>(kernel_start);
#else
            const uint64 begin = reinterpret_cast<uint64>(_entry);
#endif
            return pc >= begin && pc < reinterpret_cast<uint64>(etext);
        }

        void record_flat(PerCpuStorage &cpu, uint64 epoch, uint64 pc)
        {
            const uint64 first = detail::hash_pc(pc) % k_flat_capacity;
            for (uint64 probe = 0; probe < k_flat_capacity; ++probe)
            {
                FlatEntry &entry = cpu.flat[(first + probe) % k_flat_capacity];
                if (entry.epoch != epoch)
                {
                    entry.pc = pc;
                    entry.count = 1;
                    __atomic_store_n(&entry.epoch, epoch, __ATOMIC_RELEASE);
                    return;
                }
                if (entry.pc == pc)
                {
                    __atomic_fetch_add(&entry.count, 1, __ATOMIC_RELAXED);
                    return;
                }
            }
            tagged_add(cpu.dropped_full, epoch, 1);
        }

        uint8 unwind(uint64 pc, uint64 frame_pointer, uint64 *pcs)
        {
            pcs[0] = pc;
            proc::Pcb *pcb = Cpu::get_cpu()->get_cur_proc();
            if (pcb == nullptr)
                return 1;
            const uint64 bottom = pcb->get_kstack();
            const uint64 top = bottom + KSTACK_SIZE;
            uint8 depth = 1;
            uint64 fp = frame_pointer;
            while (depth < k_callchain_depth)
            {
                if (fp < 16 || !detail::stack_word_valid(fp - 16, bottom, top) ||
                    !detail::stack_word_valid(fp - 8, bottom, top))
                    return depth == 1 ? 0 : depth;
                const uint64 next_fp = *reinterpret_cast<const uint64 *>(fp - 16);
                const uint64 return_pc = *reinterpret_cast<const uint64 *>(fp - 8);
                if (!kernel_pc_valid(return_pc))
                    return depth;
                pcs[depth++] = return_pc;
                if (!detail::next_frame_valid(fp, next_fp, top))
                    return depth;
                fp = next_fp;
            }
            return depth;
        }

        void record_callchain(PerCpuStorage &cpu, uint64 epoch, const uint64 *pcs, uint8 depth)
        {
            const uint64 first = detail::hash_chain(pcs, depth) % k_callchain_capacity;
            for (uint64 probe = 0; probe < k_callchain_capacity; ++probe)
            {
                CallchainEntry &entry = cpu.callchains[(first + probe) % k_callchain_capacity];
                if (entry.epoch != epoch)
                {
                    for (uint8 i = 0; i < depth; ++i)
                        entry.pcs[i] = pcs[i];
                    for (uint8 i = depth; i < k_callchain_depth; ++i)
                        entry.pcs[i] = 0;
                    entry.depth = depth;
                    entry.count = 1;
                    __atomic_store_n(&entry.epoch, epoch, __ATOMIC_RELEASE);
                    return;
                }
                bool same = entry.depth == depth;
                for (uint8 i = 0; same && i < depth; ++i)
                    same = entry.pcs[i] == pcs[i];
                if (same)
                {
                    __atomic_fetch_add(&entry.count, 1, __ATOMIC_RELAXED);
                    return;
                }
            }
            tagged_add(cpu.dropped_full, epoch, 1);
        }

#ifdef RISCV
        constexpr uint64 k_sbi_ext_base = 0x10;
        constexpr uint64 k_sbi_base_probe_extension = 3;
        constexpr uint64 k_sbi_ext_pmu = 0x504d55;
        constexpr uint64 k_sbi_pmu_num_counters = 0;
        constexpr uint64 k_sbi_pmu_counter_get_info = 1;
        constexpr uint64 k_sbi_pmu_counter_cfg_match = 2;
        constexpr uint64 k_sbi_pmu_counter_start = 3;
        constexpr uint64 k_sbi_pmu_counter_stop = 4;
        constexpr uint64 k_sbi_pmu_cfg_clear_value = 1ULL << 1;
        constexpr uint64 k_sbi_pmu_cfg_set_uinh = 1ULL << 5;
        constexpr uint64 k_sbi_pmu_start_set_init_value = 1ULL << 0;
        constexpr uint64 k_sbi_pmu_stop_reset = 1ULL << 0;
        constexpr uint64 k_sbi_pmu_hw_event_type = 0;
        constexpr uint64 k_sbi_pmu_event_cycles = 1;
        constexpr uint64 k_sbi_pmu_event_instructions = 2;
        constexpr uint64 k_sie_lcofie = 1ULL << 13;

        bool arch_pmu_probe_event(ProfileEvent event)
        {
            // 仅有 SBI PMU 扩展还不足以采样；LCOFIE 必须可写，才能可靠收到
            // Sscofpmf 溢出中断。QEMU/OpenSBI 常见的“可计数但不可采样”会在此回退。
            const uint64 old_sie = riscv::r_sie();
            riscv::w_sie(old_sie | k_sie_lcofie);
            const bool overflow_interrupt = (riscv::r_sie() & k_sie_lcofie) != 0;
            riscv::w_sie(old_sie);
            if (!overflow_interrupt)
                return false;
            const sbiret probe = new_sbi_call(k_sbi_ext_base, k_sbi_base_probe_extension,
                                              k_sbi_ext_pmu, 0, 0, 0, 0, 0);
            if (static_cast<int64>(probe.error) != 0 || probe.value == 0)
                return false;
            const sbiret counters = new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_num_counters,
                                                 0, 0, 0, 0, 0, 0);
            if (static_cast<int64>(counters.error) != 0 || counters.value == 0)
                return false;
            const uint64 count = counters.value > 64 ? 64 : counters.value;
            const uint64 mask = count == 64 ? static_cast<uint64>(-1) : ((1ULL << count) - 1);
            const uint64 event_code = event == ProfileEvent::Instructions
                                          ? k_sbi_pmu_event_instructions : k_sbi_pmu_event_cycles;
            const sbiret configured = new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_cfg_match,
                                                   0, mask, 0, event_code, 0, 0);
            if (static_cast<int64>(configured.error) != 0 || configured.value >= count)
                return false;
            const sbiret info = new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_get_info,
                                             configured.value, 0, 0, 0, 0, 0);
            new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_stop,
                         configured.value, 1, k_sbi_pmu_stop_reset, 0, 0, 0);
            if (static_cast<int64>(info.error) != 0 || (info.value >> 63) != 0)
                return false; // firmware counter 没有硬件溢出采样能力
            const uint64 width = ((info.value >> 12) & 0x3fULL) + 1;
            return width == 64;
        }

        bool arch_pmu_configure(PerCpuStorage &cpu, ProfileEvent event, uint64 period)
        {
            const sbiret counters = new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_num_counters,
                                                 0, 0, 0, 0, 0, 0);
            if (static_cast<int64>(counters.error) != 0 || counters.value == 0)
                return false;
            const uint64 count = counters.value > 64 ? 64 : counters.value;
            const uint64 mask = count == 64 ? static_cast<uint64>(-1) : ((1ULL << count) - 1);
            const uint64 event_code = event == ProfileEvent::Instructions
                                          ? k_sbi_pmu_event_instructions : k_sbi_pmu_event_cycles;
            const uint64 event_idx = (k_sbi_pmu_hw_event_type << 16) | event_code;
            const sbiret configured = new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_cfg_match,
                                                   0, mask,
                                                   k_sbi_pmu_cfg_clear_value | k_sbi_pmu_cfg_set_uinh,
                                                   event_idx, 0, 0);
            if (static_cast<int64>(configured.error) != 0 || configured.value >= count)
                return false;
            cpu.pmu_counter = configured.value;
            const sbiret started = new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_start,
                                                cpu.pmu_counter, 1,
                                                k_sbi_pmu_start_set_init_value,
                                                0 - period, 0, 0);
            if (static_cast<int64>(started.error) != 0)
                return false;
            riscv::w_sie(riscv::r_sie() | k_sie_lcofie);
            return true;
        }

        void arch_pmu_reload(PerCpuStorage &cpu, uint64 period)
        {
            new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_stop,
                         cpu.pmu_counter, 1, 0, 0, 0, 0);
            // 按 Sscofpmf 顺序先停表，再清 LCOFI pending，最后重装初值。
            riscv::w_sip(riscv::r_sip() & ~k_sie_lcofie);
            new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_start,
                         cpu.pmu_counter, 1, k_sbi_pmu_start_set_init_value,
                         0 - period, 0, 0);
        }

        void arch_pmu_disable(PerCpuStorage &cpu)
        {
            new_sbi_call(k_sbi_ext_pmu, k_sbi_pmu_counter_stop,
                         cpu.pmu_counter, 1, 0, 0, 0, 0);
            riscv::w_sip(riscv::r_sip() & ~k_sie_lcofie);
            riscv::w_sie(riscv::r_sie() & ~k_sie_lcofie);
        }
#elif defined(LOONGARCH)
        constexpr uint64 k_loongarch_cpucfg6_pmp = 1ULL << 0;
        constexpr uint64 k_loongarch_perf_plv0 = 1ULL << 16;
        constexpr uint64 k_loongarch_perf_ie = 1ULL << 20;
        constexpr uint64 k_loongarch_perf_overflow = 1ULL << 63;

        uint64 loongarch_cpucfg6()
        {
            uint64 index = 6;
            uint64 value = 0;
            asm volatile("cpucfg %0, %1" : "=r"(value) : "r"(index));
            return value;
        }

        void loongarch_write_perfctrl0(uint64 value)
        {
            asm volatile("csrwr %0, 0x200" : : "r"(value) : "memory");
        }

        void loongarch_write_perfcntr0(uint64 value)
        {
            asm volatile("csrwr %0, 0x201" : : "r"(value) : "memory");
        }

        bool arch_pmu_probe_event(ProfileEvent)
        {
            const uint64 config = loongarch_cpucfg6();
            const uint64 counter_bits = ((config >> 8) & 0x3fULL) + 1;
            return (config & k_loongarch_cpucfg6_pmp) != 0 && counter_bits >= 64;
        }

        bool arch_pmu_configure(PerCpuStorage &cpu, ProfileEvent event, uint64 period)
        {
            if (!arch_pmu_probe_event(event) || period >= k_loongarch_perf_overflow)
                return false;
            cpu.pmu_counter = 0;
            loongarch_write_perfctrl0(0);
            loongarch_write_perfcntr0(k_loongarch_perf_overflow - period);
            const uint64 event_code = event == ProfileEvent::Instructions ? 0x01 : 0x00;
            // 只置 PLV0，明确禁止用户态 PLV3 计数。
            loongarch_write_perfctrl0(event_code | k_loongarch_perf_plv0 | k_loongarch_perf_ie);
            return true;
        }

        void arch_pmu_reload(PerCpuStorage &, uint64 period)
        {
            loongarch_write_perfctrl0(0);
            loongarch_write_perfcntr0(k_loongarch_perf_overflow - period);
            const uint64 event_code = g_profile.event == ProfileEvent::Instructions ? 0x01 : 0x00;
            loongarch_write_perfctrl0(event_code | k_loongarch_perf_plv0 | k_loongarch_perf_ie);
        }

        void arch_pmu_disable(PerCpuStorage &)
        {
            loongarch_write_perfctrl0(0);
        }
#endif

        void record_kernel_sample(PerCpuStorage &cpu, uint64 epoch, uint64 pc, uint64 frame_pointer)
        {
            if (!kernel_pc_valid(pc))
            {
                tagged_add(cpu.invalid_pc, epoch, 1);
                return;
            }
            record_flat(cpu, epoch, pc);
            tagged_add(cpu.samples, epoch, 1);
            if (g_profile.callchain)
            {
                uint64 pcs[k_callchain_depth]{};
                const uint8 depth = unwind(pc, frame_pointer, pcs);
                if (depth == 0)
                    tagged_add(cpu.unwind_failed, epoch, 1);
                else
                    record_callchain(cpu, epoch, pcs, depth);
            }
        }
    }

    uint64 count_set_bits(uint64 value)
    {
        volatile uint64 remaining = value;
        uint64 count = 0;
        while (remaining != 0)
        {
            count += remaining & 1ULL;
            remaining >>= 1;
        }
        return count;
    }

    uint64 timestamp() { return tmm::get_hw_time_stamp(); }
    uint64 timebase_hz() { return tmm::get_main_frequence(); }

    void add(Counter counter, uint64 value)
    {
        const uint64 index = static_cast<uint64>(counter);
        if (index < k_counter_count)
            tagged_add(g_storage[current_cpu_slot()].metrics[index], load_epoch(&g_metrics_epoch), value);
    }

    void set_max(Counter counter, uint64 value)
    {
        const uint64 index = static_cast<uint64>(counter);
        if (index < k_counter_count)
            tagged_max(g_storage[current_cpu_slot()].metrics[index], load_epoch(&g_metrics_epoch), value);
    }

    void record_syscall(uint64 number, uint64 elapsed_time_ticks)
    {
        const uint64 epoch = load_epoch(&g_metrics_epoch);
        PerCpuStorage &cpu = g_storage[current_cpu_slot()];
        tagged_add(cpu.metrics[static_cast<uint64>(Counter::Syscall)], epoch, 1);
        tagged_add(cpu.metrics[static_cast<uint64>(Counter::SyscallTimeTicks)], epoch, elapsed_time_ticks);
        if (number < k_syscall_slots)
        {
            tagged_add(cpu.syscall_count[number], epoch, 1);
            tagged_add(cpu.syscall_time_ticks[number], epoch, elapsed_time_ticks);
        }
    }

    uint64 metrics_epoch() { return load_epoch(&g_metrics_epoch); }
    uint64 profile_epoch() { return load_epoch(&g_profile_epoch); }
    uint64 next_snapshot_id() { return __atomic_add_fetch(&g_snapshot_id, 1, __ATOMIC_RELAXED); }
    void reset_metrics() { __atomic_add_fetch(&g_metrics_epoch, 1, __ATOMIC_ACQ_REL); }
    void reset_profile() { __atomic_add_fetch(&g_profile_epoch, 1, __ATOMIC_ACQ_REL); }
    void reset_all() { reset_metrics(); reset_profile(); }

    uint64 descriptor_count() { return sizeof(k_descriptors) / sizeof(k_descriptors[0]); }
    const MetricDescriptor &descriptor(uint64 index) { return k_descriptors[index]; }

    uint64 counter_cpu(Counter counter, uint64 cpu)
    {
        if (cpu >= NUMCPU || static_cast<uint64>(counter) >= k_counter_count)
            return 0;
        return tagged_read(g_storage[cpu].metrics[static_cast<uint64>(counter)], metrics_epoch());
    }

    uint64 syscall_count_cpu(uint64 number, uint64 cpu)
    {
        return number < k_syscall_slots && cpu < NUMCPU
                   ? tagged_read(g_storage[cpu].syscall_count[number], metrics_epoch()) : 0;
    }

    uint64 syscall_time_ticks_cpu(uint64 number, uint64 cpu)
    {
        return number < k_syscall_slots && cpu < NUMCPU
                   ? tagged_read(g_storage[cpu].syscall_time_ticks[number], metrics_epoch()) : 0;
    }

    ProfileConfig profile_config()
    {
        ProfileConfig copy = g_profile;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return copy;
    }

    bool timer_frequency_valid(uint32 frequency)
    {
        return frequency == 1 || frequency == 2 || frequency == 5 || frequency == 10 ||
               frequency == 20 || frequency == 25 || frequency == 50 || frequency == 100;
    }

    bool pmu_available(ProfileEvent event)
    {
        const uint64 online = Cpu::online_cpu_mask();
        for (uint64 cpu = 0; cpu < NUMCPU; ++cpu)
        {
            if ((online & (1ULL << cpu)) == 0)
                continue;
            const bool known = event == ProfileEvent::Instructions
                                   ? __atomic_load_n(&g_storage[cpu].pmu_instructions_known, __ATOMIC_ACQUIRE)
                                   : __atomic_load_n(&g_storage[cpu].pmu_cycles_known, __ATOMIC_ACQUIRE);
            const bool supported = event == ProfileEvent::Instructions
                                       ? g_storage[cpu].pmu_instructions_supported
                                       : g_storage[cpu].pmu_cycles_supported;
            if (!known || !supported)
                return false;
        }
        return online != 0;
    }

    int profile_start(ProfileBackend backend, ProfileEvent event, uint32 frequency,
                      uint64 period, bool callchain)
    {
        if (!timer_frequency_valid(frequency) || period == 0)
            return -EINVAL;
        ProfileBackend active = backend;
        if (backend == ProfileBackend::Auto)
            active = pmu_available(event) ? ProfileBackend::Pmu : ProfileBackend::Timer;
        if (active == ProfileBackend::Pmu && !pmu_available(event))
            return -95; // EOPNOTSUPP
        g_profile = ProfileConfig{true, backend, active, event, frequency, period, callchain};
        __atomic_add_fetch(&g_profile_generation, 1, __ATOMIC_ACQ_REL);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        return 0;
    }

    void profile_stop()
    {
        __atomic_store_n(&g_profile.active, false, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_profile_generation, 1, __ATOMIC_ACQ_REL);
    }

    const char *backend_name(ProfileBackend backend)
    {
        switch (backend)
        {
        case ProfileBackend::Auto: return "auto";
        case ProfileBackend::Timer: return "timer";
        case ProfileBackend::Pmu: return "pmu";
        }
        return "unknown";
    }

    const char *event_name(ProfileEvent event)
    {
        return event == ProfileEvent::Instructions ? "instructions" : "cycles";
    }

    void on_timer_interrupt(bool from_kernel, uint64 pc, uint64 frame_pointer)
    {
        PerCpuStorage &cpu = g_storage[current_cpu_slot()];
        if (!cpu.pmu_cycles_known)
        {
            cpu.pmu_cycles_supported = arch_pmu_probe_event(ProfileEvent::CpuCycles);
            __atomic_store_n(&cpu.pmu_cycles_known, true, __ATOMIC_RELEASE);
        }
        if (!cpu.pmu_instructions_known)
        {
            cpu.pmu_instructions_supported = arch_pmu_probe_event(ProfileEvent::Instructions);
            __atomic_store_n(&cpu.pmu_instructions_known, true, __ATOMIC_RELEASE);
        }
        if (!__atomic_load_n(&g_profile.active, __ATOMIC_ACQUIRE))
        {
            if (cpu.pmu_generation != 0)
            {
                arch_pmu_disable(cpu);
                cpu.pmu_generation = 0;
            }
            return;
        }
        if (g_profile.active_backend == ProfileBackend::Pmu)
        {
            const uint64 generation = load_epoch(&g_profile_generation);
            if (cpu.pmu_generation != generation &&
                arch_pmu_configure(cpu, g_profile.event, g_profile.period))
                cpu.pmu_generation = generation;
            return;
        }
        if (cpu.pmu_generation != 0)
        {
            arch_pmu_disable(cpu);
            cpu.pmu_generation = 0;
        }
        const uint32 divider = 100 / g_profile.frequency;
        cpu.timer_divider++;
        if (cpu.timer_divider < divider)
            return;
        cpu.timer_divider = 0;
        const uint64 epoch = profile_epoch();
        if (!from_kernel)
        {
            tagged_add(cpu.user_skipped, epoch, 1);
            return;
        }
        record_kernel_sample(cpu, epoch, pc, frame_pointer);
    }

    void on_pmu_interrupt(bool from_kernel, uint64 pc, uint64 frame_pointer)
    {
        PerCpuStorage &cpu = g_storage[current_cpu_slot()];
        if (!__atomic_load_n(&g_profile.active, __ATOMIC_ACQUIRE) ||
            g_profile.active_backend != ProfileBackend::Pmu)
        {
            arch_pmu_disable(cpu);
            cpu.pmu_generation = 0;
            return;
        }
        if (cpu.pmu_generation != load_epoch(&g_profile_generation))
            return;
        const uint64 epoch = profile_epoch();
        if (from_kernel)
            record_kernel_sample(cpu, epoch, pc, frame_pointer);
        else
            tagged_add(cpu.user_skipped, epoch, 1);
        arch_pmu_reload(cpu, g_profile.period);
    }

    bool flat_sample(uint64 cpu, uint64 index, FlatSample &out)
    {
        if (cpu >= NUMCPU || index >= k_flat_capacity)
            return false;
        const FlatEntry &entry = g_storage[cpu].flat[index];
        if (__atomic_load_n(&entry.epoch, __ATOMIC_ACQUIRE) != profile_epoch())
            return false;
        out.pc = entry.pc;
        out.count = __atomic_load_n(&entry.count, __ATOMIC_RELAXED);
        return out.count != 0;
    }

    bool callchain_sample(uint64 cpu, uint64 index, CallchainSample &out)
    {
        if (cpu >= NUMCPU || index >= k_callchain_capacity)
            return false;
        const CallchainEntry &entry = g_storage[cpu].callchains[index];
        if (__atomic_load_n(&entry.epoch, __ATOMIC_ACQUIRE) != profile_epoch())
            return false;
        out.depth = entry.depth;
        out.count = __atomic_load_n(&entry.count, __ATOMIC_RELAXED);
        for (uint8 i = 0; i < k_callchain_depth; ++i)
            out.pcs[i] = entry.pcs[i];
        return out.count != 0 && out.depth != 0;
    }

#define PROFILE_VALUE_READER(function_name, member)                         \
    uint64 function_name(uint64 cpu)                                        \
    {                                                                        \
        return cpu < NUMCPU ? tagged_read(g_storage[cpu].member, profile_epoch()) : 0; \
    }
    PROFILE_VALUE_READER(profile_samples_cpu, samples)
    PROFILE_VALUE_READER(profile_dropped_full_cpu, dropped_full)
    PROFILE_VALUE_READER(profile_invalid_pc_cpu, invalid_pc)
    PROFILE_VALUE_READER(profile_user_skipped_cpu, user_skipped)
    PROFILE_VALUE_READER(profile_unwind_failed_cpu, unwind_failed)
#undef PROFILE_VALUE_READER

    uint64 symbol_count()
    {
        return static_cast<uint64>(__f7ly_perf_symbols_end - __f7ly_perf_symbols_start);
    }

    bool symbol_at(uint64 index, uint64 &start, uint64 &end, const char *&name)
    {
        const uint64 count = symbol_count();
        if (index >= count)
            return false;
        const EmbeddedSymbol &symbol = __f7ly_perf_symbols_start[index];
        start = symbol.address;
        end = index + 1 < count ? __f7ly_perf_symbols_start[index + 1].address
                                : reinterpret_cast<uint64>(etext);
        name = __f7ly_perf_symbol_names_start + symbol.name_offset;
        return true;
    }
}

#endif
