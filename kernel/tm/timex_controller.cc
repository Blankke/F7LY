#include "timex_controller.hh"

#include <asm-generic/errno-base.h>
#include <bits/time.h>

namespace tmm
{
    KernelTimexController k_timex;

    KernelTimexController::State KernelTimexController::make_default_state()
    {
        State state{};
        state.status = k_sta_nano;
        state.tolerance = k_timex_frequency_limit;
        state.tick = 1000000L / k_clock_hz;
        return state;
    }

    bool KernelTimexController::timespec_to_ns_checked(const tmm::timespec &ts,
                                                       int64_t &ns) const
    {
        if (ts.tv_sec < 0 ||
            ts.tv_nsec < 0 ||
            ts.tv_nsec >= abi::k_nsec_per_sec)
        {
            return false;
        }
        if (ts.tv_sec > INT64_MAX / abi::k_nsec_per_sec)
        {
            return false;
        }

        ns = static_cast<int64_t>(ts.tv_sec) * abi::k_nsec_per_sec + ts.tv_nsec;
        return true;
    }

    void KernelTimexController::ns_to_timespec(int64_t ns, tmm::timespec &ts) const
    {
        if (ns < 0)
        {
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
            return;
        }

        ts.tv_sec = static_cast<long>(ns / abi::k_nsec_per_sec);
        ts.tv_nsec = static_cast<long>(ns % abi::k_nsec_per_sec);
    }

    int KernelTimexController::state_result_code() const
    {
        return (_state.status & k_sta_unsync) ? k_time_error : k_time_ok;
    }

    int KernelTimexController::populate_time_field(abi::KernelTimexOld &tx) const
    {
        tmm::timespec now{};
        if (tmm::k_tm.clock_gettime(static_cast<tmm::SystemClockId>(CLOCK_REALTIME), &now) < 0)
        {
            return -EIO;
        }

        tx.time.tv_sec = now.tv_sec;
        tx.time.tv_usec = (_state.status & k_sta_nano) ? now.tv_nsec : now.tv_nsec / 1000;
        return 0;
    }

    int KernelTimexController::snapshot(abi::KernelTimexOld &tx)
    {
        tx.modes = (_state.status & k_sta_nano) ? k_adj_nano : k_adj_micro;
        tx.offset = _state.offset;
        tx.freq = _state.freq;
        tx.maxerror = _state.maxerror;
        tx.esterror = _state.esterror;
        tx.status = _state.status;
        tx.constant = _state.constant;
        tx.precision = _state.precision;
        tx.tolerance = _state.tolerance;
        tx.tick = _state.tick;
        tx.tai = _state.tai;

        int time_ret = populate_time_field(tx);
        if (time_ret < 0)
        {
            return time_ret;
        }
        return state_result_code();
    }

    int KernelTimexController::apply_delta_to_realtime(const abi::KernelTimexOld &tx)
    {
        int64_t delta_ns = static_cast<int64_t>(tx.time.tv_sec) * abi::k_nsec_per_sec;
        if (_state.status & k_sta_nano)
        {
            delta_ns += tx.time.tv_usec;
        }
        else
        {
            delta_ns += static_cast<int64_t>(tx.time.tv_usec) * 1000;
        }

        tmm::timespec now{};
        if (tmm::k_tm.clock_gettime(static_cast<tmm::SystemClockId>(CLOCK_REALTIME), &now) < 0)
        {
            return -EIO;
        }

        int64_t now_ns = 0;
        if (!timespec_to_ns_checked(now, now_ns))
        {
            return -EINVAL;
        }

        tmm::timespec target{};
        ns_to_timespec(now_ns + delta_ns, target);
        return tmm::k_tm.clock_settime(static_cast<tmm::SystemClockId>(CLOCK_REALTIME), &target);
    }

    int KernelTimexController::apply(abi::KernelTimexOld &tx, bool has_privilege)
    {
        const unsigned int modes = tx.modes;
        const bool is_special_offset_mode =
            modes == k_adj_offset_singleshot || modes == k_adj_offset_ss_read;
        const bool effective_nano_mode =
            (modes & k_adj_nano) ? true :
            (modes & k_adj_micro) ? false :
            ((_state.status & k_sta_nano) != 0);
        const long effective_offset_limit =
            effective_nano_mode ? k_timex_offset_limit_us * 1000L : k_timex_offset_limit_us;

        if (modes == 0 || modes == k_adj_offset_ss_read)
        {
            return snapshot(tx);
        }
        if (!is_special_offset_mode && (modes & ~k_timex_known_mode_mask) != 0)
        {
            return -EINVAL;
        }
        if (!has_privilege)
        {
            return -EPERM;
        }
        if ((modes & k_adj_nano) && (modes & k_adj_micro))
        {
            return -EINVAL;
        }
        if (modes == k_adj_offset_singleshot)
        {
            _state.offset = tx.offset;
            return snapshot(tx);
        }

        if ((modes & k_adj_status) && (tx.status & ~k_timex_status_writable_mask) != 0)
        {
            return -EINVAL;
        }
        if ((modes & k_adj_frequency) &&
            (tx.freq < -k_timex_frequency_limit || tx.freq > k_timex_frequency_limit))
        {
            return -EINVAL;
        }
        if ((modes & k_adj_offset) &&
            (tx.offset < -effective_offset_limit || tx.offset > effective_offset_limit))
        {
            return -EINVAL;
        }
        if ((modes & k_adj_tick) &&
            (tx.tick < k_timex_tick_min || tx.tick > k_timex_tick_max))
        {
            return -EINVAL;
        }

        if (modes & k_adj_offset)
        {
            _state.offset = tx.offset;
        }
        if (modes & k_adj_frequency)
        {
            _state.freq = tx.freq;
        }
        if (modes & k_adj_maxerror)
        {
            _state.maxerror = tx.maxerror;
        }
        if (modes & k_adj_esterror)
        {
            _state.esterror = tx.esterror;
        }
        if (modes & k_adj_status)
        {
            _state.status =
                (_state.status & ~k_timex_status_writable_mask) |
                (tx.status & k_timex_status_writable_mask);
        }
        if (modes & k_adj_timeconst)
        {
            _state.constant = tx.constant;
        }
        if (modes & k_adj_tick)
        {
            _state.tick = tx.tick;
        }
        if (modes & k_adj_tai)
        {
            _state.tai = static_cast<int>(tx.constant);
        }
        if (modes & k_adj_nano)
        {
            _state.status |= k_sta_nano;
        }
        if (modes & k_adj_micro)
        {
            _state.status &= ~k_sta_nano;
        }
        if (modes & k_adj_setoffset)
        {
            int delta_ret = apply_delta_to_realtime(tx);
            if (delta_ret < 0)
            {
                return delta_ret;
            }
        }

        return snapshot(tx);
    }
}
