#include "tm/platform_rtc_backend.hh"

#include "hal/loongarch/platform_board.hh"

namespace platform::rtc_backend
{
namespace
{
constexpr uint32 k_toy_trim = 0x20;
constexpr uint32 k_toy_read_low = 0x2c;
constexpr uint32 k_toy_read_high = 0x30;
constexpr uint32 k_rtc_control = 0x40;
constexpr uint32 k_rtc_trim = 0x60;
constexpr uint32 k_rtc_enable_mask = (1U << 13) | (1U << 11) | (1U << 8);

volatile uint32 *rtc_register(uint32 offset)
{
    return reinterpret_cast<volatile uint32 *>(
        loongarch::board::mmio_address(loongarch::board::k_rtc_physical) + offset);
}

bool leap_year(uint32 year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool decode_toy(uint32 high, uint32 low, uint64 &epoch)
{
    const uint32 year = 1900U + high;
    const uint32 month = (low >> 26) & 0x3fU;
    const uint32 day = (low >> 21) & 0x1fU;
    const uint32 hour = (low >> 16) & 0x1fU;
    const uint32 minute = (low >> 10) & 0x3fU;
    const uint32 second = (low >> 4) & 0x3fU;
    static constexpr uint8 month_days[12] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (year < 1970 || year > 9999 || month == 0 || month > 12 ||
        day == 0 || hour > 23 || minute > 59 || second > 59)
    {
        return false;
    }
    uint32 days_this_month = month_days[month - 1];
    if (month == 2 && leap_year(year))
    {
        ++days_this_month;
    }
    if (day > days_this_month)
    {
        return false;
    }

    uint64 days = 0;
    for (uint32 current = 1970; current < year; ++current)
    {
        days += leap_year(current) ? 366 : 365;
    }
    for (uint32 current = 1; current < month; ++current)
    {
        days += month_days[current - 1];
        if (current == 2 && leap_year(year))
        {
            ++days;
        }
    }
    days += day - 1;
    epoch = days * 86400ULL + hour * 3600ULL + minute * 60ULL + second;
    return true;
}
} // namespace

ReadResult read_epoch_seconds(uint64 &epoch_seconds)
{
    *rtc_register(k_toy_trim) = 0;
    *rtc_register(k_rtc_trim) = 0;
    *rtc_register(k_rtc_control) |= k_rtc_enable_mask;
    asm volatile("dbar 0" ::: "memory");

    for (uint32 attempt = 0; attempt < 3; ++attempt)
    {
        // TOY 低寄存器的读取会锁存同一时刻的高寄存器，顺序不能互换。
        const uint32 low = *rtc_register(k_toy_read_low);
        const uint32 high = *rtc_register(k_toy_read_high);
        if (decode_toy(high, low, epoch_seconds))
        {
            return ReadResult::Ready;
        }
    }
    epoch_seconds = 0;
    return ReadResult::InvalidValue;
}

const char *name()
{
    return "LS2K1000 TOY";
}
} // namespace platform::rtc_backend
