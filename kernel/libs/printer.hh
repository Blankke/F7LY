#pragma once
#include "devs/console.hh"
#include "string.hh"
// enum OutputLevel
// 	{
// 		out_trace,
// 		out_info,
// 		out_warn,
// 		out_error,
// 		out_panic,
// 	};
#define panic(info, args...) k_printer.k_panic(__FILE__, __LINE__, info, ##args)

#define printf(info, args...) k_printer.print(info, ##args)
#define snprintf(buffer, size, fmt, args...) k_printer.snprint(buffer, size, fmt, ##args)
#define assert(expr, detail, args...) ((expr) ? (void)0 : k_printer.assrt(__FILE__, __LINE__, #expr, detail, ##args))

#ifndef COLOR_PRINT
#define COLOR_PRINT

#define __PRINTF_WARN_COLOR(color, format, ...) \
    do {                                        \
        if (Printer::warn_group_enabled())      \
            k_printer.print(color format "\33[0m", ##__VA_ARGS__); \
    } while (0)

#define __PRINTF_INFO_COLOR(color, format, ...) \
    do {                                        \
        if (Printer::info_group_enabled())      \
            k_printer.print(color format "\33[0m", ##__VA_ARGS__); \
    } while (0)

#define printfRed(format, ...) __PRINTF_WARN_COLOR("\33[1;31m", format, ##__VA_ARGS__)
#define printfGreen(format, ...) __PRINTF_INFO_COLOR("\33[1;32m", format, ##__VA_ARGS__)
#define printfBlue(format, ...) __PRINTF_INFO_COLOR("\33[1;34m", format, ##__VA_ARGS__)
#define printfCyan(format, ...) __PRINTF_INFO_COLOR("\33[1;36m", format, ##__VA_ARGS__)
#define printfYellow(format, ...) __PRINTF_WARN_COLOR("\33[1;33m", format, ##__VA_ARGS__)
#define printfWhite(format, ...) __PRINTF_INFO_COLOR("\33[1;37m", format, ##__VA_ARGS__)
#define printfMagenta(format, ...) __PRINTF_INFO_COLOR("\33[1;35m", format, ##__VA_ARGS__)

#define tracef(info, args...) Printer::trace(__FILE__, __LINE__, info, ##args)

// 颜色太少了，我给你加几个
#define printfBlack(format, ...) __PRINTF_INFO_COLOR("\33[1;30m", format, ##__VA_ARGS__)
#define printfOrange(format, ...) __PRINTF_WARN_COLOR("\33[1;38;5;208m", format, ##__VA_ARGS__)
#define printfPurple(format, ...) __PRINTF_INFO_COLOR("\33[1;38;5;129m", format, ##__VA_ARGS__)
#define printfPink(format, ...) __PRINTF_WARN_COLOR("\33[1;38;5;205m", format, ##__VA_ARGS__)
#define printfBrown(format, ...) __PRINTF_WARN_COLOR("\33[1;38;5;94m", format, ##__VA_ARGS__)
#define printfGray(format, ...) __PRINTF_INFO_COLOR("\33[1;90m", format, ##__VA_ARGS__)
#define printfLightRed(format, ...) __PRINTF_WARN_COLOR("\33[0;91m", format, ##__VA_ARGS__)
#define printfLightGreen(format, ...) __PRINTF_INFO_COLOR("\33[0;92m", format, ##__VA_ARGS__)
#define printfLightBlue(format, ...) __PRINTF_INFO_COLOR("\33[0;94m", format, ##__VA_ARGS__)
#define printfLightCyan(format, ...) __PRINTF_INFO_COLOR("\33[0;96m", format, ##__VA_ARGS__)
#define printfLightYellow(format, ...) __PRINTF_WARN_COLOR("\33[0;93m", format, ##__VA_ARGS__)
#define printfLightMagenta(format, ...) __PRINTF_INFO_COLOR("\33[0;95m", format, ##__VA_ARGS__)

// Background colors
#define printfBgRed(format, ...) __PRINTF_WARN_COLOR("\33[1;41m", format, ##__VA_ARGS__)
#define printfBgGreen(format, ...) __PRINTF_INFO_COLOR("\33[1;42m", format, ##__VA_ARGS__)
#define printfBgBlue(format, ...) __PRINTF_INFO_COLOR("\33[1;44m", format, ##__VA_ARGS__)
#define printfBgYellow(format, ...) __PRINTF_WARN_COLOR("\33[1;43m", format, ##__VA_ARGS__)
#define printfBgCyan(format, ...) __PRINTF_INFO_COLOR("\33[1;46m", format, ##__VA_ARGS__)
#define printfBgMagenta(format, ...) __PRINTF_INFO_COLOR("\33[1;45m", format, ##__VA_ARGS__)
// Info print macros
#define Info(fmt, ...) printf("[INFO] => " fmt "", ##__VA_ARGS__)
#define Info_R(fmt, ...) printfRed("[INFO] => " fmt "", ##__VA_ARGS__)

// TODO macro
#define TODO(x)
#endif

class Printer
{
private:
	enum out_type
	{
		console,
		file,
		device,
	};
	out_type _type;
	dev::Console *_console;
	SpinLock _lock;
	int _locking = 1;
	int _panicked = 0;
	static int _trace_flag;
	static char _lower_digits[];
	static char _upper_digits[];

public:
	Printer() {}

	void init();
	inline int is_panic() { return _panicked; }

	// 内部状态访问器（仅供同文件内辅助函数使用）
	inline bool locking_enabled() const { return _locking != 0; }
	inline void set_locking(bool enabled) { _locking = enabled ? 1 : 0; }
	inline void acquire_lock() { _lock.acquire(); }
	inline void release_lock() { _lock.release(); }
	inline dev::Console *get_console() const { return _console; }
	inline void set_panicked() { _panicked = 1; }
	static inline const char *lower_digits() { return _lower_digits; }
	static inline const char *upper_digits() { return _upper_digits; }

	// 控制 printf 输出的方法
	static void enable_printf();
	static void disable_printf();
	static bool is_printf_disabled();
	static void enable_warn_group();
	static void disable_warn_group();
	static bool warn_group_enabled();
	static void enable_info_group();
	static void disable_info_group();
	static bool info_group_enabled();
	static void enable_trace_group();
	static void disable_trace_group();
	static bool trace_group_enabled();

	void print(const char *fmt, ...);
	int snprint(char *buffer, size_t size, const char *fmt, ...);
	void printint(int xx, int base, int sign);
	void printbyte(uint8 x);
	void printptr(uint64 x);

	static void k_panic(const char *f, uint l, const char *info, ...)__attribute__((noreturn));
	static void panic_va(const char *f, uint l, const char *info, va_list ap);
	static void error(const char *f, uint l, const char *info, ...);
	static void error_va(const char *f, uint l, const char *info, va_list ap);
	static void warn(const char *f, uint l, const char *info, ...);
	static void warn_va(const char *f, uint l, const char *info, va_list ap);
	static void info(const char *f, uint l, const char *info, ...);
	static void info_va(const char *f, uint l, const char *info, va_list ap);
	static void trace(const char *f, uint l, const char *info, ...);
	static void trace_va(const char *f, uint l, const char *info, va_list ap);
	static void assrt(const char *f, uint l, const char *expr, const char *detail, ...);
	static void assrt_va(const char *f, uint l, const char *expr, const char *detail,
						 va_list ap);

private:
	int _divide(ulong &n, int base)
	{
		int res = (int)(n % base);
		n = n / base;
		return res;
	}

	bool _is_number(char c) { return (unsigned)(c - '0') < 10; }

	int _to_number(char c) { return c - '0'; }

public:
	void print(const string &str)
	{
		print("%s", str.c_str());
	}
		static Printer &endl(Printer & p)
		{
			p.print("\n");
			return p;
		}

		// 接收 endl 的 << 运算符
		Printer &operator<<(Printer &(*func)(Printer &))
		{
			return func(*this);
		}

		// string 重载
		Printer &operator<<(const string &str)
		{
			print("%s", str.c_str());
			return *this;
		}

		// const char* 也顺带支持一下
		Printer &operator<<(const char *str)
		{
			print("%s", str);
			return *this;
		}
	
};
extern Printer k_printer;

// printf 控制宏定义（必须放在类定义之后以避免宏展开冲突）
#define enable_printf() Printer::enable_printf()
#define disable_printf() Printer::disable_printf()
