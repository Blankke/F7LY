#pragma once
#include "types.hh"
#include "syscall_defs.hh"

namespace syscall{
    constexpr uint max_syscall_funcs_num = 2048;
	constexpr uint max_path_len = 128;
	constexpr uint max_arg_num = 32;

	class SyscallHandler
	{
		//使用了这个类的成员函数指针
		using SyscallFunc = uint64 (SyscallHandler::*)(); // 定义一个函数指针类型 SyscallFunc
	private:
		SyscallFunc _syscall_funcs[ max_syscall_funcs_num ]; // 存储系统调用函数的指针数组
		const char * _syscall_name[ max_syscall_funcs_num ]; // 存储系统调用名称的指针数组
		uint64_t _default_syscall_impl(); // 默认的系统调用实现
	public:
		SyscallHandler() {};
		void init();
		uint64 invoke_syscaller( uint64 sys_num ); // 调用系统调用


	private:
		int _fetch_addr( uint64 addr, uint64 &out_data );
		int _fetch_str( uint64 addr, void *buf, uint64 max );
		uint64 _arg_raw( int arg_n );

		int _arg_int( int arg_n, int &out_int );
		int _arg_addr( int arg_n, uint64 &out_addr );
		int _arg_str( int arg_n, char *buf, int max );



	private:	// ================ syscall functions ================
		
       uint64 sys_exec        ();
       uint64 sys_fork        ();
       uint64 sys_exit        ();
       uint64 sys_wait        ();
       uint64 sys_wait4       ();
       uint64 sys_getppid     ();
       uint64 sys_getpid      ();
       uint64 sys_pipe2       ();
       uint64 sys_dup3        ();
       uint64 sys_read        ();
       uint64 sys_kill        ();
       uint64 sys_execve      ();
       uint64 sys_fstat       ();
       uint64 sys_statx       ();
       uint64 sys_chdir       ();
       uint64 sys_dup         ();
       uint64 sys_sleep       ();
       uint64 sys_uptime      ();
       uint64 sys_openat      ();
       uint64 sys_write       ();
       uint64 sys_unlinkat    ();
       uint64 sys_linkat      ();
       uint64 sys_mkdirat     ();
       uint64 sys_close       ();
       uint64 sys_mkn         ();
       uint64 sys_clone       ();
       uint64 sys_umount2     ();
       uint64 sys_mount       ();
       uint64 sys_brk         ();
       uint64 sys_munmap      ();
       uint64 sys_mmap        ();
       uint64 sys_times       ();
       uint64 sys_uname       ();
       uint64 sys_sched_yield ();
       uint64 sys_gettimeofday();
       uint64 sys_nanosleep   ();
       uint64 sys_getcwd      ();
       uint64 sys_getdents64  ();
       uint64 sys_shutdown    ();  
	};

	extern SyscallHandler k_syscall_handler;





}