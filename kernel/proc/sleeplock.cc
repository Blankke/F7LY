
#include "sleeplock.hh"
#include "proc_manager.hh"


namespace proc
{
	void SleepLock::init( const char *lock_name, const char *name )
	{
		_lock.init( lock_name );
		_name = name;
		_locked = false;
		_pid = 0;
	}

	void SleepLock::acquire()
	{
		_lock.acquire();
		while ( _locked )
			proc::k_pm.sleep(this, &_lock);
		_locked = true;
		_pid = k_pm.get_cur_pcb()->get_pid();
		_lock.release();
	}

	void SleepLock::release()
	{
		_lock.acquire();
		_locked = 0;
		_pid = 0;
		proc::k_pm.wakeup(this);
		_lock.release();
	}

	bool SleepLock::is_holding()
	{
		bool held;
		_lock.acquire();
		held = _locked && ( _pid == k_pm.get_cur_pcb()->get_pid() );
		_lock.release();
		return held;
	}
} // namespace proc
