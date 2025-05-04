//
// Created by Li shuang ( pseudonym ) on 2024-03-26 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#include "spinlock.hh"
#include "../types.hh"

SpinLock::SpinLock() {}

void SpinLock::init( const char *name )
{
	this->_name = name;
	this->_locked = 0;
}

void SpinLock::acquire()
{
	// if ( is_held() )
		// log_panic( "SpinLock acquire: repeat" );
	while ( __sync_lock_test_and_set( &_locked, 1 ) != 0 )
		;
	__sync_synchronize();

}

void SpinLock::release()
{
	// if ( !is_held() )
	// 	log_panic("SpinLock release: not hold SpinLock"); 

	__sync_synchronize();
	__sync_lock_release( &_locked );

}
