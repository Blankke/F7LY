#pragma once
#include "types.hh"
#include "memlayout.hh"
#include "platform.hh"
#include "rv_csr.hh"
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
		Pte(pte_t *addr) : _data_addr(addr) {};
		void set_addr(pte_t *addr);
		bool is_null();

		bool is_valid();
		bool is_writable();
		bool is_readable();
		bool is_executable();
		bool is_user();
		bool is_leaf();
		bool is_present(); 
		void set_valid();
		void set_writable();
		void set_readable();
		void set_executable();
		void set_user();
		void set_data(uint64 data);
		uint64 get_flags();
		void *pa();

		// 慎用！！！这个函数会使PTE的值清零！
		void clear_data();
		uint64 get_data();

		
	};
}