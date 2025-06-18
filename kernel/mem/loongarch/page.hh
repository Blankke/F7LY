//
// Copy from Li shuang ( pseudonym ) on 2024-03-28 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once 
#ifdef LOONGARCH
#include "types.hh"
#include "spinlock.hh"
#ifndef PG_SIZE
#define PG_SIZE (4*_1K)
#endif

#ifndef PN_MASK
#define PN_MASK 0x1FFUL
#endif
namespace mem
{
	/// @brief page enum for page info 
	/// @details __________________________________________________
	/// | the paging schema: 
	/// | 	63 ... 48 -- must be zero 
	/// | 	47 ... 39 -- 9 bits of level-3 virtual page number 
	/// | 	38 ... 30 -- 9 bits of level-2 virtual page number 
	/// | 	29 ... 21 -- 9 bits of level-1 virtual page number 
	/// | 	20 ... 12 -- 9 bits of last level virtual page number 
	///	|	11 ... 0  -- 12 bits of byte offset within the page 
	/// | _________________________________________________________
	enum PageEnum : uint64
	{
		pg_size_shift = 12,
		pg_size = 0x1 << pg_size_shift, 		// 4KiB page 
		pg_offset_mask = 0xFFFUL,

		pt_bits_null = 0, 
		pt_bits_width = 9,
		pt_bits_mask = 0x1FFUL,
		pt_vpn_shift = pg_size_shift,
		pt_vpn_mask = pt_bits_mask << pt_vpn_shift,  // 0x FFFFFFFFF000  [39:12]

		dir1_vpn_shift = 21,
		dir2_vpn_shift = 30, 
		dir3_vpn_shift = 39,
		dir4_vpn_shift = 48,

		dir1_vpn_mask = pt_bits_mask << dir1_vpn_shift,
		dir2_vpn_mask = pt_bits_mask << dir2_vpn_shift,
		dir3_vpn_mask = pt_bits_mask << dir3_vpn_shift,

		pte_width = 0x0UL 
	};

	/// @brief This struct just be used to unused physical page;
	/// @details A simple link list connect all unused page;
	struct PageHead
	{
		struct PageHead *_next;
	};


#define PT_LEVEL 4
	constexpr ulong page_size = PG_SIZE;
	static_assert( ( page_size & ( page_size - 1 ) ) == 0 );
	constexpr ulong page_size_shift = [] () -> ulong
	{
		ulong i = 1;
		ulong k = 0;
		for ( ; ( i & page_size ) == 0; i <<= 1, ++k );
		return k;
	}( );

	constexpr ulong page_number_mask = PN_MASK;
	constexpr ulong page_number_mask_width = [] ()->ulong
	{
		ulong i = 1;
		ulong k = 0;
		for ( ; ( i & page_number_mask ) != 0; i <<= 1, ++k );
		return k;
	}( );

	constexpr ulong page_round_up( ulong addr )
	{
		ulong pg_sz = page_size;
		return ( ( addr + pg_sz - 1 ) & ~( pg_sz - 1 ) );
	}

	constexpr ulong page_round_down( ulong addr )
	{
		ulong pg_sz = page_size;
		return ( addr & ~( pg_sz - 1 ) );
	}

	constexpr bool is_page_align( uint64 addr )
	{
		ulong pg_sz = page_size;
		return ( addr & ( pg_sz - 1 ) ) == 0;
	}

#if PT_LEVEL == 4
	constexpr ulong pgd_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 3 );
	constexpr ulong pud_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 2 );
	constexpr ulong pmd_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 1 );
	constexpr ulong pt_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 0 );
#endif
#if PT_LEVEL == 3
	constexpr ulong pgd_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 2 );
	constexpr ulong pud_mask = 0;
	constexpr ulong pmd_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 1 );
	constexpr ulong pt_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 0 );
#endif
#if PT_LEVEL == 2
	constexpr ulong pgd_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 1 );
	constexpr ulong pud_mask = 0;
	constexpr ulong pmd_mask = 0;
	constexpr ulong pt_mask = page_number_mask << page_size_shift << ( page_number_mask_width * 0 );
#endif

#if PT_LEVEL > 4 || PT_LEVEL < 2
#error "不支持的页表级别"
#endif

	constexpr ulong lowest_1_bit( ulong i )
	{
		ulong k = 1;
		ulong cnt = 0;
		for ( ; ( k & i ) == 0; k <<= 1, cnt++ );
		return cnt;
	}

	constexpr ulong pgd_mask_shift = pgd_mask == 0 ? 0 : lowest_1_bit( pgd_mask );
	constexpr ulong pud_mask_shift = pud_mask == 0 ? 0 : lowest_1_bit( pud_mask );
	constexpr ulong pmd_mask_shift = pmd_mask == 0 ? 0 : lowest_1_bit( pmd_mask );
	constexpr ulong pt_mask_shift = pt_mask == 0 ? 0 : lowest_1_bit( pt_mask );

	constexpr ulong pgd_num( ulong va )
	{
		return ( pgd_mask == 0 ) ? -1 : ( ( va & pgd_mask ) >> pgd_mask_shift );
	}

	constexpr ulong pud_num( ulong va )
	{
		return ( pud_mask == 0 ) ? -1 : ( ( va & pud_mask ) >> pud_mask_shift );
	}

	constexpr ulong pmd_num( ulong va )
	{
		return ( pmd_mask == 0 ) ? -1 : ( ( va & pmd_mask ) >> pmd_mask_shift );
	}

	constexpr ulong pt_num( ulong va )
	{
		return ( pt_mask == 0 ) ? -1 : ( ( va & pt_mask ) >> pt_mask_shift );
	}


	// enum PageStatus
	// {
	// 	undefined,		// error status 
	// 	free,			// free in kernel page list, only physical page using 
	// 	not_init, 		// has been alloc but not init 
	// 	used,			// is being used by process 
	// };


	// /// @brief virtual class for page 
	// class Page
	// {
	// public:

	// };
}

#endif // LOONGARCH