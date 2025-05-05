#include "types.hh"
#include "memlayout.hh"
#include "platform.h"
#include"rv_csr.hh"
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

		bool is_valid() { return ( ( *_data_addr & riscv::PteEnum::pte_valid_m ) != 0 ); }
		bool is_writable() { return ( ( *_data_addr & riscv::PteEnum::pte_writable_m ) != 0 ); }
		bool is_readable() { return ( ( *_data_addr & riscv::PteEnum::pte_readable_m ) != 0 ); }
		bool is_executable() { return ( ( *_data_addr & riscv::PteEnum::pte_executable_m ) != 0 ); }
		bool is_user() { return ( ( *_data_addr & riscv::PteEnum::pte_user_m ) != 0 ); }
		void set_valid(  ) { *_data_addr |=  riscv::PteEnum::pte_valid_m ; }
		void set_writable(  ) { *_data_addr |=  riscv::PteEnum::pte_writable_m ; }
		void set_readable(  ) { *_data_addr |=  riscv::PteEnum::pte_readable_m ; }
		void set_executable(  ) { *_data_addr |=  riscv::PteEnum::pte_executable_m ; }
		void set_user(  ) { *_data_addr |=  riscv::PteEnum::pte_user_m ; }
		void set_data( uint64 data ) { *_data_addr |= data; }
		uint64 get_flags(){return *_data_addr & 0x3FF;}
		void* pa() { return PTE2PA(get_data()); }

		// 慎用！！！这个函数会使PTE的值清零！
		void clear_data() { *_data_addr = 0; }
		uint64 get_data() { return *_data_addr; }

	};
}