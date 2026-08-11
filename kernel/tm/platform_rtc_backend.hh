#pragma once

#include "types.hh"

namespace platform::rtc_backend
{
enum class ReadResult : uint8
{
    Ready,
    Unavailable,
    InvalidValue,
};

// 后端只负责读取硬件墙钟并转换成 Unix epoch；时间基准的安装属于通用时间层。
ReadResult read_epoch_seconds(uint64 &epoch_seconds);
const char *name();
} // namespace platform::rtc_backend
