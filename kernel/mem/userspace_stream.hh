//
// Copied from Li shuang ( pseudonym ) on 2024-08-07
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#pragma once

#include <EASTL/string.h>
#include "printer.hh"
#include <pagetable.hh>
#include <virtual_memory_manager.hh>
#include <tuple>

#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined (LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif

namespace mem
{
	using UsBufPtr = u8 *;
	using UsBufLen = u64;
	using UsRangeDesc = std::tuple<UsBufPtr, UsBufLen>;

	class UserspaceStream
	{
		using PT = PageTable;

	public:
		enum RetCode
		{
			rc_ok,
			rc_fail,
			rc_not_open,
		};

	private:
		u64 _start_addr = 0; // 用户空间起始地址
		u64 _end_addr = 0;	 // 用户空间结束地址
		PT *_pt = nullptr;
		u64 _ptr = 0; // 流指针（用户空间）

		u8 *_cache_ptr = nullptr; // 缓存的页内部指针
		u8 *_cache_end = nullptr; // 缓存的页结束地址

		RetCode _errno = rc_ok;

	public:
		UserspaceStream() = default;
		UserspaceStream(void *u_addr_start, u64 u_size, PT *u_pt);

		RetCode open();
		RetCode close();

		RetCode errno() { return _errno; }
		u64 rest_space() { return _end_addr - _ptr; }

		UserspaceStream &operator<<(UsRangeDesc &rd);
		UserspaceStream &operator>>(UsRangeDesc &rd);

		template <typename T>
		UserspaceStream &operator>>(T &d)
		{
			UsRangeDesc rd{(u8 *)&d, sizeof(d)};
			return *this >> rd;
		}

		template <typename T>
		UserspaceStream &operator<<(T &d)
		{
			UsRangeDesc rd{(u8 *)&d, sizeof(d)};
			return *this << rd;
		}

		UserspaceStream &operator>>(char *str)
		{
			while (1)
			{
				*this >> *str;
				if (*str == 0)
					break;
				str++;
			}
			return *this;
		}
		UserspaceStream &operator<<(char *str)
		{
			while (1)
			{
				*this << *str;
				if (*str == '\0')
					break;
				str++;
			}
			return *this;
		}

		UserspaceStream &operator>>(eastl::string &str)
		{
			char c;
			str.clear();
			while (1)
			{
				*this >> c;
				if (c == 0)
					break;
				str.push_back(c);
			}
			return *this;
		}

		UserspaceStream &operator<<(eastl::string &str)
		{
			for (auto c : str)
			{
				*this << c;
			}
			char c = '\0';
			*this << c;
			return *this;
		}

	private:
	//"此处有风险，参考了rv版本的学长代码，to_vir()函数因为返回本身，故删除，la版本的代码未实现"
		int _update_cache(void *va)
		{
			// printfCyan("[UserspaceStream::_update_cache] va: %p\n", va);
			u64 a = (u64)va;
			if (is_page_align(a))
			{
#ifdef RISCV
				_cache_ptr = (u8 *)_pt->walk_addr(a);
				_cache_end = _cache_ptr + PGSIZE;
#elif defined(LOONGARCH)
				_cache_ptr = (u8 *)to_vir((ulong)_pt->walk_addr(a));
				_cache_end = _cache_ptr + PGSIZE;
#endif
				// printfGreen("[_update_cache] page aligned: _cache_ptr=%p, _cache_end=%p\n", _cache_ptr, _cache_end);
			}
			else
			{
#ifdef RISCV
				u64 page_start = PGROUNDDOWN(a);
				u64 offset = a - page_start;
				_cache_ptr = (u8 *)_pt->walk_addr(page_start) + offset;
				_cache_end = (u8 *)_pt->walk_addr(page_start) + PGSIZE;
#elif defined(LOONGARCH)
				u64 page_start = PGROUNDDOWN(a);
				u64 offset = a - page_start;
				_cache_ptr = (u8 *)to_vir((ulong)_pt->walk_addr(page_start)) + offset;
				_cache_end = (u8 *)to_vir((ulong)_pt->walk_addr(page_start)) + PGSIZE;
#endif
				// printfYellow("[_update_cache] not page aligned: page_start=%p, offset=%u, _cache_ptr=%p, _cache_end=%p\n", 
				// 	(void*)page_start, offset, _cache_ptr, _cache_end);
			}
			return 0;
		}
	};

} // namespace mem
