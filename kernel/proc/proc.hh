#pragma once
#include "types.hh"
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
#include "trapframe.hh"
#include "context.hh"
#include "spinlock.hh"
#include <EASTL/atomic.h>
#include <EASTL/string.h>
#include "signal.hh"
#include "prlimit.hh"
#include "futex.hh"
#include "fs/vfs/file/file.hh"
#include "process_memory_manager.hh"
#include "cpu.hh"
#include "param.h"

// CPU掩码定义，兼容Linux的cpu_set_t
struct CpuMask
{
    uint64 bits;

    CpuMask() : bits(0) {}
    CpuMask(uint64 mask) : bits(mask) {}

    void set(int cpu) { bits |= (1ULL << cpu); }
    void clear(int cpu) { bits &= ~(1ULL << cpu); }
    bool is_set(int cpu) const { return (bits & (1ULL << cpu)) != 0; }
    void zero() { bits = 0; }
    void fill() { bits = (1ULL << NUMCPU) - 1 ;}
    bool empty() const { return bits == 0; }
};
namespace fs
{
    class dentry;
    class file;
} // namespace fs
namespace proc
{
    enum ProcState
    {
        UNUSED,
        USED,
        SLEEPING,
        RUNNABLE,
        RUNNING,
        STOPPED,
        ZOMBIE
    };

    constexpr uint num_process = NPROC;    // 系统中允许的最大进程数量；与旧 param.h 宏保持单一权威。
    constexpr int default_proc_prio = 0;   // 默认 nice 值
    constexpr int lowest_proc_prio = 19;   // 最低优先级对应的 nice 值
    constexpr int highest_proc_prio = -20; // 最高优先级对应的 nice 值
    constexpr uint fd_table_capacity = 1024; // fd 表物理容量，支持高编号 fd 并与运行时上限解耦。
    extern uint max_open_files;              // 运行时上限，避免编译器在各调用点展开 1024 次固定循环。
    constexpr uint max_supplementary_groups = 64; // 固定补充组容量，避免凭据路径动态分配。
    constexpr uint pid_max = 4194304;      // Linux 默认级别的 PID 上限，供 /proc/sys/kernel/pid_max 和范围校验使用
    constexpr int k_interval_timer_count = 3; // ITIMER_REAL / ITIMER_VIRTUAL / ITIMER_PROF

    struct ofile
    {
        SpinLock _lock;                         // 共享 fd 表锁，保护并发 open/close/dup
        fs::file *_ofile_ptr[fd_table_capacity]; // 进程打开的文件列表 (文件描述符 -> 文件结构)
        bool _reserved[fd_table_capacity];       // 预留槽位，避免并发 open 在文件真正创建前重复抢同一个 fd
        int _shared_ref_cnt;
        uint _highest_fd_plus_one; // 当前 fd 表需要扫描的上界，避免高频路径固定扫完整容量
        bool _fl_cloexec[fd_table_capacity]; // 记录每个文件描述符的 close-on-exec 标志

        void init(const char *lock_name)
        {
            _lock.init(lock_name);
            _shared_ref_cnt = 1;
            _highest_fd_plus_one = 0;
            memset(_ofile_ptr, 0, sizeof(_ofile_ptr));
            memset(_reserved, 0, sizeof(_reserved));
            memset(_fl_cloexec, 0, sizeof(_fl_cloexec));
        }
    };
    struct sighand_struct
    {
        proc::ipc::signal::sigaction *actions[proc::ipc::signal::SIGRTMAX + 1];
        // CLONE_SIGHAND 的成员可在不同 CPU 同时 clone/exit；引用计数必须原子，
        // 否则普通 ++/-- 会丢更新并提前释放整张 handler 表。
        eastl::atomic<int> refcnt{1};
    };
    struct rlimit
    {
        /* The current (soft) limit.  */
        rlim_t rlim_cur;
        /* The hard limit.  */
        rlim_t rlim_max;
    };
    struct interval_timer_state
    {
        // 这里统一用微秒保存状态，sys_setitimer()/sys_getitimer() 再做 sec/usec 转换，
        // 这样可以把 REAL/VIRTUAL/PROF 三种计时源收敛到同一套核心逻辑里。
        // CPU0 的时钟中断会先无锁读取该位，只对真正武装了
        // ITIMER_REAL 的 PCB 取锁。原子提示允许与 setitimer 并发：
        // 错过一次新武装最多只延后一个 tick，但不会丢失定时器。
        eastl::atomic<bool> armed{false};
        uint64 interval_us = 0;
        uint64 expiry_us = 0;
    };
    struct time_namespace_offsets
    {
        // time namespace 只给单调/boottime 时钟加偏移，不影响 CLOCK_REALTIME。
        int64 monotonic_offset_ns = 0;
        int64 boottime_offset_ns = 0;
    };

    struct net_namespace_state
    {
        // 维护网络命名空间内 /proc/sys/net/ipv4/conf/*/tag 的最小状态。
        // default/tag 代表新 netns 初始化模板，lo/tag 是当前 namespace 的 loopback 参数。
        int ipv4_conf_default_tag = 0;
        int ipv4_conf_lo_tag = 0;
    };

    constexpr uint64 k_initial_ipc_namespace_id = 1;
    constexpr uint64 k_initial_mount_namespace_id = 1;

    class Pcb
    {

    public:
        /****************************************************************************************
         * 基本进程标识和状态管理
         ****************************************************************************************/
        SpinLock _lock; // 进程控制块的锁，用于并发访问控制
        int _global_id; // 全局ID，用于在进程池中唯一标识进程
        int _pid;       // 进程ID (Process ID)
        int _tid = 0;   // 线程ID，在单线程进程中等于PID @todo: 多线程支持

        Pcb *_parent;      // 父进程的PCB指针
        char _name[30];    // 进程名称，用于调试和识别
        eastl::string exe; // 可执行文件的绝对路径
        eastl::string _cmdline; // 最近一次成功 execve 的 NUL 分隔 argv

        // 新增：标准Linux进程标识符
        int _ppid; // 父进程PID，用于快速访问，避免通过_parent指针获取
        int _pgid; // 进程组ID，用于作业控制
        int _tgid; // 线程组ID，同一进程的所有线程共享同一个TGID，主线程的TGID等于PID

        int _sid;      // 会话ID，用于终端管理
        uint32 _uid;   // 真实用户ID
        uint32 _euid;  // 有效用户ID
        uint32 _suid;  // 保存的设置用户ID
        uint32 _fsuid; // 文件系统用户ID
        uint32 _gid;   // 真实组ID
        uint32 _egid;  // 有效组ID
        uint32 _sgid;  // 保存的设置组ID
        uint32 _fsgid; // 文件系统组ID
        uint32 _supplementary_groups[max_supplementary_groups]; // setgroups/getgroups 使用的补充组列表
        int _supplementary_group_count;                         // 当前有效补充组数量
        // Linux capability ABI 目前定义到 CAP_CHECKPOINT_RESTORE(40)，用两个 u32 保存三组能力集。
        uint32 _cap_effective[2];
        uint32 _cap_permitted[2];
        uint32 _cap_inheritable[2];
        uint32 _cap_ambient[2];
        uint32 _cap_bounding[2];

        /****************************************************************************************
         * 进程状态和调度信息
         ****************************************************************************************/
        enum ProcState _state; // 进程当前状态 (unused, used, sleeping, runnable, running, zombie)
        // 唤醒端先按 channel 无锁筛选，再只获取真正候选者的 PCB 锁。
        // 因此 channel 必须原子发布；状态本身仍只允许在 _lock 下访问。
        eastl::atomic<void *> _chan{nullptr};
        // 通用 sleep/wakeup 的等待通道位图成员资格。位图只做候选筛选，
        // 最终 channel 与状态仍必须在 PCB 锁下复核。
        uint _wait_channel_bucket = 0;
        bool _wait_channel_registered = false;
        int _killed;           // 进程终止标志位，非零表示进程被标记为终止
        bool _exiting;         // 已进入退出清理流程，禁止 timer 抢占式 yield
        bool _mm_cleanup_active; // 正在脱离旧 mm；exec 换映像期间也可能 sleep
        // 退出任务由 scheduler 在切回后、仍持有 PCB 锁时完成回收。
        bool _deferred_reap;
        int _xstate;           // 进程退出状态码，供父进程通过wait()系统调用获取
        int _parent_exit_signal; // 非线程子任务退出时需要发送给父进程的信号，0 表示不发送
        int _stop_signal;      // 最近一次使任务停止的 job-control 信号
        bool _stop_reported;   // wait4/waitid 是否已经消费本次停止事件
        bool _continued_pending; // SIGCONT 后等待父进程消费的继续事件
        bool _has_child_tasks;  // 是否创建过普通子进程；无子线程退出时可跳过 reparent 全表扫描

        // 调度相关字段
        int _slot;     // 当前时间片剩余量 @todo: 应使用更精确的时间单位
        int _priority; // CPU 调度使用的 nice 值，范围为 [-20, 19]，数值越小优先级越高
        int _sched_policy;          // Linux sched_* ABI 策略，不包含 SCHED_RESET_ON_FORK 标志
        int _sched_priority;        // 实时策略优先级；普通策略固定为 0
        bool _sched_reset_on_fork;  // fork 后子进程是否恢复 SCHED_OTHER
        int _io_priority_override; // 研究/后续 ioprio 扩展使用的块层优先级覆盖值
        bool _has_io_priority_override; // false 时块层默认跟随 _priority，true 时使用覆盖值

        // CPU亲和性字段
        CpuMask _cpu_mask; // CPU亲和性掩码，每个位表示一个CPU核心
        // 最近一次实际运行的 CPU，也是未显式绑核任务的粘附放置目标；供
        // /proc/<pid>/stat 的 processor 字段使用。若 affinity 排除了该 CPU，
        // 调度器会在新的允许 CPU 成功认领任务时更新此字段。
        int _last_cpu;
        // 当前实际执行此 PCB 的 CPU 槽位；-1 表示任务已经让出 CPU。
        // 该字段只在持有 _lock 时读写，是 SMP 调度状态机的硬性不变量：一个
        // PCB 绝不能同时在两个 CPU 上运行，否则 trapframe 与内核栈会被并发覆盖。
        int _running_cpu;

        /****************************************************************************************
         * 内存管理
         ****************************************************************************************/
        uint64 _kstack = 0;    // 内核栈的虚拟地址
        TrapFrame *_trapframe; // 用户态寄存器保存区，用于系统调用和异常处理, 在usertrapret时映射
        bool _used_fpu;        // LoongArch 懒 FPU：线程第一次触发浮点禁用异常后才保存/恢复 FPU 现场
        bool _used_lsx;        // LoongArch 懒 LSX：启用后必须保存完整 128 位向量现场
    private:
        // trap 返回热路径缓存：只有 mm、页表根和 trapframe 物理页三者都
        // 未变化时，USER_TRAPFRAME(_global_id) 的 PTE 才可跳过软件 walk。
        // PCB 仅会在持有自身锁时被单 CPU 执行，因此这里不需要原子字段。
        class ProcessMemoryManager *_trapframe_mapping_mm;
        uint64 _trapframe_mapping_pagetable_base;
        uint64 _trapframe_mapping_pa;
        // 阶段1：统一内存管理器（替代分散的内存字段）
        class ProcessMemoryManager* _memory_manager;

    public:
        /****************************************************************************************
         * 上下文切换
         ****************************************************************************************/
        Context _context; // 内核态上下文信息，用于进程切换时保存/恢复寄存器

        /****************************************************************************************
         * 文件系统和I/O管理
         ****************************************************************************************/
        fs::dentry *_cwd;        // 当前工作目录的dentry指针
        eastl::string _cwd_name; // 当前工作目录的路径字符串 @todo: 与_cwd冗余，需要统一
        eastl::string _root_name; // chroot 后的进程根目录，使用底层文件系统绝对路径保存
        ofile *_ofile;           // 打开文件描述符表，包含文件指针和close-on-exec标志
        mode_t _umask;           // 文件模式创建掩码，用于屏蔽新创建文件的权限位
        uint32 _personality;     // 当前进程的 Linux personality(2) 状态

        /****************************************************************************************
         * 线程和同步原语
         ****************************************************************************************/
        void *_futex_addr;                        // futex等待地址，仅用于调试和错误诊断
        // 共享 futex 的 value 是物理地址；私有 futex 的 value 是用户虚拟地址，
        // 并由 _futex_private + 当前 ProcessMemoryManager 完成精确判等。这样 COW
        // 改变私有页物理地址后，已经入队的等待者仍可被同一 mm 中的 WAKE 找到。
        eastl::atomic<uint64> _futex_key{0};
        bool _futex_private = false;
        // timer 路径只能先无锁定位桶，再遵循桶锁 -> PCB 锁做复核；因此将入队
        // 时计算出的桶号原子发布，避免在锁外读取私有 key 的非原子身份字段。
        eastl::atomic<uint32> _futex_bucket_index{0};
        // 定时 FUTEX_WAIT 的硬件截止时间。使用原子发布，定时器先读它选择
        // 桶，再在桶锁和 PCB 锁下二次确认，避免与入队/唤醒并发时读到半个值。
        eastl::atomic<uint64> _futex_timeout_deadline{0};
        // 仅在 PCB 锁下访问：0 表示普通唤醒，SYS_ETIMEDOUT 表示定时器唤醒。
        int _futex_wait_result = 0;
        uint64 _clear_tid_addr = 0;               // 线程退出的时候清除该地址的值(8字节)
        robust_list_head *_robust_list = nullptr; // 健壮futex链表头，用于线程退出时清理
        uint64 _robust_list_user_addr = 0;        // 健壮futex链表头的原始用户虚拟地址，用于 get_robust_list ABI
        Pcb *_vfork_parent = nullptr;             // CLONE_VFORK 子进程释放地址空间前需要唤醒的父进程

        /****************************************************************************************
         * 进程间通信(IPC)
         ****************************************************************************************/
        // uint _mqmask; // 消息队列使用掩码 @todo: 非标准Linux字段，需要说明用途

        // // TODO: 共享内存相关 - 标准Linux使用shm_file_data结构
        // // uint _shm;                  // 共享内存起始虚拟地址
        // // void *_shmva[SHM_NUM];      // 共享内存区域虚拟地址数组
        // // uint _shmkeymask;           // 共享物理内存页使用掩码

        /****************************************************************************************
         * 信号处理
         ****************************************************************************************/
        sighand_struct *_sigactions = nullptr;          // 信号处理函数表，类似Linux的sighand_struct
        uint64 _sigmask = 0;                            // 信号屏蔽掩码，阻塞指定信号
        uint64 _signal = 0;                             // 待处理信号掩码
        uint64 _siginfo_mask = 0;                       // 哪些 pending signal 还携带一份最小 siginfo
        ipc::signal::LinuxSigInfo _queued_siginfo[ipc::signal::SIGRTMAX + 1]; // 每个 signal 只保留最后一份排队信息
        bool _sigsuspend_restore_pending = false;       // sigsuspend 临时 mask 生效后，等待信号返回时恢复旧 mask
        uint64 _sigsuspend_saved_sigmask = 0;            // sigsuspend 进入前的原始信号屏蔽掩码
        ipc::signal::signal_frame *sig_frame = nullptr; // 信号处理栈帧，保存信号处理上下文
        ipc::signal::signalstack _alt_stack;           // 信号处理备用栈
        bool _on_sigstack = false;                      // 当前是否在信号栈上执行

        /****************************************************************************************
         * 资源限制
         ****************************************************************************************/
        rlimit64 _rlim_vec[ResourceLimitId::RLIM_NLIMITS]; // 进程资源限制数组，符合Linux rlimit规范

        /****************************************************************************************
         * prctl相关字段
         ****************************************************************************************/
        int _dumpable = 1;              // 进程可dump标志，0=禁止，1=允许
        int _pdeathsig = 0;             // 父进程死亡时发送给本进程的信号
        int _keepcaps = 0;              // 保持capabilities标志  
        int _timing = 0;                // 进程定时模式，0=normal,1=statistical
        int _no_new_privs = 0;          // 禁止获得新权限标志
        int _thp_disable = 0;           // 禁用透明大页标志
        int _seccomp_mode = 0;          // seccomp 当前模式；0 表示未启用
        uint64 _timer_slack_ns = 50000; // 定时器松弛时间(纳秒)
        uint64 _securebits = 0;         // 安全位

        /****************************************************************************************
         * 时间统计和会计信息
         ****************************************************************************************/
        uint64 _start_tick;        // 进程开始运行时的时钟节拍数
        uint64 _user_ticks;        // 进程在用户态累计运行时钟节拍数
        uint64 _last_user_tick;    // 进程上次进入用户态的时钟节拍数
        uint64 _kernel_entry_tick; // 进程进入内核态的时钟节拍数

        // 新增：详细时间统计
        uint64 _stime;          // 系统态时间 (内核态运行时间)
        uint64 _utime;          // 用户态时间 (用户态运行时间)
        uint64 _cutime;         // 子进程用户态时间累计
        uint64 _cstime;         // 子进程系统态时间累计
        uint64 _start_time;     // 进程启动时间 (绝对时间戳)
        uint64 _start_boottime; // 自系统启动以来的启动时间
        interval_timer_state _itimer[k_interval_timer_count]; // 每进程 interval timer 状态
        time_namespace_offsets _timens_current;  // 当前任务看到的 time namespace 偏移
        time_namespace_offsets _timens_children; // fork/clone 后子任务应继承的 time namespace 偏移
        net_namespace_state _netns; // 最小网络 namespace 视图（当前只覆盖 clone09 所需 sysctl）
        uint64 _ipc_ns_id = k_initial_ipc_namespace_id; // SysV IPC namespace，隔离 shmget key 空间
        uint64 _mnt_ns_id = k_initial_mount_namespace_id; // mount namespace，隔离 bind/move/umount 视图

    public:
        Pcb();
        void init(const char *lock_name, uint gid);
        ofile *ensure_ofile(); // 按需创建文件描述符表
        sighand_struct *ensure_sighand(); // 按需创建信号处理表
        void cleanup_ofile();   // 释放ofile资源的方法
        void cleanup_sighand(); // 释放sighand_struct资源的方法
        void cleanup_memory_manager(); // 释放ProcessMemoryManager资源
        void set_memory_manager(ProcessMemoryManager* mm); // 设置新的内存管理器
        // 仅重置内存管理器指针，不执行 cleanup；用于 PCB 复用或失败回滚时切断历史脏指针。
        void reset_memory_manager_ptr(ProcessMemoryManager* mm = nullptr)
        {
            invalidate_trapframe_mapping_cache();
            _memory_manager = mm;
        }
        ProcessMemoryManager* get_memory_manager() { return _memory_manager; } // 获取内存管理器
        uint32 get_personality() const { return _personality; }
        void set_personality(uint32 personality) { _personality = personality; }
        void map_kstack(mem::PageTable &pt);
        fs::dentry *get_cwd() { return _cwd; }
        int get_priority();
        int get_io_priority();
        void set_io_priority_override(int priority);
        void clear_io_priority_override();

        // 程序段管理方法
        int add_program_section(void *start, ulong size, const char *name = nullptr);
        void remove_program_section(int index);
        void clear_all_program_sections();
        void reset_memory_sections(); // 重置所有内存管理信息
        uint64 get_total_program_memory() const;
        void copy_program_sections(const Pcb *src);

        // 堆内存管理方法
        void init_heap(uint64 start_addr);
        uint64 grow_heap(uint64 new_end);
        uint64 shrink_heap(uint64 new_end);

        // 内存大小计算方法（内部使用）
        void update_total_memory_size();
        uint64 calculate_total_memory_size() const;

        // 内存一致性检查方法（内部使用）
        bool verify_memory_consistency();

        // 其他内存管理接口
        void emergency_memory_cleanup();         // 紧急内存清理
        bool check_memory_leaks() const;         // 检查内存泄漏

    public:
        Context *get_context() { return &_context; }

    public:
        // fs::Dentry *get_cwd() { return _cwd; }
        void kill()
        {
            _lock.acquire();
            _killed = 1;
            _lock.release();
        }
        Pcb *get_parent() const { return _parent; }
        void set_xstate(int xstate) { _xstate = xstate; }
        // void set_chan(void *chan) { _chan = chan; }
        uint get_pid() const { return _pid; }
        uint get_tid() const { return _tid; }
        uint get_global_id() const { return _global_id; }
        uint get_ppid() const { return _parent ? _parent->_pid : _ppid; } // 优先使用_parent，回退到_ppid
        uint get_pgid() const { return _pgid; }
        uint get_tgid() const { return _tgid; }
        uint get_sid() const { return _sid; }
        uint32 get_uid() const { return _uid; }
        uint32 get_euid() const { return _euid; }
        uint32 get_suid() const { return _suid; }
        uint32 get_fsuid() const { return _fsuid; }
        uint32 get_gid() const { return _gid; }
        uint32 get_egid() const { return _egid; }
        uint32 get_sgid() const { return _sgid; }
        uint32 get_fsgid() const { return _fsgid; }
        mode_t get_umask() const { return _umask; }             // 获取文件模式创建掩码
        void set_umask(mode_t umask) { _umask = umask & 0777; } // 设置umask，只保留权限位

        TrapFrame *get_trapframe() { return _trapframe; }
        const TrapFrame *get_trapframe() const { return _trapframe; }
        void set_trapframe(TrapFrame *tf)
        {
            if (_trapframe != tf)
            {
                invalidate_trapframe_mapping_cache();
                _trapframe = tf;
            }
        }

        bool trapframe_mapping_cache_matches(ProcessMemoryManager *mm,
                                             uint64 pagetable_base,
                                             uint64 trapframe_pa) const
        {
            return _trapframe_mapping_mm == mm &&
                   _trapframe_mapping_pagetable_base == pagetable_base &&
                   _trapframe_mapping_pa == trapframe_pa;
        }
        void cache_trapframe_mapping(ProcessMemoryManager *mm,
                                     uint64 pagetable_base,
                                     uint64 trapframe_pa)
        {
            _trapframe_mapping_mm = mm;
            _trapframe_mapping_pagetable_base = pagetable_base;
            _trapframe_mapping_pa = trapframe_pa;
        }
        void invalidate_trapframe_mapping_cache()
        {
            _trapframe_mapping_mm = nullptr;
            _trapframe_mapping_pagetable_base = 0;
            _trapframe_mapping_pa = 0;
        }

        uint64 get_kstack() const { return _kstack; }
        void set_kstack(uint64 kstack) { _kstack = kstack; }

        // 页表访问：通过ProcessMemoryManager
        mem::PageTable *get_pagetable();
        const mem::PageTable *get_pagetable() const;
        void set_pagetable(const mem::PageTable &pt);

        // 共享VM标志：通过ProcessMemoryManager
        bool get_shared_vm() const;
        void set_shared_vm(bool shared);

        // VMA访问：通过ProcessMemoryManager
        VMA *get_vma();
        const VMA *get_vma() const;
        void set_vma(VMA *vma);

        // 程序段访问方法：通过ProcessMemoryManager
        int get_prog_section_count() const;
        const program_section_desc *get_prog_sections() const;
        program_section_desc *get_prog_sections();
        void set_prog_section_count(int count);

        // 堆内存访问方法：通过ProcessMemoryManager
        uint64 get_heap_start() const;
        uint64 get_heap_end() const;
        uint64 get_heap_size() const;
        uint64 get_mmap_cursor() const;
        void set_heap_start(uint64 start_addr);
        void set_heap_end(uint64 end_addr);
        void set_mmap_cursor(uint64 next_addr);

        // 内存大小访问方法：通过ProcessMemoryManager
        uint64 get_size() const;

        ProcState get_state() const { return _state; }
        char *get_name() { return _name; }
        uint64 get_last_user_tick() const { return _last_user_tick; }
        uint64 get_user_ticks() const { return _user_ticks; }
        uint64 get_stime() const { return _stime; }
        uint64 get_cutime() const { return _cutime; }
        uint64 get_cstime() const { return _cstime; }
        uint64 get_start_tick() const { return _start_tick; }
        uint64 get_start_time() const { return _start_time; }
        uint64 get_start_boottime() const { return _start_boottime; }
        fs::file *get_open_file(int fd)
        {
            if (fd < 0 || fd >= (int)max_open_files || _ofile == nullptr)
                return nullptr;
            return _ofile->_ofile_ptr[fd];
        }

        // 获取打开文件数量限制
        uint64 get_nofile_limit() const
        {
            return _rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_cur;
        }

        // 获取文件大小限制
        uint64 get_fsize_limit() const
        {
            return _rlim_vec[ResourceLimitId::RLIMIT_FSIZE].rlim_cur;
        }

        void add_signal(int sig, const ipc::signal::LinuxSigInfo *info = nullptr)
        {
            ipc::signal::add_signal(this, sig, info);
        }

        void set_last_user_tick(uint64 tick) { _last_user_tick = tick; }
        void set_user_ticks(uint64 ticks) { _user_ticks = ticks; }

        // 新增的设置器方法
        void set_ppid(int ppid) { _ppid = ppid; }
        void set_pgid(int pgid) { _pgid = pgid; }
        void set_tgid(int tgid) { _tgid = tgid; }
        void set_sid(int sid) { _sid = sid; }
        void set_uid(uint32 uid) { _uid = uid; }
        void set_euid(uint32 euid) { _euid = euid; }
        void set_suid(uint32 suid) { _suid = suid; }
        void set_fsuid(uint32 fsuid) { _fsuid = fsuid; }
        void set_gid(uint32 gid) { _gid = gid; }
        void set_egid(uint32 egid) { _egid = egid; }
        void set_sgid(uint32 sgid) { _sgid = sgid; }
        void set_fsgid(uint32 fsgid) { _fsgid = fsgid; }

        // CPU亲和性相关方法
        const CpuMask &get_cpu_mask() const { return _cpu_mask; }
        void set_cpu_mask(const CpuMask &mask) { _cpu_mask = mask; }

        bool is_process() const
        {
            return _tid == _tgid; // 线程ID等于线程组ID表示是主线程
        }

        bool is_killed()
        {
            int k;
            _lock.acquire();
            k = _killed;
            _lock.release();
            return k;
        }
    };

    extern Pcb k_proc_pool[num_process]; // 全局进程池
}
