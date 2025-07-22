#pragma once
#include "types.hh"
#include "proc/proc.hh"
#include "printer.hh"
#ifdef RISCV
#include "riscv/rv_csr.hh"
#elif defined(LOONGARCH)
#include "loongarch/la_csr.hh"
#endif

#define NUMCPU 1

class Cpu
{
private:
        proc::Pcb *_cur_proc;   // 当前进程
        proc::Context _context; // 进程上下文
        int _num_off;           // 关闭中断层数
        int _int_ena;           // 关中断前中断开关状态
        
public:
        proc::Context *get_context() { return &_context; }

        int get_num_off() { return _num_off; }
        int get_int_ena() { return _int_ena; }
        void set_int_ena(int x) { _int_ena = x; }
        void set_cur_proc(proc::Pcb *p) { _cur_proc = p; }

        uint64 get_time();

        static inline uint64 read_sp()
        {
                uint64 x;
#ifdef RISCV
                asm volatile("mv %0, sp" : "=r"(x));
#elif defined(LOONGARCH)
                asm volatile("addi.d %0, $sp, 0" : "=r"(x));
#endif
                return x;
        }

        static inline uint64 read_tp()
        {
                uint64 x;
#ifdef RISCV
                asm volatile("mv %0, tp" : "=r"(x));
#elif defined(LOONGARCH)
                asm volatile("addi.d %0, $tp, 0" : "=r"(x));
#endif
                return x;
        }

        static inline uint64 read_ra()
        {
                uint64 x;
#ifdef RISCV
                asm volatile("mv %0, ra" : "=r"(x));
#elif defined(LOONGARCH)
                asm volatile("addi.d %0, $ra, 0" : "=r"(x));
#endif
                return x;
        }

        /// @brief get current cpu info
        /// @return cpu info
        static Cpu *get_cpu();

        static inline int get_intr_stat()
        {
#ifdef RISCV
                uint64 x = riscv::r_sstatus();
                return (x & SSTATUS_SIE) != 0;
#elif defined(LOONGARCH)
                uint64 x = r_csr_crmd(); // 假设 crmd 对应 mstatus CSR
                return (x & loongarch::csr::crmd::ie_m) != 0;
#endif
        };

        static void push_intr_off();
        static void pop_intr_off();
        static void enable_fpu();

        // 不用这个，太傻逼了，不如w_csr()，以后再删
        static inline void write_csr(uint64 addr, uint64 val)
        {
                asm volatile("csrw %0, %1" : : "i"(addr), "r"(val));
        }

        static inline uint64 read_csr(uint64 addr)
        {
                uint64 x;
                asm volatile("csrr %0, %1" : "=r"(x) : "i"(addr));
                return x;
        }

        static inline void interrupt_on() { _intr_on(); }

        static inline void interrupt_off() { _intr_off(); }

        proc::Pcb *get_cur_proc() { return _cur_proc; }

private:
        static void _intr_on();
        static void _intr_off();
};
extern Cpu k_cpus[NUMCPU];