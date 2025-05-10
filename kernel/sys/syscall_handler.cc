#include "syscall_handler.hh"
#include "printer.hh"
#include "proc.hh"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"
#include "klib.hh"
#include "list.hh"
#include "param.h"
namespace syscall{

#define BIND_SYSCALL(sysname)                         \
    _syscall_funcs[SYS_##sysname] = &SyscallHandler::sys_##sysname; \
    _syscall_name[SYS_##sysname] = #sysname;

    // 新的默认系统调用处理成员函数的实现
    // 请确保在 SyscallHandler 类的头文件中声明此函数:
    // private: uint64 _default_syscall_impl();
    uint64 SyscallHandler::_default_syscall_impl() {
        printfYellow("Syscall not implemented\n");
        return 0;
    }

    void SyscallHandler::init(){
       for(auto&func : _syscall_funcs){
        //默认实现
        func = &SyscallHandler::_default_syscall_impl;
    }
    //初始化系统调用名称
    for(auto&name : _syscall_name){
        name = nullptr;
    }
    BIND_SYSCALL(exit);
    BIND_SYSCALL(exec);
    BIND_SYSCALL(fork);
    BIND_SYSCALL(exit);
    BIND_SYSCALL(wait);
    BIND_SYSCALL(wait4);
    BIND_SYSCALL(getppid);
    BIND_SYSCALL(getpid);
    BIND_SYSCALL(pipe2);
    BIND_SYSCALL(dup3);
    BIND_SYSCALL(read);
    BIND_SYSCALL(kill);
    BIND_SYSCALL(execve);
    BIND_SYSCALL(fstat);
    BIND_SYSCALL(statx);
    BIND_SYSCALL(chdir);
    BIND_SYSCALL(dup);
    BIND_SYSCALL(sleep);
    BIND_SYSCALL(uptime);
    BIND_SYSCALL(openat);
    BIND_SYSCALL(write);
    BIND_SYSCALL(unlinkat);
    BIND_SYSCALL(linkat);
    BIND_SYSCALL(mkdirat);
    BIND_SYSCALL(close);
    BIND_SYSCALL(mkn);
    BIND_SYSCALL(clone);
    BIND_SYSCALL(umount2);
    BIND_SYSCALL(mount);
    BIND_SYSCALL(brk);
    BIND_SYSCALL(munmap);
    BIND_SYSCALL(mmap);
    BIND_SYSCALL(times);
    BIND_SYSCALL(uname);
    BIND_SYSCALL(sched_yield);
    BIND_SYSCALL(gettimeofday);
    BIND_SYSCALL(nanosleep);
    BIND_SYSCALL(getcwd);
    BIND_SYSCALL(getdents64);
    BIND_SYSCALL(shutdown);
    }
    uint64 SyscallHandler::invoke_syscaller(uint64 sys_num){
        if (sys_num >= max_syscall_funcs_num || _syscall_funcs[sys_num] == nullptr) {
            // 你也可以 throw 异常或返回错误码
            return -1;
        }
        return (this->*_syscall_funcs[sys_num])();
    }

	// ---------------- private helper functions ----------------

	int SyscallHandler::_fetch_addr( uint64 addr, uint64 &out_data )
	{
		proc::Pcb		  *p  = (proc::Pcb *)proc::k_pm.get_cur_pcb();
		// if ( addr >= p->get_size() || addr + sizeof( uint64 ) > p->get_size()
		// ) 	return -1;
		mem::PageTable pt = p->get_pagetable();
		if ( mem::k_vmm.copy_in( pt, &out_data, addr, sizeof( out_data ) ) < 0 )
			return -1;
		return 0;
	}

	int SyscallHandler::_fetch_str( uint64 addr, void *buf, uint64 max )
	{
		proc::Pcb		  *p   = (proc::Pcb *) proc::k_pm.get_cur_pcb();
		mem::PageTable pt  = p->get_pagetable();
		int			   err = mem::k_vmm.copy_str_in( pt, buf, addr, max );
		if ( err < 0 ) return err;
		return strlen( (const char *) buf );
	}

	uint64 SyscallHandler::_arg_raw( int arg_n )
	{
		proc::Pcb* p = (proc::Pcb*)proc::k_pm.get_cur_pcb();
        switch(arg_n){
            case 0:
                return p->get_trapframe()->a0;
            case 1:
                return p->get_trapframe()->a1;
            case 2:
                return p->get_trapframe()->a2;
            case 3:
                return p->get_trapframe()->a3;
            case 4:
                return p->get_trapframe()->a4;
            case 5:
                return p->get_trapframe()->a5;
        }
        panic("[SyscallHandler::_arg_raw]arg_n is out of range");
        return -1;
	}
    int SyscallHandler::_arg_int(int arg_n,int &out_int){
        uint64 raw_val = _arg_raw(arg_n);
        if(raw_val < (uint64)INT_MIN || raw_val > (uint64)INT_MAX){
            printfRed("[SyscallHandler::_arg_int]arg_n is out of range");
            return -1;
        }
        out_int = (int)raw_val;
        return 0;
    }
    int SyscallHandler::_arg_addr(int arg_n,uint64 &out_addr){
        uint64 raw_val = _arg_raw(arg_n);
        if(raw_val < 0 || raw_val > UINT64_MAX){
            printfRed("[SyscallHandler::_arg_addr]arg_n is out of range");
            return -1;
        }
        out_addr = raw_val;
        return 0;
    }
    int SyscallHandler::_arg_str(int arg_n,char *buf,int max){
        uint64 addr;
        if(_arg_addr(arg_n,addr) < 0){
            printfRed("[SyscallHandler::_arg_str]arg_n is out of range");
            return -1;
        }
        return _fetch_str(addr,buf,max);
    }

    // ---------------- syscall functions ----------------
    uint64 SyscallHandler::sys_exec(){
        TODO("sys_exec");
        printfYellow("sys_exec\n");
        return 0;
        }
    uint64 SyscallHandler::sys_fork(){
        TODO("sys_fork");
        printfYellow("sys_fork\n");
        return 0;
    }
    uint64 SyscallHandler::sys_exit(){
        TODO("sys_exit");
        printfYellow("sys_exit\n");
        return 0;
    }
    uint64 SyscallHandler::sys_wait(){
        TODO("sys_wait");
        printfYellow("sys_wait\n");
        return 0;
    }
    uint64 SyscallHandler::sys_wait4(){
        TODO("sys_wait4");
        printfYellow("sys_wait4\n");
        return 0;
    }
    uint64 SyscallHandler::sys_getppid(){
        TODO("sys_getppid");
        printfYellow("sys_getppid\n");
        return 0;
    }
    uint64 SyscallHandler::sys_getpid(){
        TODO("sys_getpid");
        printfYellow("sys_getpid\n");
        return 0;
    }
    uint64 SyscallHandler::sys_pipe2(){
        TODO("sys_pipe2");
        printfYellow("sys_pipe2\n");
        return 0;
    }
    uint64 SyscallHandler::sys_dup3(){
        TODO("sys_dup3");
        printfYellow("sys_dup3\n");
        return 0;
    }
    uint64 SyscallHandler::sys_read(){
        TODO("sys_read");
        printfYellow("sys_read\n"); 
        return 0;
    }
    uint64 SyscallHandler::sys_kill(){
        TODO("sys_kill");
        printfYellow("sys_kill\n");
        return 0;
    }   
    uint64 SyscallHandler::sys_execve(){
        TODO("sys_execve");
        printfYellow("sys_execve\n");
        return 0;
    }
    
}//namespace syscall
