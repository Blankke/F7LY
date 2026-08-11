//
// Copied from Li Shuang ( pseudonym ) on 2024-05-17 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#include <cxxabi.h>

#include "__cxx_abi.hh"
#include "common.hh"

extern "C" {
	void *__dso_handle = 0;
}

namespace __cxxabiv1
{
	extern "C" {

/* **************** cxa guard **************** */

		int __cxa_guard_acquire( __guard * g )
		{
			// Itanium ABI 要求 guard 的首字节在初始化完成后非零。用第二字节
			// 表示“正在构造”，可让多个 CPU 竞争函数内 static 时只有一个
			// 执行构造函数，其余等待 release/abort，而不是同时写同一对象。
			constexpr uint64 guard_ready = 1;
			constexpr uint64 guard_running = 1ULL << 8;
			auto *state = reinterpret_cast<uint64 *>( g );

			for ( ;; )
			{
				uint64 observed = __atomic_load_n( state, __ATOMIC_ACQUIRE );
				if ( observed & guard_ready )
				{
					return 0;
				}
				if ( observed == 0 )
				{
					uint64 expected = 0;
					if ( __atomic_compare_exchange_n( state, &expected,
							guard_running, false, __ATOMIC_ACQ_REL,
							__ATOMIC_ACQUIRE ) )
					{
						return 1;
					}
					continue;
				}
				asm volatile( "" ::: "memory" );
			}
		}

		void __cxa_guard_release( __guard *g )
		{
			__atomic_store_n( reinterpret_cast<uint64 *>( g ), 1ULL,
				__ATOMIC_RELEASE );
		}

		void __cxa_guard_abort( __guard *g )
		{
			// 本项目禁用异常，但保留 ABI 正确的回滚语义；若未来构造路径
			// 显式调用 abort，其他 CPU 可以重新竞争初始化权。
			__atomic_store_n( reinterpret_cast<uint64 *>( g ), 0ULL,
				__ATOMIC_RELEASE );
		}

/* **************** cxa at_exit **************** */

		atexit_func_entry_t __atexit_funcs[ ATEXIT_MAX_FUNCS ];
		atexit_func_entry_t __atexit_func_entry_free_list;
		atexit_func_entry_t __atexit_func_entry_busy_list;

		uarch_t __atexit_func_count = 0;

		void __init_atexit_func_entry( void )
		{
			// 空闲链表和已登记链表都使用循环哨兵。此前 busy 链表以 nullptr
			// 初始化，__cxa_finalize() 会在空表上直接解引用空指针。
			__atexit_func_entry_busy_list._next_entry = &__atexit_func_entry_busy_list;
			__atexit_func_entry_busy_list._prev_entry = &__atexit_func_entry_busy_list;

			__atexit_func_entry_free_list._next_entry = &__atexit_func_entry_free_list;
			__atexit_func_entry_free_list._prev_entry = &__atexit_func_entry_free_list;

			for ( int i = 0; i < ATEXIT_MAX_FUNCS; i++ )
			{
				__insert_atexit_func_entry_list( &__atexit_func_entry_free_list, &__atexit_funcs[ i ] );
			}
		}

		/****************************************************************************
		 * about atexit abi, refer to:
		 * 1. https://itanium-cxx-abi.github.io/cxx-abi/abi.html#dso-dtor-runtime-api
		 * 2. https://wiki.osdev.org/C%2B%2B#GCC
		 ****************************************************************************/

		int __cxa_atexit( void ( *f )( void * ), void *objptr, void *dso )
		{
			Info( ">>>> __cxa_atexit called" );
			if ( __atexit_func_count >= ATEXIT_MAX_FUNCS )
			{
				return -1;
			}
			if ( __atexit_func_entry_list_is_empty( &__atexit_func_entry_free_list ) )
			{
				panic(
					"atexit: no func entry to use but __atexit_func_count not has a valid value,\n"
					"        which makes code executed at here to avoid bad result."
				);
			}

			atexit_func_entry_t* entry = __atexit_func_entry_free_list._next_entry;
			__remove_atexit_func_entry_list( entry );

			entry->destructor_func = f;
			entry->obj_ptr = objptr;
			entry->__dso_handle = dso;
			__insert_atexit_func_entry_list( &__atexit_func_entry_busy_list, entry );
			__atexit_func_count++;
			return 0;
		};

		void __cxa_finalize( void *d )
		{
			atexit_func_entry_t* fentry = __atexit_func_entry_busy_list._next_entry;
			Info(
				">>>> __cxa_finalize called"
				">>>> d is %p",
				d
			);
			if ( !d )
			{
				while ( fentry != &__atexit_func_entry_busy_list )
				{
					atexit_func_entry_t* next_entry = fentry->_next_entry;
					if ( fentry->destructor_func )
					{
						( *fentry->destructor_func )( fentry->obj_ptr );
						__remove_atexit_func_entry_list( fentry );
						__insert_atexit_func_entry_list( &__atexit_func_entry_free_list, fentry );
						--__atexit_func_count;
					}
					fentry = next_entry;
				}
				return;
			}

			while ( fentry != &__atexit_func_entry_busy_list )
			{
				atexit_func_entry_t* next_entry = fentry->_next_entry;
				if ( fentry->__dso_handle == d )
				{
					( *fentry->destructor_func )( fentry->obj_ptr );
					__remove_atexit_func_entry_list( fentry );
					__insert_atexit_func_entry_list( &__atexit_func_entry_free_list, fentry );
					--__atexit_func_count;
				}
				fentry = next_entry;
			}
		};

	}
} // namespace __cxxabiv1
