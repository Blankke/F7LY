# Trampoline for signal handling
    .section sigtrampsec

    .globl sig_trampoline
    .align 12
sig_trampoline:
    li.d $a7, 139
    syscall 0