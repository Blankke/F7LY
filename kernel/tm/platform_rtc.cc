#include "platform_rtc.hh"

#include "platform_rtc_backend.hh"
#include "timer_manager.hh"
#include "printer.hh"

namespace tmm
{
bool initialize_platform_realtime()
{
    uint64 epoch = 0;
    const platform::rtc_backend::ReadResult result =
        platform::rtc_backend::read_epoch_seconds(epoch);
    if (result == platform::rtc_backend::ReadResult::Unavailable)
    {
        // 没有硬件 RTC 是平台能力缺失，不是启动失败；保持 timer manager 的默认基准。
        return true;
    }
    if (result != platform::rtc_backend::ReadResult::Ready)
    {
        platformDiagnosticWarn("[rtc] %s returned an invalid value; using fallback realtime base\n",
                               platform::rtc_backend::name());
        return false;
    }

    timespec now{static_cast<long>(epoch), 0};
    if (k_tm.clock_settime(CLOCK_REALTIME, &now) != 0)
    {
        platformDiagnosticWarn("[rtc] failed to install %s epoch=%lu\n",
                               platform::rtc_backend::name(), epoch);
        return false;
    }
    platformDiagnosticInfo("[rtc] source=%s epoch=%lu\n",
                           platform::rtc_backend::name(), epoch);
    return true;
}
} // namespace tmm
