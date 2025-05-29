#pragma once

#include "types.hh"
namespace fs
{
    class dentry;
    class file;
    class normal_file;
} // namespace fs

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
        int used;               // 是否已被使用
        uint64 addr;            // 起始地址
        int len;                // 长度
        int prot;               // 权限
        int flags;              // 标志位
        int vfd;                // 对应的文件描述符
        ///@todo 此处原本是normal_file *vfile; // 对应的文件，改为fs::file是因为normal_file继承自fs::file
        fs::normal_file *vfile;      //  对应文件   
        int offset;             // 文件偏移，本实验中一直为0
    };
}