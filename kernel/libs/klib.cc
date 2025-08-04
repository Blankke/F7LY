#include "klib.hh"
// #include <stdarg.h>
// #include <stddef.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif
void *memset(void *s, int c, size_t n) noexcept(true)
{
	for (char *b = (char *)s; (size_t)(b - (char *)s) < n; b++)
		*b = c;
	return s;
}

void *memmove(void *dst, const void *src, size_t n) noexcept(true)
{
	// may overlap
	if (n <= 0)
		return dst;
	const char *i = (const char *)src;
	char *j = (char *)dst;
	for (i += n - 1, j += n - 1; i >= (const char *)src; i--, j--)
		*j = *i;
	return dst;
}

void *memcpy(void *out, const void *in, size_t n) noexcept(true)
{
	const char *i = (const char *)in;
	char *j = (char *)out;
	for (; static_cast<size_t>(j - ((char *)out)) < n; i++, j++)
		*j = *i;
	return out;
}

int memcmp(const void *s1, const void *s2, size_t n) noexcept(true)
{
	size_t cnt = 1;
	const unsigned char *i = (const unsigned char *)s1, *j = (const unsigned char *)s2;
	if (n == 0)
		return 0;
	for (; *i == *j; i++, j++, cnt++)
	{
		if (!*i || cnt >= n)
			return 0;
	}
	return *i < *j ? -1 : 1;
}

const void *
memchr(const void *src_void,
	   int c,
	   size_t length) noexcept(true)
{
	const unsigned char *src = (const unsigned char *)src_void;
	unsigned char d = c;

	while (length--)
	{
		if (*src == d)
			return (void *)src;
		src++;
	}

	return nullptr;
}

// convert wide char string into uchar string
void snstr(char *dst, wchar const *src, int len)
{
	while (len-- && *src)
	{
		*dst++ = (uchar)(*src & 0xff);
		src++;
	}
	while (len-- > 0)
		*dst++ = 0;
}

char *strncpy(char *dst, const char *src, size_t n) noexcept(true)
{
	char *j;
	size_t cnt = 0;
	for (j = dst; *src && cnt < n; src++, j++, cnt++)
		*j = *src;
	for (; cnt < n; j++, cnt++)
		*j = '\0';
	return dst;
}

char *strcat(char *dst, const char *src) noexcept(true)
{
	char *j = dst;
	while (*j)
		j++;
	for (; *src; src++, j++)
		*j = *src;
	*j = '\0';
	return dst;
}

int strcmp(const char *s1, const char *s2) noexcept(true)
{
	for (; *s1 == *s2; s1++, s2++)
	{
		if (!*s1)
			return 0;
	}
	return *s1 < *s2 ? -1 : 1;
}

int strncmp(const char *s1, const char *s2, size_t n) noexcept(true)
{
	size_t cnt = 1;
	if (n == 0)
		return 0;
	for (; *s1 == *s2; s1++, s2++, cnt++)
	{
		if (!*s1 || cnt == n)
			return 0;
	}
	return *s1 < *s2 ? -1 : 1;
}

int strncmpamb(const char *s1, const char *s2, size_t n)
{
	size_t cnt = 1;
	if (n == 0)
		return 0;
	for (; (*s1 == *s2) || ((*s1 >= 'a') && (*s1 <= 'z') && ((*s2 - *s1) == ('A' - 'a'))) || ((*s1 >= 'A') && (*s1 <= 'Z') && ((*s1 - *s2) == ('A' - 'a'))); s1++, s2++, cnt++)
	{
		if (!*s1 || cnt == n)
			return 0;
	}
	return *s1 < *s2 ? -1 : 1;
}

char *strchr(const char *s, char c)
{
	for (; *s; s++)
		if (*s == c)
			return (char *)s;
	return 0;
}

char *strcpy(char *dst, const char *src) noexcept(true)
{
	char *j;
	for (j = dst; *src; src++, j++)
		*j = *src;
	*j = '\0';
	return dst;
}

char *safestrcpy(char *s, const char *t, int n)
{
	char *os;

	os = s;
	if (n <= 0)
		return os;
	while (--n > 0 && (*s++ = *t++) != 0)
		;
	*s = 0;
	return os;
}

size_t strlen(const char *s) noexcept(true)
{
	size_t len = 0;
	while (*s)
		s++, len++;
	return len;
}

float ceilf(float x)
{
	union
	{
		float f;
		uint32 i;
	} u = {x};

	// Extract sign, exponent, and mantissa from x
	uint32 sign = u.i & 0x80000000;
	int exponent = (u.i >> 23 & 0xFF) - 127;
	uint32 mantissa = u.i & 0x7FFFFF;

	// If x is NaN, infinity, or already an integer, return x
	if (exponent >= 23 || x != x || x == 1.0f / 0.0f || x == -1.0f / 0.0f)
	{
		return x;
	}

	// If x is less than one and not zero, return 1.0 or 0.0 depending on the sign
	if (exponent < 0)
	{
		return sign ? -0.0f : 1.0f;
	}

	// If the fractional part is zero, return x
	if ((mantissa & ((1 << (23 - exponent)) - 1)) == 0)
	{
		return x;
	}

	// Otherwise, return the next integer toward positive infinity
	if (!sign)
	{
		u.i += 1 << (23 - exponent);
	}
	u.i &= ~((1 << (23 - exponent)) - 1);

	return u.f;
}

void *operator new[](size_t size, const char *name, int flags, unsigned debugFlags, const char *file, int line)
{
	return operator new(size);
}

void *operator new[](size_t size, size_t alignment, size_t alignmentOffset, const char *pName, int flags, unsigned debugFlags, const char *file, int line)
{
	// currently we don't support alignment
	return operator new(size);
}

// Random number generator state
static unsigned long rand_seed = 1;

void srand(unsigned int seed)
{
	rand_seed = seed;
}

int rand(void)
{
	// Linear congruential generator (same as used in many C libraries)
	rand_seed = rand_seed * 1103515245 + 12345;
	return (unsigned int)(rand_seed / 65536) % 32768;
}

int atoi(const char *nptr)
{
	int result = 0;
	int sign = 1;
	
	// Skip whitespace
	while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' || 
		   *nptr == '\r' || *nptr == '\f' || *nptr == '\v')
		nptr++;
	
	// Handle sign
	if (*nptr == '-') {
		sign = -1;
		nptr++;
	} else if (*nptr == '+') {
		nptr++;
	}
	
	// Convert digits
	while (*nptr >= '0' && *nptr <= '9') {
		result = result * 10 + (*nptr - '0');
		nptr++;
	}
	
	return result * sign;
}

int sprintf(char *str, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int result = vsprintf(str, format, ap);
	va_end(ap);
	return result;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int result = vsnprintf(str, size, format, ap);
	va_end(ap);
	return result;
}

int vsprintf(char *str, const char *format, va_list ap)
{
	// Use a very large size for sprintf (no bounds checking)
	return vsnprintf(str, SIZE_MAX, format, ap);
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	if (str == nullptr || size == 0 || format == nullptr)
		return -1;

	int i, c;
	const char *s;
	char *buf_ptr = str;
	char *buf_end = str + size - 1; // 为 '\0' 留一个位置
	int total_written = 0;
	
	static char _lower_digits[] = "0123456789abcdef";
	static char _upper_digits[] = "0123456789ABCDEF";

	// 内部函数：向缓冲区写入一个字符
	auto write_char = [&](char ch) -> bool {
		if (buf_ptr < buf_end) {
			*buf_ptr++ = ch;
			total_written++;
			return true;
		}
		total_written++;  // 仍然计数，即使没有写入
		return false;
	};

	// 内部函数：向缓冲区写入整数
	auto write_int = [&](int xx, int base, int sign) {
		char int_buf[16];
		int idx = 0;
		unsigned int x;

		if (sign && (sign = xx < 0))
			x = -xx;
		else
			x = xx;

		do {
			int_buf[idx++] = _lower_digits[x % base];
		} while ((x /= base) != 0);

		if (sign) int_buf[idx++] = '-';

		while (--idx >= 0) {
			write_char(int_buf[idx]);
		}
	};

	// 内部函数：向缓冲区写入长整数
	auto write_long = [&](long xx, int base, int sign) {
		char long_buf[32];
		int idx = 0;
		unsigned long x;

		if (sign && (sign = xx < 0))
			x = -xx;
		else
			x = xx;

		do {
			long_buf[idx++] = _lower_digits[x % base];
		} while ((x /= base) != 0);

		if (sign) long_buf[idx++] = '-';

		while (--idx >= 0) {
			write_char(long_buf[idx]);
		}
	};

	// 内部函数：向缓冲区写入无符号长整数
	auto write_ulong = [&](unsigned long val, int base) {
		char ulong_buf[32];
		int idx = 0;

		do {
			ulong_buf[idx++] = _lower_digits[val % base];
		} while ((val /= base) != 0);

		while (--idx >= 0) {
			write_char(ulong_buf[idx]);
		}
	};

	// 内部函数：向缓冲区写入size_t
	auto write_size_t = [&](size_t val, int base) {
		char size_buf[32];
		int idx = 0;

		do {
			size_buf[idx++] = _lower_digits[val % base];
		} while ((val /= base) != 0);

		while (--idx >= 0) {
			write_char(size_buf[idx]);
		}
	};

	// 内部函数：向缓冲区写入十六进制数
	auto write_hex = [&](unsigned long long val, int width, bool uppercase) {
		char hex_buf[16];
		int j = 0;
		char *digits = uppercase ? _upper_digits : _lower_digits;
		
		do {
			hex_buf[j++] = digits[val % 16];
		} while ((val /= 16) != 0);
		
		// Padding with '0' if width > j
		for (int k = j; k < width; k++)
			write_char('0');
		
		while (--j >= 0)
			write_char(hex_buf[j]);
	};

	for (i = 0; (c = format[i] & 0xff) != 0; ) {
		if (c != '%') {
			write_char(c);
			i++;
			continue;
		}
		i++; // skip '%'
		int width = 0;
		// Parse width (e.g., %04x)
		while (format[i] >= '0' && format[i] <= '9') {
			width = width * 10 + (format[i] - '0');
			i++;
		}
		
		// Parse length modifiers
		bool is_long = false;
		bool is_size_t = false;
		if (format[i] == 'l') {
			is_long = true;
			i++;
		} else if (format[i] == 'z') {
			is_size_t = true;
			i++;
		}
		
		c = format[i] & 0xff;
		if (c == 0)
			break;
		switch (c) {
		case 'b':
			write_int(va_arg(ap, int), 2, 1);
			break;
		case 'd':
			if (is_long) {
				write_long(va_arg(ap, long), 10, 1);
			} else {
				write_int(va_arg(ap, int), 10, 1);
			}
			break;
		case 'u':
			if (is_long) {
				write_ulong(va_arg(ap, unsigned long), 10);
			} else if (is_size_t) {
				write_size_t(va_arg(ap, size_t), 10);
			} else {
				write_int(va_arg(ap, unsigned int), 10, 0);
			}
			break;
		case 'x':
			if (is_long) {
				write_hex(va_arg(ap, unsigned long), width, false);
			} else {
				write_hex(va_arg(ap, unsigned long long), width, false);
			}
			break;
		case 'X':
			if (is_long) {
				write_hex(va_arg(ap, unsigned long), width, true);
			} else {
				write_hex(va_arg(ap, unsigned long long), width, true);
			}
			break;
		case 'o': {
			unsigned long long val = va_arg(ap, unsigned long long);
			char oct_buf[16];
			int j = 0;
			do {
				oct_buf[j++] = _upper_digits[val % 8];
			} while ((val /= 8) != 0);
			for (int k = j; k < width; k++)
				write_char('0');
			while (--j >= 0)
				write_char(oct_buf[j]);
			break;
		}
		case 'p': {
			write_char('0');
			write_char('x');
			unsigned long long val = va_arg(ap, unsigned long long);
			for (int k = 0; (unsigned int)k < (sizeof(unsigned long long) * 2); k++, val <<= 4) {
				write_char(_lower_digits[val >> 60]);
			}
			break;
		}
		case 's':
			if ((s = va_arg(ap, const char *)) == 0)
				s = "(null)";
			for (; *s; s++)
				write_char(*s);
			break;
		case 'c': {
			int ch = va_arg(ap, int);
			write_char(ch);
			break;
		}
		case '%':
			write_char('%');
			break;
		default:
			// Print unknown % sequence to draw attention.
			write_char('%');
			write_char(c);
			break;
		}
		i++;
	}

	// 添加字符串结束符
	*buf_ptr = '\0';

	return total_written;
}