//
// Created by Li shuang ( pseudonym ) on 2024-03-26 
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
		// _build_pte_bit_(valid, 0x1, 0)		// valid
		// _build_pte_bit_(readable, 0x1, 1)	// readable
		// _build_pte_bit_(writable, 0x1, 2)	// writable
		// _build_pte_bit_(executable, 0x1, 3) // executable
		// _build_pte_bit_(user, 0x1, 4)		// user can access

		_build_pte_bit_(valid, 0x1, 0)      // V: 有效位 (1 = 该PTE有效)
		_build_pte_bit_(dirty, 0x1, 1)      // D: 脏位 (1 = 该页被修改过)
		_build_pte_bit_(plv, 0x3, 2)      // D: 脏位 (1 = 该页被修改过)
		_build_pte_bit_(user,3L,2)
		_build_pte_bit_(mat,1L,4)   //Memory Access Type
		_build_pte_bit_(phy,1L,7) //Physical page exists
		_build_pte_bit_(write,1,8)  //W: 可写位 (1 = 该页可写)
		_build_pte_bit_(NX,1UL,62) // NX: 不可执行位 (1 = 该页不可执行)
		_build_pte_bit_(NR,1L,62) // NR: 不可读位 (1 = 该页不可读)
		_build_pte_bit_(RPLV,1L,63) //restricted privilege level enable
		_build_pte_bit_(read,0,0) //R
		_build_pte_bit_(X,0,0) //X

		// // 物理页号和标志掩码
		// pte_ppn_s = 10,                     // PPN: 物理页号起始位
		// pte_ppn_m = 0xFFFFFFFFFFFULL << pte_ppn_s, // 44位物理页号掩码

		// // 标志位整体掩码
		// pte_flags_m = pte_valid_m | pte_readable_m | pte_writable_m | pte_executable_m |
		//              pte_user_m | pte_global_m | pte_accessed_m | pte_dirty_m | pte_rsw_m,
	};
#undef _build_pte_bit_
};

    // namespace loongarch