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

#define printfRed(format, ...) k_printer.print("\33[1;31m" format "\33[0m", ##__VA_ARGS__)
#define printfGreen(format, ...) k_printer.print("\33[1;32m" format "\33[0m", ##__VA_ARGS__)
#define printfBlue(format, ...) k_printer.print("\33[1;34m" format "\33[0m", ##__VA_ARGS__)
#define printfCyan(format, ...) k_printer.print("\33[1;36m" format "\33[0m", ##__VA_ARGS__)
#define printfYellow(format, ...) k_printer.print("\33[1;33m" format "\33[0m", ##__VA_ARGS__)
#define printfWhite(format, ...) k_printer.print("\33[1;37m" format "\33[0m", ##__VA_ARGS__)
#define printfMagenta(format, ...) k_printer.print("\33[1;35m" format "\33[0m", ##__VA_ARGS__)
// 颜色太少了，我给你加几个
#define printfBlack(format, ...) k_printer.print("\33[1;30m" format "\33[0m", ##__VA_ARGS__)
#define printfOrange(format, ...) k_printer.print("\33[1;38;5;208m" format "\33[0m", ##__VA_ARGS__)
#define printfPurple(format, ...) k_printer.print("\33[1;38;5;129m" format "\33[0m", ##__VA_ARGS__)
#define printfPink(format, ...) k_printer.print("\33[1;38;5;205m" format "\33[0m", ##__VA_ARGS__)
#define printfBrown(format, ...) k_printer.print("\33[1;38;5;94m" format "\33[0m", ##__VA_ARGS__)
#define printfGray(format, ...) k_printer.print("\33[1;90m" format "\33[0m", ##__VA_ARGS__)
#define printfLightRed(format, ...) k_printer.print("\33[0;91m" format "\33[0m", ##__VA_ARGS__)
#define printfLightGreen(format, ...) k_printer.print("\33[0;92m" format "\33[0m", ##__VA_ARGS__)
#define printfLightBlue(format, ...) k_printer.print("\33[0;94m" format "\33[0m", ##__VA_ARGS__)
#define printfLightCyan(format, ...) k_printer.print("\33[0;96m" format "\33[0m", ##__VA_ARGS__)
#define printfLightYellow(format, ...) k_printer.print("\33[0;93m" format "\33[0m", ##__VA_ARGS__)
#define printfLightMagenta(format, ...) k_printer.print("\33[0;95m" format "\33[0m", ##__VA_ARGS__)
// Background colors
#define printfBgRed(format, ...) k_printer.print("\33[1;41m" format "\33[0m", ##__VA_ARGS__)
#define printfBgGreen(format, ...) k_printer.print("\33[1;42m" format "\33[0m", ##__VA_ARGS__)
#define printfBgBlue(format, ...) k_printer.print("\33[1;44m" format "\33[0m", ##__VA_ARGS__)
#define printfBgYellow(format, ...) k_printer.print("\33[1;43m" format "\33[0m", ##__VA_ARGS__)
#define printfBgCyan(format, ...) k_printer.print("\33[1;46m" format "\33[0m", ##__VA_ARGS__)
#define printfBgMagenta(format, ...) k_printer.print("\33[1;45m" format "\33[0m", ##__VA_ARGS__)
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

	// 控制 printf 输出的方法
	static void enable_printf();
	static void disable_printf();
	static bool is_printf_disabled();

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
