#pragma once

#include "syscall_abi.hh"
#include "timer_manager.hh"

namespace tmm
{
    namespace abi = syscall::abi;

    class KernelTimexController
    {
    public:
        // adjtimex/clock_adjtime 的状态机独立于 syscall 分发。
        // handler 只传入用户结构和权限判断结果，由控制器维护内核时间校准状态。
        bool timespec_to_ns_checked(const tmm::timespec &ts, int64_t &ns) const;
        void ns_to_timespec(int64_t ns, tmm::timespec &ts) const;
        int snapshot(abi::KernelTimexOld &timex);
        int apply(abi::KernelTimexOld &timex, bool has_privilege);

    private:
        struct State
        {
            long offset = 0;
            long freq = 0;
            long maxerror = 0;
            long esterror = 0;
            int status = 0;
            long constant = 0;
            long precision = 1;
            long tolerance = 0;
            long tick = 0;
            int tai = 0;
        };

        static constexpr unsigned int k_adj_offset = 0x0001;
        static constexpr unsigned int k_adj_frequency = 0x0002;
        static constexpr unsigned int k_adj_maxerror = 0x0004;
        static constexpr unsigned int k_adj_esterror = 0x0008;
        static constexpr unsigned int k_adj_status = 0x0010;
        static constexpr unsigned int k_adj_timeconst = 0x0020;
        static constexpr unsigned int k_adj_tai = 0x0080;
        static constexpr unsigned int k_adj_setoffset = 0x0100;
        static constexpr unsigned int k_adj_micro = 0x1000;
        static constexpr unsigned int k_adj_nano = 0x2000;
        static constexpr unsigned int k_adj_tick = 0x4000;
        static constexpr unsigned int k_adj_offset_singleshot = 0x8001;
        static constexpr unsigned int k_adj_offset_ss_read = 0xa001;

        static constexpr int k_time_ok = 0;
        static constexpr int k_time_error = 5;

        static constexpr int k_sta_pll = 0x0001;
        static constexpr int k_sta_ppsfreq = 0x0002;
        static constexpr int k_sta_ppstime = 0x0004;
        static constexpr int k_sta_fll = 0x0008;
        static constexpr int k_sta_ins = 0x0010;
        static constexpr int k_sta_del = 0x0020;
        static constexpr int k_sta_unsync = 0x0040;
        static constexpr int k_sta_freqhold = 0x0080;
        static constexpr int k_sta_nano = 0x2000;
        static constexpr int k_sta_mode = 0x4000;

        static constexpr unsigned int k_timex_known_mode_mask =
            k_adj_offset | k_adj_frequency | k_adj_maxerror | k_adj_esterror |
            k_adj_status | k_adj_timeconst | k_adj_tai | k_adj_setoffset |
            k_adj_micro | k_adj_nano | k_adj_tick;
        static constexpr int k_timex_status_writable_mask =
            k_sta_pll | k_sta_ppsfreq | k_sta_ppstime | k_sta_fll |
            k_sta_ins | k_sta_del | k_sta_unsync | k_sta_freqhold |
            k_sta_nano | k_sta_mode;
        static constexpr long k_timex_frequency_limit = 32768000L;
        static constexpr long k_timex_offset_limit_us = 500000L;
        static constexpr long k_clock_hz = static_cast<long>(1000000ULL / tmm::tick_period_us);
        static constexpr long k_timex_tick_min = 900000L / k_clock_hz;
        static constexpr long k_timex_tick_max = 1100000L / k_clock_hz;

        static State make_default_state();
        int state_result_code() const;
        int populate_time_field(abi::KernelTimexOld &timex) const;
        int apply_delta_to_realtime(const abi::KernelTimexOld &timex);

        State _state = make_default_state();
    };

    extern KernelTimexController k_timex;
}
