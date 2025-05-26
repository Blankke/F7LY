//
// Created by Li Shuang ( pseudonym ) on 2024-04-23 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#include "function.hh"
#include "common.hh"

namespace std
{
	void __throw_bad_function_call()
	{
		panic( "! >>> bad function call" );
	}
} // namespace std
