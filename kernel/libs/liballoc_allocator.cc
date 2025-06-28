

#include "liballoc_allocator.hh"
#include "buddysystem.hh"
#include "memlayout.hh"
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined (LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
#include "common.hh"
#include "printer.hh"
#include "platform.hh"
namespace mem
{
// ________________________________________________________________
// >>>> class L_Allocator 

	void L_Allocator::init( const char *lock_name, BuddySystem * base_alloc )
	{
		_lock.init( lock_name );
		_base_allocator = base_alloc;

		_mem_root = _best_bet = nullptr;
		_cach_size = _used_size = 0;
		_page_per_chunk = _default_page_per_chunk;
	}

// -------- public interface --------

	void *L_Allocator::malloc( uint64 req_size )
	{
		int64 size = ( int64 ) req_size;
		if ( size <= 0 )
		{
			printfYellow( "L-allocator : Try to allocate 0 byte memory. Null will be returned." );
			return nullptr;
		}
		if ( size >= ( int64 ) vm_kernel_heap_size )
		{
			printfYellow( "L-allocator : request size too big : %d Bytes", size );
			return nullptr;
		}
		if ( _use_align_ )
			size += _alignment_ + _align_info_length_;

		_lock.acquire();

		if ( _mem_root == nullptr )
		{
			_mem_root = _allocate_new_chunk( size );
			if ( _mem_root == nullptr )
			{
				_lock.release();
				printfYellow( "L-allocator : init the root cache failed due to no memory." );
				return nullptr;
			}
		}

		// Now to find enough space 

		L_TagMajor * maj = _mem_root;
		bool started_bet = false;
		int64 best_size = 0;

		// start at the best bet 
		if ( _best_bet )
		{
			best_size = _best_bet->size - _best_bet->usage;
			if ( best_size > size + ( int64 ) sizeof( L_TagMinor ) )
			{
				maj = _best_bet;
				started_bet = true;
			}
		}

		int64 diff;
		void *p;

		// loop all link list to find space 
		while ( maj )
		{
			diff = maj->size - maj->usage;		// free memory in the chunk
			if ( best_size < diff )				// this chunk has more memory than best-bet 
			{
				_best_bet = maj;
				best_size = diff;
			}

			// CASE 1 : There is not enough space in this major chunk
			if ( diff < size + ( int64 ) sizeof( L_TagMinor ) )
			{
			   // another major chunk next to this one
				if ( maj->next )
				{
					maj = maj->next;
					continue;
				}

				// maybe best-bet not best
				if ( started_bet )
				{
					maj = _mem_root;			// we shall start all over again
					started_bet = false;
					continue;
				}

				// no chunk whose free space big enough, allocate new one
				maj->next = _allocate_new_chunk( size );
				if ( maj->next == nullptr )
				{
					printfYellow( "L-allocator : no memory to allocate for the major chunk" );
					break;
				}
				maj->next->prev = maj;
				maj = maj->next;

				// fall through to CASE 2
			}


			// CASE 2 : It's a brand new chunk
			if ( maj->first == nullptr )
			{
				maj->first = ( L_TagMinor* ) ( ( uint64 ) maj + sizeof( L_TagMajor ) );

				maj->first->magic = _liballoc_magic_;
				maj->first->prev = nullptr;
				maj->first->next = nullptr;
				maj->first->chunk = maj;
				maj->first->size = size;
				maj->first->req_size = req_size;
				maj->usage += size + sizeof( L_TagMinor );

				_used_size += size;

				p = ( void* ) ( ( uint64 ) maj->first + sizeof( L_TagMinor ) );

				_align( p );

				_lock.release();
				return p;
			}

			// CASE 3 : Chunk in use and enough space at the start of the chunk 
			diff = ( int64 ) ( ( uint64 ) maj->first - ( uint64 ) maj - sizeof( L_TagMajor ) );
			if ( diff >= size + ( int64 ) sizeof( L_TagMinor ) )
			{
				maj->first->prev = ( L_TagMinor* ) ( ( uint64 ) maj + sizeof( L_TagMajor ) );
				maj->first->prev->next = maj->first;
				maj->first = maj->first->prev;

				maj->first->magic = _liballoc_magic_;
				maj->first->prev = nullptr;
				maj->first->chunk = maj;
				maj->first->size = size;
				maj->first->req_size = req_size;
				maj->usage += size + sizeof( L_TagMinor );

				_used_size += size;

				p = ( void* ) ( ( uint64 ) maj->first + sizeof( L_TagMinor ) );

				_align( p );

				_lock.release();
				return p;
			}

			// CASE 4 : There is enough space in this block, but not sure it's contiguous
			L_TagMinor * min = maj->first;
			while ( min )		// loop the chunk
			{
				// CASE 4.1 : End of minors in a chunk. 
				if ( min->next == nullptr )
				{
					// the rest of this chunk is free, look up whether it's big enough
					diff = ( int64 ) ( ( uint64 ) maj + maj->size - ( uint64 ) min - sizeof( L_TagMinor ) - min->size );
					if ( diff >= size + ( int64 ) sizeof( L_TagMinor ) )
					{
						min->next = ( L_TagMinor* ) ( ( uint64 ) min + sizeof( L_TagMinor ) + min->size );
						min->next->prev = min;
						min = min->next;

						min->next = nullptr;
						min->magic = _liballoc_magic_;
						min->chunk = maj;
						min->size = size;
						min->req_size = req_size;
						maj->usage += size + sizeof( L_TagMinor );

						_used_size += size;

						p = ( void* ) ( ( uint64 ) min + sizeof( L_TagMinor ) );

						_align( p );

						_lock.release();
						return p;
					}
				} // min->next == nullptr

				// CASE 4.2 : Look up whether there is enough space between tow minors
				else
				{
					diff = ( int64 ) ( ( uint64 ) min->next - ( uint64 ) min - sizeof( L_TagMinor ) - min->size );
					if ( diff >= size + ( int64 ) sizeof( L_TagMinor ) )
					{
						L_TagMinor * new_min = ( L_TagMinor* ) ( ( uint64 ) min + sizeof( L_TagMinor ) + min->size );

						new_min->magic = _liballoc_magic_;
						new_min->next = min->next;
						new_min->prev = min;
						new_min->size = size;
						new_min->req_size = req_size;
						new_min->chunk = maj;
						min->next->prev = new_min;
						min->next = new_min;
						maj->usage += size + sizeof( L_TagMinor );

						_used_size += size;

						p = ( void * ) ( ( uint64 ) new_min + sizeof( L_TagMinor ) );
						_align( p );

						_lock.release();
						return p;
					}
				}

				// look up next minor
				min = min->next;
			}

			// CASE 5 : Chunk full and no next chunk! Allocate a new chunk and loop it
			if ( maj->next == nullptr )
			{
				if ( started_bet )
				{
					maj = _mem_root;
					started_bet = false;
					continue;
				}

				// run out of all chunks, need to allocate new one
				maj->next = _allocate_new_chunk( size );
				if ( maj->next == nullptr )
				{
					printfYellow( "L-allocator : no memory to allocate for the major chunk\n" );
					break;
				}
				maj->next->prev = maj;
			}

			maj = maj->next;
		} // while ( maj )

		_lock.release();

		printfYellow( "L-allocator : All cases exhausted. No memory available." );
		return nullptr;
	} // L_Allocator::malloc()


	void L_Allocator::free( void *ptr )
	{
		if ( ptr == nullptr )
			return;

		_unalign( ptr );

		_lock.acquire();

		L_TagMinor * min = ( L_TagMinor* ) ( ( uint64 ) ptr - sizeof( L_TagMinor ) );

		if ( min->magic != _liballoc_magic_ )
		{
			// Check for overrun errors. For all bytes of _liballoc_magic_ 
			if (
				( ( min->magic & 0xFFFFFF ) == ( _liballoc_magic_ & 0xFFFFFF ) ) ||
				( ( min->magic & 0xFFFF ) == ( _liballoc_magic_ & 0xFFFF ) ) ||
				( ( min->magic & 0xFF ) == ( _liballoc_magic_ & 0xFF ) )
				)
			{
				panic( "L-allocator : Possible 1-3 byte overrun for magic %x != %x",
					min->magic,
					_liballoc_magic_ );
			}


			if ( min->magic == _liballoc_dead )
			{
				panic( "L-allocator : Multiple free() attempt on %x.", ptr );
			}
			else
			{
				panic( "L-allocator : Bad free( %x ).", ptr );
			}

			// being lied to...
			_lock.release();		// release the lock
			return;
		}

		L_TagMajor * maj = min->chunk;

		_used_size -= min->size;
		maj->usage -= ( min->size + sizeof( L_TagMinor ) );
		min->magic = _liballoc_dead;

		if ( min->next != nullptr ) min->next->prev = min->prev;

		if ( min->prev != nullptr ) min->prev->next = min->next;
		if ( min->prev == nullptr ) maj->first = min->next;

		if ( maj->first )
		{
			if ( _best_bet )
			{
				int64 best_size = ( int64 ) ( _best_bet->size - _best_bet->usage );
				int64 maj_size = ( int64 ) ( maj->size - maj->usage );
				if ( maj_size > best_size )
					_best_bet = maj;
			}
		}
		else	// chunk completely unused 
		{
			if ( _mem_root == maj )
				_mem_root = maj->next;
			if ( _best_bet == maj )
				_best_bet = nullptr;
			if ( maj->prev )
				maj->prev->next = maj->next;
			if ( maj->next )
				maj->next->prev = maj->prev;

			_cach_size -= maj->size; //以字节为单位的
			_base_allocator->free_pages(maj);//此处必须和alloc_pages成对使用
		}

		_lock.release();

	} // L_Allocator::free()


// -------- private helper function -------- 

	L_TagMajor * L_Allocator::_allocate_new_chunk( uint64 size )
	{
		assert( _base_allocator != nullptr, "L-allocator : try to cache chunks while not inited!\n" );

		uint64 st;

		st = size + sizeof( L_TagMajor ) + sizeof( L_TagMinor );

		// pages occupied
		st += PGSIZE - 1;
		st /= PGSIZE;

		// make sure it's greater than the min pages
		if ( st < _page_per_chunk )
			st = _page_per_chunk;
		//使用buddy系统分配内存,buddy系统分配的内存以页的数量为单位，st是页的数量
		//此时alloc_pages返回的是内存的虚拟地址，若使用Alloc，则返回的是相对基址的偏移量
		L_TagMajor * maj = ( L_TagMajor* ) _base_allocator->alloc_pages(st);
		if ( maj == nullptr )
		{
			printfYellow( "L-allocator : try to request %d pages but no memory to use.\n", st );
			return nullptr;
		}

		maj->prev = maj->next = nullptr;
		maj->pages = st;
		maj->size = st * PGSIZE;
		maj->usage = sizeof( L_TagMajor );
		maj->first = nullptr;

		_cach_size += maj->size;

		return maj;
	}

	void L_Allocator::_align( void * &ptr )
	{
		if ( _use_align_ )
		{
			uint64 diff;
			ptr = ( void* ) ( ( uint64 ) ptr + _align_info_length_ );
			diff = ( uint64 ) ptr & ( _alignment_ - 1 );
			if ( diff != 0 )
			{
				diff = _alignment_ - diff;
				ptr = ( void* ) ( ( uint64 ) ptr + diff );
			}
			*( ( _AlignType_ * ) ( ( uint64 ) ptr - _align_info_length_ ) ) =
				diff + _align_info_length_;
		}
	}

	void L_Allocator::_unalign( void * &ptr )
	{
		if ( _use_align_ )
		{
			uint64 diff = *( ( _AlignType_* ) ( ( uint64 ) ptr - _align_info_length_ ) );
			if ( diff < ( _alignment_ + _align_info_length_ ) )
			{
				ptr = ( void* ) ( ( uint64 ) ptr - diff );
			}
		}
	}
} // namespace kernellib
