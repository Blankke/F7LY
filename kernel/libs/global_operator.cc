
#include "printer.hh"
#include "global_operator.hh"
#include "heap_memory_manager.hh"

void * operator new ( uint64 size )
{
	// Info("new with size %d\n", size);
	void *p = mem::k_hmm.allocate( size );

	return p;
}
void * operator new[] ( uint64 size )
{
	// Info("new[] with size %d\n", size);
	void *p = mem::k_hmm.allocate( size );

	return p;
}
void operator delete ( void * p ) noexcept
{
	// Info("delete 0x%p\n", p);
	mem::k_hmm.free( p );
}
void operator delete[] ( void * p ) noexcept
{
	// Info("delete[] 0x%p\n", p);
	mem::k_hmm.free( p );
}
void operator delete( void * p, uint64 size ) noexcept
{
	// Info("delete 0x%p with size %d\n", p, size);
	mem::k_hmm.free( p );
}
void operator delete[] ( void * p, uint64 size ) noexcept
{
	// Info("delete[] 0x%p with size %d\n", p, size);
	mem::k_hmm.free( p );
}