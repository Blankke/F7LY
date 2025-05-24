// #include "backtrace.hh"
// #include "memlayout.hh"
// #include "types.hh"
// #ifdef __gnu_linux__
// bool sp_is_in_kstack( void *sp )
// {
//     uint64 st	= mem::vml::vm_kernel_start;
//     uint64 ed	= mem::vml::vm_kernel_end;
//     uint64 spa = (u64) sp;
//     return spa >= st && spa < ed;

//     // char *spa = (char *) sp;
//     // return spa >= entry_stack && spa < entry_stack_end;
// }


// #endif