#pragma once 
#include "../types.hh"

    struct Context
    {
        uint64 ra;   // x1
        uint64 sp;   // x2

        // Callee-saved registers
        uint64 s0;   // x8 (fp)
        uint64 s1;   // x9
        uint64 s2;   // x18
        uint64 s3;   // x19
        uint64 s4;   // x20
        uint64 s5;   // x21
        uint64 s6;   // x22
        uint64 s7;   // x23
        uint64 s8;   // x24
        uint64 s9;   // x25
        uint64 s10;  // x26
        uint64 s11;  // x27
    };
 
	// Context k_proc_context_pool[ proc_pool_size ];

	// Context * proc_context_pool() { return k_proc_context_pool; };
