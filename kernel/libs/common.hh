
#pragma once

#include "types.hh"
#include "klib.hh"
#include "printer.hh"

/// @brief 将指定位设置为1
void bit_set(void *data, uint kbit);

/// @brief 将指定位清零
void bit_reset(void *data, uint kbit);

/// @brief 读取指定位
[[nodiscard]] bool bit_test(void *data, uint kbit);

/// @brief 按 memcmp 语义比较定长字符串：0 表示相等，负值表示 s < t，正值表示 s > t
int compare(const char *s, const char *t, uint len);

/// @brief 获取最低的置位位置，x==0 时返回 -1
int lowest_bit(uint64 x);

/// @brief 获取最高的置位位置，x==0 时返回 -1
int highest_bit(uint64 x);

inline char unicode_to_ascii(uint16 uc)
{
	return static_cast<char>(uc & 0xFFU);
}

namespace math
{
/// @brief 求幂
/// @param x 底数
/// @param y 指数
/// @return x 的 y 次方
uint64 power(uint64 x, uint64 y);
} // namespace math
