#pragma once

namespace platform::power
{
// 请求当前平台关机；硬件不支持时永久停驻当前 CPU。该函数不会返回。
[[noreturn]] void shutdown();
} // namespace platform::power
