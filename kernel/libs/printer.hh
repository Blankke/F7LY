#include "console.hh"
#include "../hal/riscv/sbi.hh"
// enum OutputLevel
// 	{
// 		out_trace,
// 		out_info,
// 		out_warn,
// 		out_error,
// 		out_panic,
// 	};


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
		Console *	_console;
        SpinLock		_lock;
		int					_locking  = 1;
		int					_panicked = 0;
		static int	_trace_flag;
		static char _lower_digits[];
		static char _upper_digits[];
    public:
        Printer() {}
    
		void init();
        inline int is_panic() { return _panicked; }
    
		void printf( const char *fmt, ... );
        void printint( int xx, int base, int sign );
		void printbyte( uint8 x );
		void printptr( uint64 x );

		static void panic( const char *f, uint l, const char *info, ... );
		static void panic_va( const char *f, uint l, const char *info, va_list ap );
		static void error( const char *f, uint l, const char *info, ... );
		static void error_va( const char *f, uint l, const char *info, va_list ap );
		static void warn( const char *f, uint l, const char *info, ... );
		static void warn_va( const char *f, uint l, const char *info, va_list ap );
		static void info( const char *f, uint l, const char *info, ... );
		static void info_va( const char *f, uint l, const char *info, va_list ap );
		static void trace( const char *f, uint l, const char *info, ... );
		static void trace_va( const char *f, uint l, const char *info, va_list ap );
		static void assrt( const char *f, uint l, const char *expr, const char *detail, ... );
		static void assrt_va( const char *f, uint l, const char *expr, const char *detail,
							  va_list ap );
		private:
		int _divide( ulong &n, int base )
		{
			int res = (int) ( n % base );
			n		= n / base;
			return res;
		}
    
		bool _is_number( char c ) { return (unsigned) ( c - '0' ) < 10; }
    
		int	 _to_number( char c ) { return c - '0'; }
    
};
extern Printer k_printer;