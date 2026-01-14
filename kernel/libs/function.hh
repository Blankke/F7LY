#pragma once

#include <functional>

// 在内核态没有异常支持，std::function 仍会依赖该符号。
namespace std
{
	[[noreturn]] void __throw_bad_function_call();
} // namespace std
