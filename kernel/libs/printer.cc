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
#define DIS_PRINTF
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
    c = fmt[i] & 0xff;
    if (c == 0)
      break;
    switch (c) {
    case 'b':
      printint(va_arg(ap, int), 2, 1);
      break;
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'u':
      printint(va_arg(ap, uint), 10, 0);
      break;
    case 'x': {
      // 打印无符号16进制（64位）
      uint64 val = va_arg(ap, uint64);
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
      // 打印大写无符号16进制（64位）
      uint64 val = va_arg(ap, uint64);
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

