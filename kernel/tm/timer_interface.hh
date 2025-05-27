//
// Created by Li Shuang ( pseudonym ) on 2024-07-12 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once
#include "printer.hh"
namespace tmm
{
	extern  int handle_tick_intr();

	extern  ulong get_ticks();



	extern  ulong get_main_frequence();

	extern  ulong cycles_per_tick();

	extern  ulong get_hw_time_stamp();

	extern  ulong time_stamp_to_usec( ulong ts );

	extern  ulong usec_to_time_stamp( ulong us );

} // namespace tmm
