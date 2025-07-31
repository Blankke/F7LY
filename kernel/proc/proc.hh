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
#include <EASTL/string.h>
#include "signal.hh"
#include "prlimit.hh"
#include "futex.hh"
#include "fs/vfs/file/file.hh"
#include "signal.hh"

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
    void fill() { bits = ~0ULL; }
    bool empty() const { return bits == 0; }
};
namespace fs
{
    class dentry;
    class file;
} // namespace fs
namespace proc
{
    constexpr int NVMA = 30; // 每个进程最多的虚拟内存区域数量
    enum ProcState
    {
        UNUSED,
        USED,
        SLEEPING,
        RUNNABLE,
        RUNNING,
        ZOMBIE
    };

    constexpr uint num_process = 32;      // 系统中允许的最大进程数量
    constexpr int default_proc_prio = 10; // 默认进程优先级
    constexpr int lowest_proc_prio = 19;  // 最低进程优先级
    constexpr int highest_proc_prio = 0;  // 最高进程优先级
    constexpr uint max_open_files = 128;  // 每个进程最多可以打开的文件数量
    struct ofile
    {
        fs::file *_ofile_ptr[max_open_files]; // 进程打开的文件列表 (文件描述符 -> 文件结构)
        int _shared_ref_cnt;
        bool _fl_cloexec[max_open_files]; // 记录每个文件描述符的 close-on-exec 标志
    };
    struct sighand_struct
    {
        proc::ipc::signal::sigaction *actions[proc::ipc::signal::SIGRTMAX + 1];
        int refcnt;
    };
    struct program_section_desc
    {
        void *_sec_start = nullptr; // virtual address
        ulong _sec_size = 0;
        const char *_debug_name = nullptr;
    };
    struct rlimit
    {
        /* The current (soft) limit.  */
        rlim_t rlim_cur;
        /* The hard limit.  */
        rlim_t rlim_max;
    };
    constexpr int max_program_section_num = 16;
    constexpr int max_vma_num = 10;
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

        /****************************************************************************************
         * 进程状态和调度信息
         ****************************************************************************************/
        enum ProcState _state; // 进程当前状态 (unused, used, sleeping, runnable, running, zombie)
        void *_chan;           // 进程睡眠时等待的通道，指向等待的资源或事件
        int _killed;           // 进程终止标志位，非零表示进程被标记为终止
        int _xstate;           // 进程退出状态码，供父进程通过wait()系统调用获取

        // 调度相关字段
        int _slot;     // 当前时间片剩余量 @todo: 应使用更精确的时间单位
        int _priority; // 进程优先级 (0最高，19最低)，符合Linux nice值规范
        
        // CPU亲和性字段
        CpuMask _cpu_mask; // CPU亲和性掩码，每个位表示一个CPU核心

        /****************************************************************************************
         * 内存管理
         ****************************************************************************************/
        uint64 _kstack = 0;      // 内核栈的虚拟地址
        bool _shared_vm = false; // 标记是否与父进程共享虚拟内存(CLONE_VM标志)
        
        // 程序段管理
        program_section_desc _prog_sections[max_program_section_num]; // 程序段描述数组
        int _prog_section_cnt = 0;                                    // 已记录的程序段数量
        
        // 堆内存管理
        uint64 _heap_start = 0;  // 堆的起始地址
        uint64 _heap_end = 0;    // 堆的结束地址
        
        mem::PageTable _pt;                                           // 用户空间页表，等同于Linux的mm->pgd
        TrapFrame *_trapframe;                                        // 用户态寄存器保存区，用于系统调用和异常处理

    private:
        uint64 _sz;              // 进程占用的总内存空间大小(字节)，包含所有程序段的总和，由内部自动管理

    public:

        // 虚拟内存区域管理
        struct VMA
        {
            vma _vm[NVMA]; // 虚拟内存区域数组，类似Linux的vm_area_struct
            int _ref_cnt;  // VMA引用计数，用于copy-on-write机制
        };
        VMA *_vma; // VMA管理结构指针

        /****************************************************************************************
         * 上下文切换
         ****************************************************************************************/
        Context _context; // 内核态上下文信息，用于进程切换时保存/恢复寄存器

        /****************************************************************************************
         * 文件系统和I/O管理
         ****************************************************************************************/
        fs::dentry *_cwd;        // 当前工作目录的dentry指针
        eastl::string _cwd_name; // 当前工作目录的路径字符串 @todo: 与_cwd冗余，需要统一
        ofile *_ofile;           // 打开文件描述符表，包含文件指针和close-on-exec标志
        mode_t _umask;           // 文件模式创建掩码，用于屏蔽新创建文件的权限位

        /****************************************************************************************
         * 线程和同步原语
         ****************************************************************************************/
        void *_futex_addr;                        // futex等待地址，用于用户态同步原语
        uint64 _clear_tid_addr = 0;               // 线程退出的时候清除该地址的值(8字节)
        robust_list_head *_robust_list = nullptr; // 健壮futex链表头，用于线程退出时清理

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
        ipc::signal::signal_frame *sig_frame = nullptr; // 信号处理栈帧，保存信号处理上下文

        /****************************************************************************************
         * 资源限制
         ****************************************************************************************/
        rlimit64 _rlim_vec[ResourceLimitId::RLIM_NLIMITS]; // 进程资源限制数组，符合Linux rlimit规范

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



    public:
        Pcb();
        void init(const char *lock_name, uint gid);
        void cleanup_ofile();   // 释放ofile资源的方法
        void cleanup_sighand(); // 释放sighand_struct资源的方法
        void map_kstack(mem::PageTable &pt);
        fs::dentry *get_cwd() { return _cwd; }
        int get_priority();
        
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
        uint64 get_heap_size() const { return _heap_end > _heap_start ? _heap_end - _heap_start : 0; }
        void set_heap_start(uint64 start_addr) { _heap_start = start_addr; }
        void set_heap_end(uint64 end_addr) { _heap_end = end_addr; }
        
        // 内存大小计算方法（内部使用）
        void update_total_memory_size();
        uint64 calculate_total_memory_size() const;
        
        // 内存一致性检查方法（内部使用）
        bool verify_memory_consistency() const;
        
        // 内存管理接口
        void free_all_memory_resources();       // 释放所有内存资源
        void emergency_memory_cleanup();        // 紧急内存清理
        bool check_memory_leaks() const;        // 检查内存泄漏
        void print_detailed_memory_info() const; // 打印详细内存信息

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
        void set_state(ProcState state) { _state = state; }
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
        mode_t get_umask() const { return _umask; }                   // 获取文件模式创建掩码
        void set_umask(mode_t umask) { _umask = umask & 0777; } // 设置umask，只保留权限位
        TrapFrame *get_trapframe() { return _trapframe; }
        uint64 get_kstack() const { return _kstack; }
        mem::PageTable *get_pagetable() { return &_pt; }
        ProcState get_state() const { return _state; }
        char *get_name() { return _name; }
        uint64 get_size() const { return _sz; }
        uint64 get_heap_start() const { return _heap_start; }
        uint64 get_heap_end() const { return _heap_end; }
        int get_prog_section_count() const { return _prog_section_cnt; }
        const program_section_desc* get_prog_sections() const { return _prog_sections; }
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

        void add_signal(int sig)
        {
            ipc::signal::add_signal(this, sig);
        }

        void set_trapframe(TrapFrame *tf) { _trapframe = tf; }

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
        
        // CPU亲和性相关方法
        const CpuMask& get_cpu_mask() const { return _cpu_mask; }
        void set_cpu_mask(const CpuMask& mask) { _cpu_mask = mask; }
        
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
