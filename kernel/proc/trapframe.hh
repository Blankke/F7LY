#pragma once
#include "types.hh"
//由于signal.h和proc.h的耦合问题，将trapframe的定义单独放在一个.h中，实际上可以被修改

#ifdef RISCV
struct TrapFrame
{
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};
#elif defined(LOONGARCH)
struct TrapFrame {
  /*   0 */ uint64 ra;
  /*   8 */ uint64 tp;
  /*  16 */ uint64 sp;
  /*  24 */ uint64 a0;
  /*  32 */ uint64 a1;
  /*  40 */ uint64 a2;
  /*  48 */ uint64 a3;
  /*  56 */ uint64 a4;
  /*  64 */ uint64 a5;
  /*  72 */ uint64 a6;
  /*  80 */ uint64 a7;
  /*  88 */ uint64 t0;
  /*  96 */ uint64 t1;
  /* 104 */ uint64 t2;
  /* 112 */ uint64 t3;
  /* 120 */ uint64 t4;
  /* 128 */ uint64 t5;
  /* 136 */ uint64 t6;
  /* 144 */ uint64 t7;
  /* 152 */ uint64 t8;
  /* 160 */ uint64 r21;
  /* 168 */ uint64 fp;
  /* 176 */ uint64 s0;
  /* 184 */ uint64 s1;
  /* 192 */ uint64 s2;
  /* 200 */ uint64 s3;
  /* 208 */ uint64 s4;
  /* 216 */ uint64 s5;
  /* 224 */ uint64 s6;
  /* 232 */ uint64 s7;
  /* 240 */ uint64 s8;
  /* 248 */ uint64 kernel_sp;     // top of process's kernel stack
  /* 256 */ uint64 kernel_trap;   // usertrap()
  /* 264 */ uint64 era;           // saved user program counter
  /* 272 */ uint64 kernel_hartid; // saved kernel tp
  /* 280 */ uint64 kernel_pgdl;   // saved kernel pagetable
  /*
   * LoongArch 浮点寄存器由用户态 ABI 直接用于传参和保存中间结果。
   * 系统调用、缺页和 timer trap 都可能打断 libm/musl 动态链接器路径；
   * 不保存 FPU 状态会让不同进程/线程或内核路径相互污染浮点现场。
   */
  /* 288 */ uint64 f[32];
  /* 544 */ uint64 fcsr;
  /* 552 */ uint64 fcc[8];
  /*
   * LSX 的 128 位向量寄存器与 FPR 共享低 64 位。保留上面的 f[] 供现有
   * signal ABI 使用，同时以 16 字节对齐的完整镜像保存可抢占的 LSX 现场。
   */
  /* 624 */ alignas(16) uint64 lsx[32][2];
};
#endif
