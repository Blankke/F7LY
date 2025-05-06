#include "printer.hh"
#include <stdarg.h>
#include "../hal/riscv/sbi.hh"

// 全局打印器实例
Printer k_printer;
Console kConsole;

int	 Printer::_trace_flag	  = 0;
char Printer::_lower_digits[] = "0123456789abcdef";
char Printer::_upper_digits[] = "0123456789ABCDEF";

void Printer::init()
{
	_lock.init("printer");
	_locking = 1;
	
	// 初始化控制台并关联
	kConsole.init();
	_console = &kConsole;
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
 va_list ap;
  int i, c, tmp_locking;
  const char *s;

    tmp_locking=this->_locking;
  if(tmp_locking)
    _lock.acquire();

  if (fmt == 0)
    k_panic(__FILE__, __LINE__, "null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      _console->console_putc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, const char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        _console->console_putc(*s);
      break;
    case '%':
      _console->console_putc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      _console->console_putc('%');
      _console->console_putc(c);
      break;
    }
  }
  va_end(ap);

  if(tmp_locking)
    _lock.release();
}

void Printer::k_panic( const char *f, uint l, const char *info, ... )
{
  va_list ap;
  va_start( ap, info );
  printf("panic: ");
  printf(f);
  k_printer.print("\n");
  va_end( ap );
  k_printer._panicked = 1; // freeze uart output from other CPUs
  sbi_shutdown();
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

