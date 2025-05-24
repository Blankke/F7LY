//
// Copy from Li shuang ( pseudonym ) on 2024-03-26 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once 

#include "types.hh"

namespace loongarch
{
	namespace csr
	{
		enum CsrAddr 
		{
			crmd = 0x0,
			cpuid = 0x20,
			save0 = 0x30,
			dmwin0 = 0x180,
			dmwin1 = 0x181,
			dmwin2 = 0x182,
			dmwin3 = 0x183,
			tlbehi = 0x11,
			pgdl = 0x19,
			pgd = 0x1b,
			tlbrentry = 0x88,
			tlbrbadv = 0x89,
			tlbrsave = 0x8b,
		};


		/// @brief	s: shift;
		///			m: mask;
		enum crmd
		{
			plv_s = 0x0,
			plv_m = 0x3 << plv_s,
			ie_s = 0x2,
			ie_m = 0x1 << ie_s,
			da_s = 0x3,
			da_m = 0x1 << da_s,
			pg_s = 0x4,
			pg_m = 0x1 << pg_s,
			datf_s = 0x5,
			datf_m = 0x3 << datf_s,
			datm_s = 0x7,
			datm_m = 0x3 << datm_s,
			we_s = 0x9,
			we_m = 0x1 << we_s,
		};


	};

    // namespace csr
	/**
	 * @brief 页表项字段定义宏
	 */
#define _build_pte_bit_(name, mask, shift) \
	pte_##name##_s = shift,                \
	pte_##name##_m = mask << pte_##name##_s,

	/**
	 * @brief 页表项各个位域的枚举
	 */
	enum PteEnum
	{
		pte_valid_s = 0,
		pte_dirty_s = 1,
		pte_plv_s = 2,
		pte_mat_s = 4,
		pte_b_global_s = 6,		// base pte global bit shift 
		pte_h_huge_s = 6, 		// huge pte huge flag bit shift 
		pte_presence_s = 7,
		pte_writable_s = 8,
		pte_b_pa_s = 12,		// base pte physical address shift 
		pte_h_global_s = 12, 	// huge pte global bit shift 
		pte_h_pa_s = 15, 		// huge pte physical address shift 
		pte_nr_s = 61,
		pte_nx_s = 62,
		pte_rplv_s = 63,

		pte_valid_m = 0x1UL << pte_valid_s,
		pte_dirty_m = 0x1UL << pte_dirty_s,
		pte_plv_m = 0x3UL << pte_plv_s,
		pte_mat_m = 0x3UL << pte_mat_s,
		pte_b_global_m = 0x1UL << pte_b_global_s,				// base pte global bit mask 
		pte_h_huge_m = 0x1UL << pte_h_huge_s, 				// huge pte huge flag bit mask 
		pte_presence_m = 0x1UL << pte_presence_s,
		pte_writable_m = 0x1UL << pte_writable_s,
		pte_b_pa_m = 0x1FFFFFFFFFFFFUL << pte_b_pa_s, 	// base pte pa mask [ 12...60 ]
		pte_h_global_m = 0x1UL << pte_h_global_s, 			// huge pte global bit mask 
		pte_h_pa_m = 0x3FFFFFFFFFFFUL << pte_h_pa_s, 	// huge pte pa mask [ 15..60 ]
		pte_nr_m = 0x1UL << pte_nr_s,
		pte_nx_m = 0x1UL << pte_nx_s,
		pte_rplv_m = 0x1UL << pte_rplv_s,

		pte_b_flags_m = pte_valid_m | pte_dirty_m | pte_plv_m | pte_mat_m | pte_b_global_m | pte_presence_m
			| pte_writable_m | pte_nr_m | pte_nx_m | pte_rplv_m, 
	};
#undef _build_pte_bit_
};

    // namespace loongarch