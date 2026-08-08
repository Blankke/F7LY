#pragma once

namespace tmm
{
// 若板级 RTC 可用，将 CLOCK_REALTIME 校准到硬件墙钟；失败时保留内核默认基准。
bool initialize_platform_realtime();
}
