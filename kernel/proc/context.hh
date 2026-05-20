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
    enum VmaBackingKind : int
    {
        VMA_BACKING_NONE = 0,
        VMA_BACKING_FILE = 1,
        VMA_BACKING_SHM = 2,
    };

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
        int used;    // 是否已被使用
        uint64 addr; // 起始地址
        int len;     // 长度
        int prot;    // 权限
        int flags;   // 标志位
        int vfd;     // 对应的文件描述符
        // 保存与映射关联的文件指针
        fs::file *vfile; //  对应文件
        int offset;             // 文件偏移
        uint64 max_len;         // 新增：最大可扩展长度
        bool is_expandable;     // 新增：是否可扩展
        int backing_kind;       // 映射后端类型：普通/文件/共享段
        int backing_shmid;      // 共享段映射时记录真实 shmid，私有映射固定为 -1
        uint64 backing_base;    // 共享段映射时记录原始 attach 基址
    };
}
