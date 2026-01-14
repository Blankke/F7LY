#include "function.hh"
#include "printer.hh"

namespace std
{
	[[noreturn]] void __throw_bad_function_call()
	{
		panic("! >>> bad function call");
	}
} // namespace std
