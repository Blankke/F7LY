#include "tm/platform_rtc_backend.hh"

namespace platform::rtc_backend
{
ReadResult read_epoch_seconds(uint64 &epoch_seconds)
{
    epoch_seconds = 0;
    return ReadResult::Unavailable;
}

const char *name()
{
    return "none";
}
} // namespace platform::rtc_backend
