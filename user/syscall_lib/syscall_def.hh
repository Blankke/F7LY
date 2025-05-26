// #include "kernel/sys/syscall_defs.hh"

#pragma once
namespace syscall
{

 enum SysNum{
 SYS_exec         =       55,    
 SYS_fork         =       1,
 SYS_exit         =       93,
 SYS_wait         =       3,
 SYS_wait4        =       260,
 SYS_getppid      =       173,
 SYS_getpid       =       172,
 SYS_pipe2        =       59,
 SYS_dup3         =       24,
 SYS_read         =       63,
 SYS_kill         =       6,
 SYS_execve       =       221,
 SYS_fstat        =       80,
 SYS_statx        =       291,
 SYS_chdir        =       49,
 SYS_dup          =       23,
 SYS_sleep        =       13,
 SYS_uptime       =       14,
 SYS_openat       =       56,
 SYS_write        =       64,
 SYS_unlinkat     =       35,
 SYS_linkat       =       37,
 SYS_mkdirat      =       34,
 SYS_close        =       57,
 SYS_mkn          =       85,
 SYS_clone        =       220,
 SYS_umount2      =       39,
 SYS_mount        =       40,
 SYS_brk          =       214,
 SYS_munmap       =       215,
 SYS_mmap         =       222,
 SYS_times        =       153,
 SYS_uname        =       160,
 SYS_sched_yield  =       124,
 SYS_gettimeofday =       169,
 SYS_nanosleep    =       101,
 SYS_getcwd       =       17,
 SYS_getdents64   =       61,
 SYS_shutdown     =       19
 };
 
}