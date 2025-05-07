

#include "global_operator.hh"
#include "heap_memory_manager.hh"

void * operator new ( uint64 size )
{
	// log_info( "new with size %d", size );
	void *p = mem::k_hmm.allocate( size );
	// log_trace( "new alloc %p", p );
	return p;
}
void * operator new[] ( uint64 size )
{
	// log_info( "new[] with size %d", size );
	void *p = mem::k_hmm.allocate( size );
	// log_trace( "new[] alloc %p", p );
	return p;
}
void operator delete ( void * p ) noexcept
{
	// log_info( "delete 0x%p", p );
	mem::k_hmm.free( p );
}
void operator delete[] ( void * p ) noexcept
{
	// log_info( "delete[] 0x%p", p );
	mem::k_hmm.free( p );
}
void operator delete( void * p, uint64 size ) noexcept
{
	// log_info( "delete 0x%p with size %d", p, size );
	mem::k_hmm.free( p );
}
void operator delete[] ( void * p, uint64 size ) noexcept
{
	// log_info( "delete[] 0x%p with size %d", p, size );
	mem::k_hmm.free( p );
}