#include "printer.hh"
#include <stdarg.h>

#ifdef RISCV
#include "../hal/riscv/sbi.hh"
#endif

// 全局打印器实例
Printer k_printer;

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

void Printer::printint( int xx, int base, int sign )
{
    	char buf[16];
		int	 i;
		uint x;

		if ( sign && ( sign = xx < 0 ) )
			x = -xx;
		else
			x = xx;

		i = 0;
		do {
			buf[i++] = _lower_digits[x % base];
		}
		while ( ( x /= base ) != 0 );

		if ( sign ) buf[i++] = '-';

		if ( _type == out_type::console && _console )
			while ( --i >= 0 ) _console->console_putc( buf[i] );
}

void Printer::printbyte( uint8 x )
{
    if ( _type == out_type::console && _console )
        _console->console_putc(x);
}

void Printer::printptr( uint64 x )
{
    if ( _type == out_type::console && _console )
        {
              unsigned int i;
                _console->console_putc('0');
                _console->console_putc('x');
                for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
                {
                    _console->console_putc(_lower_digits[x >> 60]);
                }
        }
}

void Printer::print( const char *fmt, ... )
{
// #define DIS_PRINTF
#ifdef DIS_PRINTF
  // 当定义了 DIS_PRINTF 宏时，不产生任何输出
  // 但仍需要处理可变参数以避免潜在问题
  va_list ap1;
  va_start(ap1, fmt);
  va_end(ap1);
  return;
#endif

  va_list ap;
  int i, c, tmp_locking;
  const char *s;

  tmp_locking = this->_locking;
  if (tmp_locking)
    _lock.acquire();

  if (fmt == 0)
    k_panic(__FILE__, __LINE__, "null fmt");

  va_start(ap, fmt);
  for (i = 0; (c = fmt[i] & 0xff) != 0; ) {
    if (c != '%') {
      _console->console_putc(c);
      i++;
      continue;
    }
    i++; // skip '%'
    int width = 0;
    // Parse width (e.g., %04x)
    while (fmt[i] >= '0' && fmt[i] <= '9') {
      width = width * 10 + (fmt[i] - '0');
      i++;
    }
    
    // Parse length modifiers
    bool is_long = false;
    bool is_size_t = false;
    if (fmt[i] == 'l') {
      is_long = true;
      i++;
    } else if (fmt[i] == 'z') {
      is_size_t = true;
      i++;
    }
    
    c = fmt[i] & 0xff;
    if (c == 0)
      break;
    switch (c) {
    case 'b':
      printint(va_arg(ap, int), 2, 1);
      break;
    case 'd':
      if (is_long) {
        // %ld - long int
        long val = va_arg(ap, long);
        char buf[32];
        int j = 0;
        int sign = 0;
        unsigned long uval;
        
        if (val < 0) {
          sign = 1;
          uval = -val;
        } else {
          uval = val;
        }
        
        do {
          buf[j++] = _lower_digits[uval % 10];
        } while ((uval /= 10) != 0);
        
        if (sign) buf[j++] = '-';
        
        while (--j >= 0)
          _console->console_putc(buf[j]);
      } else {
        printint(va_arg(ap, int), 10, 1);
      }
      break;
    case 'u':
      if (is_long) {
        // %lu - unsigned long
        unsigned long val = va_arg(ap, unsigned long);
        char buf[32];
        int j = 0;
        
        do {
          buf[j++] = _lower_digits[val % 10];
        } while ((val /= 10) != 0);
        
        while (--j >= 0)
          _console->console_putc(buf[j]);
      } else if (is_size_t) {
        // %zu - size_t
        size_t val = va_arg(ap, size_t);
        char buf[32];
        int j = 0;
        
        do {
          buf[j++] = _lower_digits[val % 10];
        } while ((val /= 10) != 0);
        
        while (--j >= 0)
          _console->console_putc(buf[j]);
      } else {
        printint(va_arg(ap, uint), 10, 0);
      }
      break;
    case 'x': {
      // 打印无符号16进制
      uint64 val;
      if (is_long) {
        // %lx - unsigned long hex
        val = va_arg(ap, unsigned long);
      } else {
        val = va_arg(ap, uint64);
      }
      char buf[16];
      int j = 0;
      do {
        buf[j++] = _lower_digits[val % 16];
      } while ((val /= 16) != 0);
      // Padding with '0' if width > j
      for (int k = j; k < width; k++)
        _console->console_putc('0');
      while (--j >= 0)
        _console->console_putc(buf[j]);
      break;
    }
    case 'X': {
      // 打印大写无符号16进制
      uint64 val;
      if (is_long) {
        // %lX - unsigned long hex (uppercase)
        val = va_arg(ap, unsigned long);
      } else {
        val = va_arg(ap, uint64);
      }
      char buf[16];
      int j = 0;
      do {
        buf[j++] = _upper_digits[val % 16];
      } while ((val /= 16) != 0);
      for (int k = j; k < width; k++)
        _console->console_putc('0');
      while (--j >= 0)
        _console->console_putc(buf[j]);
      break;
    }
        case 'o': {
      // 打印大写无符号8进制（64位）
      uint64 val = va_arg(ap, uint64);
      char buf[16];
      int j = 0;
      do {
        buf[j++] = _upper_digits[val % 8];
      } while ((val /= 8) != 0);
      for (int k = j; k < width; k++)
        _console->console_putc('0');
      while (--j >= 0)
        _console->console_putc(buf[j]);
      break;
    }
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if ((s = va_arg(ap, const char *)) == 0)
        s = "(null)";
      for (; *s; s++)
        _console->console_putc(*s);
      break;
    case 'c': {
      int ch = va_arg(ap, int);
      _console->console_putc(ch);
      break;
    }
    case '%':
      _console->console_putc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      _console->console_putc('%');
      _console->console_putc(c);
      break;
    }
    i++;
  }
  va_end(ap);

  if (tmp_locking)
    _lock.release();
}

int Printer::snprint(char *buffer, size_t size, const char *fmt, ...)
{
  if (buffer == nullptr || size == 0 || fmt == nullptr)
    return -1;

  va_list ap;
  int i, c;
  const char *s;
  char *buf_ptr = buffer;
  char *buf_end = buffer + size - 1; // 为 '\0' 留一个位置
  int total_written = 0;

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
    uint x;

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
  auto write_hex = [&](uint64 val, int width, bool uppercase) {
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

  va_start(ap, fmt);
  for (i = 0; (c = fmt[i] & 0xff) != 0; ) {
    if (c != '%') {
      write_char(c);
      i++;
      continue;
    }
    i++; // skip '%'
    int width = 0;
    // Parse width (e.g., %04x)
    while (fmt[i] >= '0' && fmt[i] <= '9') {
      width = width * 10 + (fmt[i] - '0');
      i++;
    }
    
    // Parse length modifiers
    bool is_long = false;
    bool is_size_t = false;
    if (fmt[i] == 'l') {
      is_long = true;
      i++;
    } else if (fmt[i] == 'z') {
      is_size_t = true;
      i++;
    }
    
    c = fmt[i] & 0xff;
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
        write_int(va_arg(ap, uint), 10, 0);
      }
      break;
    case 'x':
      if (is_long) {
        write_hex(va_arg(ap, unsigned long), width, false);
      } else {
        write_hex(va_arg(ap, uint64), width, false);
      }
      break;
    case 'X':
      if (is_long) {
        write_hex(va_arg(ap, unsigned long), width, true);
      } else {
        write_hex(va_arg(ap, uint64), width, true);
      }
      break;
    case 'o': {
      uint64 val = va_arg(ap, uint64);
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
      uint64 val = va_arg(ap, uint64);
      for (int k = 0; (uint)k < (sizeof(uint64) * 2); k++, val <<= 4) {
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
  va_end(ap);

  // 添加字符串结束符
  *buf_ptr = '\0';

  return total_written;
}

void Printer::k_panic( const char *f, uint l, const char *info, ... )
{
  va_list ap;
  va_start( ap, info );
  printf("panic: ");
  printf("%s:%d: ", f, l);
  printf(info, ap);
  printf("\n");
  va_end( ap );
  k_printer._panicked = 1; // freeze uart output from other CPUs
  
  // 根据不同架构执行不同的关机代码
#ifdef RISCV
  sbi_shutdown();
#elif defined(LOONGARCH)
        *(volatile uint8 *)(0x8000000000000000 | 0x100E001C) = 0x34;
  // 龙芯架构的关机方法
  // 暂时使用无限循环，后续可实现具体的关机代码
  // TODO: 实现龙芯架构的关机方法
#endif

  // 无论什么架构，最后都会进入无限循环
  for(;;)
      ;
}

void Printer::assrt( const char *f, uint l, const char *expr, const char *detail, ... )
	{
		k_printer._locking = 0;
#ifdef LINUX_BUILD
		printf( "\033[91m[ assert ]=> " );
#else 
		printf( "[ assert ]=> " );
#endif 
		printf( f );
		printf( " : " );
		printf( "%d", l );
		printf( " :\n\t     " );
		_trace_flag = 1;
		printf( "assert fail for '" );
		printf( expr );
		printf( "'\n[detail] " );
		va_list ap;
		va_start( ap, detail );
		printf( detail, ap );
		va_end( ap );
		_trace_flag = 0;
#ifdef LINUX_BUILD
		printf( "\033[0m\n" );
#else 
		printf( "\n" );
#endif 
		k_printer._locking = 1;

		panic( f, l, "assert fail for above reason." );
	}

