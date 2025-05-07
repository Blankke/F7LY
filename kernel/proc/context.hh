#pragma once

#include "types.hh"

namespace proc
{
    struct Context
    {
#ifdef RISCV
        uint64 ra;
        uint64 sp;

        // callee-saved
        uint64 s0;
        uint64 s1;
        uint64 s2;
        uint64 s3;
        uint64 s4;
        uint64 s5;
        uint64 s6;
        uint64 s7;
        uint64 s8;
        uint64 s9;
        uint64 s10;
        uint64 s11;
#elif defined(LOONGARCH)
        uint64 ra;
        uint64 sp;

        // callee-saved
        uint64 s0;
        uint64 s1;
        uint64 s2;
        uint64 s3;
        uint64 s4;
        uint64 s5;
        uint64 s6;
        uint64 s7;
        uint64 s8;
        uint64 fp;
#endif
    };

    struct vma
    {
        uint64 address; // start of mem block
        int length;     // length of mem block
        int next;       // next block
    };
}