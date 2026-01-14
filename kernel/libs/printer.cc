#include "printer.hh"
#include <stdarg.h>

#ifdef RISCV
#include "../hal/riscv/sbi.hh"
#endif

// 全局打印器实例
Printer k_printer;

namespace
{

bool disable_printf_flag = false;

struct ConsoleWriter
{
	dev::Console *console;

	// 封装控制台输出，便于与 BufferWriter 共用格式化流程
	void putc(char ch) { if (console) console->console_putc(ch); }
};

struct BufferWriter
{
	char *cur;
	char *limit;
	int total;

	// 只在容量内写入，同时累计总输出长度（便于上层获知是否被截断）
	void putc(char ch)
	{
		if (cur < limit) *cur++ = ch;
		++total;
	}

	void finish() { *cur = '\0'; }
};

enum class LengthModifier
{
	Default,
	LongInt,
	SizeT,
};

// 全局数字缓冲区大小，避免魔数散落
constexpr int kNumericBufferSize = 64;

template <typename Writer>
void write_string(Writer &writer, const char *str)
{
	if (str == nullptr) str = "(null)";
	while (*str) writer.putc(*str++);
}

template <typename Writer>
void write_unsigned(Writer &writer, uint64 value, uint base, bool uppercase, int width)
{
	char buf[kNumericBufferSize];
	int idx = 0;
	const char *digits = uppercase ? Printer::upper_digits() : Printer::lower_digits();

	do
	{
		buf[idx++] = digits[value % base];
		value /= base;
	} while (value && idx < kNumericBufferSize);

	if (width > kNumericBufferSize - 1) width = kNumericBufferSize - 1;
	while (idx < width && idx < kNumericBufferSize) buf[idx++] = '0';
	while (--idx >= 0) writer.putc(buf[idx]);
}

template <typename Writer>
void write_signed(Writer &writer, int64 value, uint base, int width)
{
	if (value < 0)
	{
		writer.putc('-');
		write_unsigned(writer, static_cast<uint64>(-value), base, false, width);
	}
	else
	{
		write_unsigned(writer, static_cast<uint64>(value), base, false, width);
	}
}

template <typename Writer>
void write_pointer(Writer &writer, uint64 value)
{
	writer.putc('0');
	writer.putc('x');
	for (int i = 0; i < static_cast<int>(sizeof(uint64) * 2); ++i, value <<= 4)
	{
		writer.putc(Printer::lower_digits()[value >> 60]);
	}
}

template <typename Writer>
void vformat_to_writer(Writer &writer, const char *fmt, va_list ap)
{
	for (size_t i = 0; fmt && fmt[i]; ++i)
	{
		char c = fmt[i];
		if (c != '%')
		{
			writer.putc(c);
			continue;
		}

		++i; // skip '%'
		int width = 0;
		while (fmt[i] >= '0' && fmt[i] <= '9')
		{
			width = width * 10 + (fmt[i] - '0');
			++i;
		}

		LengthModifier length = LengthModifier::Default;
		if (fmt[i] == 'l')
		{
			length = LengthModifier::LongInt;
			++i;
		}
		else if (fmt[i] == 'z')
		{
			length = LengthModifier::SizeT;
			++i;
		}

		c = fmt[i];
		switch (c)
		{
		case 'b':
			write_signed(writer, va_arg(ap, int), 2, width);
			break;
		case 'd':
			if (length == LengthModifier::LongInt)
				write_signed(writer, va_arg(ap, long), 10, width);
			else
				write_signed(writer, va_arg(ap, int), 10, width);
			break;
		case 'u':
			if (length == LengthModifier::LongInt)
				write_unsigned(writer, va_arg(ap, unsigned long), 10, false, width);
			else if (length == LengthModifier::SizeT)
				write_unsigned(writer, va_arg(ap, size_t), 10, false, width);
			else
				write_unsigned(writer, va_arg(ap, unsigned int), 10, false, width);
			break;
		case 'x':
			if (length == LengthModifier::LongInt)
				write_unsigned(writer, va_arg(ap, unsigned long), 16, false, width);
			else
				write_unsigned(writer, va_arg(ap, unsigned int), 16, false, width);
			break;
		case 'X':
			if (length == LengthModifier::LongInt)
				write_unsigned(writer, va_arg(ap, unsigned long), 16, true, width);
			else
				write_unsigned(writer, va_arg(ap, unsigned int), 16, true, width);
			break;
		case 'o':
			if (length == LengthModifier::LongInt)
				write_unsigned(writer, va_arg(ap, unsigned long), 8, false, width);
			else
				write_unsigned(writer, va_arg(ap, unsigned int), 8, false, width);
			break;
		case 'p':
			write_pointer(writer, va_arg(ap, uint64));
			break;
		case 's':
			write_string(writer, va_arg(ap, const char *));
			break;
		case 'c':
			writer.putc(static_cast<char>(va_arg(ap, int)));
			break;
		case '%':
			writer.putc('%');
			break;
		default:
			writer.putc('%');
			writer.putc(c);
			break;
		}
	}
}

void log_with_prefix(const char *tag, const char *f, uint l, const char *info, va_list ap)
{
	if (disable_printf_flag || info == nullptr) return;

	const int need_lock = k_printer.locking_enabled();
	if (need_lock) k_printer.acquire_lock();

	ConsoleWriter writer { k_printer.get_console() };
	write_string(writer, "[");
	write_string(writer, tag);
	write_string(writer, "] ");
	write_string(writer, f);
	writer.putc(':');
	write_unsigned(writer, l, 10, false, 0);
	write_string(writer, ": ");
	vformat_to_writer(writer, info, ap);
	writer.putc('\n');

	if (need_lock) k_printer.release_lock();
}

[[noreturn]] void panic_impl(const char *f, uint l, const char *info, va_list ap)
{
	ConsoleWriter writer { k_printer.get_console() };
	write_string(writer, "panic: ");
	write_string(writer, f);
	writer.putc(':');
	write_unsigned(writer, l, 10, false, 0);
	write_string(writer, ": ");
	if (info) vformat_to_writer(writer, info, ap);
	writer.putc('\n');

	k_printer.set_panicked(); // freeze uart output from other CPUs

#ifdef RISCV
	sbi_shutdown();
#elif defined(LOONGARCH)
	*(volatile uint8 *)(0x8000000000000000 | 0x100E001C) = 0x34;
#endif

	for (;;)
		;
}

} // namespace

int	 Printer::_trace_flag	  = 0;
char Printer::_lower_digits[] = "0123456789abcdef";
char Printer::_upper_digits[] = "0123456789ABCDEF";

void Printer::init()
{
	_lock.init("printer");
	_locking = 1;

	// 初始化控制台并关联
	dev::kConsole.init();
	_console = &dev::kConsole;
	_type = out_type::console;
	printf("Printer::init end\n");
}

// printf 控制函数实现
void Printer::enable_printf()
{
	disable_printf_flag = false;
}

void Printer::disable_printf()
{
	disable_printf_flag = true;
}

bool Printer::is_printf_disabled()
{
	return disable_printf_flag;
}

void Printer::printint(int xx, int base, int sign)
{
	if (_type != out_type::console || _console == nullptr) return;

	ConsoleWriter writer { _console };
	if (sign)
		write_signed(writer, xx, base, 0);
	else
		write_unsigned(writer, static_cast<uint>(xx), base, false, 0);
}

void Printer::printbyte(uint8 x)
{
	if (_type != out_type::console || _console == nullptr) return;
	_console->console_putc(x);
}

void Printer::printptr(uint64 x)
{
	if (_type != out_type::console || _console == nullptr) return;
	ConsoleWriter writer { _console };
	write_pointer(writer, x);
}

void Printer::print(const char *fmt, ...)
{
	if (disable_printf_flag) return;
	if (fmt == nullptr) k_panic(__FILE__, __LINE__, "null fmt");

	const int tmp_locking = _locking;
	if (tmp_locking) _lock.acquire();

	va_list ap;
	va_start(ap, fmt);
	ConsoleWriter writer { _console };
	vformat_to_writer(writer, fmt, ap);
	va_end(ap);

	if (tmp_locking) _lock.release();
}

int Printer::snprint(char *buffer, size_t size, const char *fmt, ...)
{
	if (buffer == nullptr || size == 0 || fmt == nullptr) return -1;

	BufferWriter writer { buffer, buffer + size - 1, 0 };

	va_list ap;
	va_start(ap, fmt);
	vformat_to_writer(writer, fmt, ap);
	va_end(ap);

	writer.finish();
	return writer.total;
}

void Printer::k_panic(const char *f, uint l, const char *info, ...)
{
	va_list ap;
	va_start(ap, info);
	panic_impl(f, l, info, ap);
}

void Printer::panic_va(const char *f, uint l, const char *info, va_list ap)
{
	panic_impl(f, l, info, ap);
}

void Printer::error(const char *f, uint l, const char *info, ...)
{
	va_list ap;
	va_start(ap, info);
	error_va(f, l, info, ap);
	va_end(ap);
}

void Printer::error_va(const char *f, uint l, const char *info, va_list ap)
{
	log_with_prefix("error", f, l, info, ap);
}

void Printer::warn(const char *f, uint l, const char *info, ...)
{
	va_list ap;
	va_start(ap, info);
	warn_va(f, l, info, ap);
	va_end(ap);
}

void Printer::warn_va(const char *f, uint l, const char *info, va_list ap)
{
	log_with_prefix("warn", f, l, info, ap);
}

void Printer::info(const char *f, uint l, const char *info, ...)
{
	va_list ap;
	va_start(ap, info);
	info_va(f, l, info, ap);
	va_end(ap);
}

void Printer::info_va(const char *f, uint l, const char *info, va_list ap)
{
	log_with_prefix("info", f, l, info, ap);
}

void Printer::trace(const char *f, uint l, const char *info, ...)
{
	va_list ap;
	va_start(ap, info);
	trace_va(f, l, info, ap);
	va_end(ap);
}

void Printer::trace_va(const char *f, uint l, const char *info, va_list ap)
{
	if (_trace_flag == 0) return;
	log_with_prefix("trace", f, l, info, ap);
}

void Printer::assrt(const char *f, uint l, const char *expr, const char *detail, ...)
{
	// 避免锁在 panic 路径上造成死锁
	k_printer.set_locking(false);

	ConsoleWriter writer { k_printer.get_console() };
#ifdef LINUX_BUILD
	write_string(writer, "\033[91m[ assert ]=> ");
#else
	write_string(writer, "[ assert ]=> ");
#endif
	write_string(writer, f);
	write_string(writer, " : ");
	write_unsigned(writer, l, 10, false, 0);
	write_string(writer, " :\n\t     assert fail for '");
	write_string(writer, expr);
	write_string(writer, "'\n[detail] ");

	va_list ap;
	va_start(ap, detail);
	vformat_to_writer(writer, detail, ap);
	va_end(ap);

#ifdef LINUX_BUILD
	write_string(writer, "\033[0m\n");
#else
	write_string(writer, "\n");
#endif

	k_printer.set_locking(true);
	panic(f, l, "assert fail for above reason.");
}
