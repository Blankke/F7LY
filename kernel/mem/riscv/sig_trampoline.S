# Trampoline for signal handling 

#define SYS_rt_sigreturn 139

	.section sigtrampsec

	.globl sig_trampoline
sig_trampoline: 

	.globl sig_handler
# void sig_handler(int signum);
sig_handler: 
	li a7, SYS_rt_sigreturn 
	ecall