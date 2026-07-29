#pragma once
#include "types.hh"
#include "proc/proc.hh"
#include "printer.hh"
#include "param.h"
#include <EASTL/atomic.h>
#ifdef RISCV
#include "riscv/rv_csr.hh"
#elif defined(LOONGARCH)
#include "loongarch/la_csr.hh"
#endif

class Cpu
{
private:
        proc::Pcb *_cur_proc;   // 当前进程
        proc::Context _context; // 进程上下文
        int _num_off;           // 关闭中断层数
        int _int_ena;           // 关中断前中断开关状态
        uint32 _timeslice_ticks; // 当前 CPU 的本地时间片计数，不能在多核间共享
        
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

        // SMP 拓扑与启动状态。CPU 槽位上限由 NCPU 决定，possible/online 则由
        // 主核从 DTB 和实际次核启动结果中维护，用户态只能看到 online 集合。
        static bool is_valid_cpu_id(uint64 cpu_id);
        static uint64 current_cpu_id();
        static void initialize_current();
        static bool try_claim_bootstrap();
        static void bootstrap_begin();
        static void configure_topology(const uint64 *hartids, int hart_count);
        static void publish_bootstrap_ready();
        static void wait_for_bootstrap_ready();
        static bool is_bootstrap_ready();
        // 全局对象就绪后，仍要等每个 possible CPU 完成本地 CSR/trap 初始化；
        // 只有主核确认全员 online 后才统一放行 scheduler，避免用户任务落在
        // “拓扑已声明、但次核尚未可调度”的窗口内。
        static void wait_for_all_possible_cpus_online();
        static void publish_scheduler_ready();
        static void wait_for_scheduler_ready();
        static uint64 bootstrap_cpu_id();
        static bool is_bootstrap_cpu();
        static bool is_possible_cpu(uint64 cpu_id);
        static void mark_current_online();
        static uint64 possible_cpu_mask();
        static uint64 online_cpu_mask();
        static int possible_cpu_count();
        static int online_cpu_count();

        // 时钟中断在各 CPU 上独立到达；抢占决策也必须是每核独立的，避免一个
        // CPU 的 tick 误清空另一个 CPU 正在运行任务的时间片。
        bool advance_time_slice(uint32 limit);
        void reset_time_slice() { _timeslice_ticks = 0; }

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
        // 从用户态进入内核时，用户态不可能持有内核自旋锁；
        // 这里用于清理跨调度残留的关中断嵌套计数，避免污染新的 syscall/trap。
        void reset_intr_off_depth();
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

        static inline void idle_until_interrupt()
        {
#ifdef RISCV
                asm volatile("wfi");
#elif defined(LOONGARCH)
                asm volatile("idle 0");
#endif
        }

        proc::Pcb *get_cur_proc() { return _cur_proc; }

private:
        static void _intr_on();
        static void _intr_off();
};
extern Cpu k_cpus[NUMCPU];
