//
// Copy from Li shuang ( pseudonym ) on 2024-04-02 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once 
#ifdef LOONGARCH
#include "types.hh"
#include "la_csr.hh"
#include "platform.hh"


namespace mem
{
	class PageTable;

	/// @brief page table entry 
	class Pte
	{
		friend PageTable;
	private:
		pte_t *_data_addr = 0;
	public:
		Pte() = default;
		Pte( pte_t *addr ) : _data_addr( addr ) {};
		void set_addr( pte_t *addr ) { _data_addr = addr; }
		bool is_null() { return _data_addr == 0; }

		bool is_valid() { return ( ( *_data_addr & loongarch::pte_valid_m ) != 0 ); }
		bool is_dirty() { return ( ( *_data_addr & loongarch::pte_dirty_m ) != 0 ); }
		uint64 plv() { return ( ( *_data_addr & loongarch::pte_plv_m ) >> loongarch::pte_plv_s ); }
		uint64 mat() { return ( ( *_data_addr & loongarch::pte_mat_m ) >> loongarch::pte_mat_s ); }
		uint64 flags() { return *_data_addr & loongarch::pte_b_flags_m; }
		void set_plv() { *_data_addr |= loongarch::pte_plv_m; }
		void unset_plv() { *_data_addr &= ~loongarch::pte_plv_m; }
		bool is_global() { return ( ( *_data_addr & loongarch::pte_b_global_m ) != 0 ); }
		bool is_present() { return ( ( *_data_addr & loongarch::pte_presence_m ) != 0 ); }
		bool is_writable() { return ( ( *_data_addr & loongarch::pte_writable_m ) != 0 ); }
		void *pa() { return ( void* ) PTE2PA( *_data_addr ); }
		bool is_readable() { return ( ( *_data_addr & loongarch::pte_nr_m ) == 0 ); }
		bool is_executable() { return ( ( *_data_addr & loongarch::pte_nx_m ) == 0 ); }
		bool is_restrict_plv() { return ( ( *_data_addr & loongarch::pte_rplv_m ) != 0 ); }
		bool is_leaf() { return ( ( PTE_FLAGS( *_data_addr ) ) != 1 ); }

		void set_data( uint64 data ) { *_data_addr |= data; }

		// 慎用！！！这个函数会使PTE的值清零！
		void clear_data() { *_data_addr = 0; }
		uint64 get_data() { return *_data_addr; }

	};
}
#endif // LOONGARCH