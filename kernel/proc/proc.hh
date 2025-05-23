#pragma once
#include "types.hh"
#include "pagetable.hh"
#include "trapframe.hh"
#include "context.hh"
#include "spinlock.hh"
namespace proc
{
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

    class Pcb
    {
        // friend ProcessManager;
        // friend ShmManager;
        // friend Scheduler;

    public:
        SpinLock _lock;        // 进程控制块的锁，用于并发访问控制
        int _gid = num_process; // 全局ID，用于在进程池中唯一标识进程

        // TODO,文件系统相关
        // fs::Dentry *_cwd;                     // 当前工作目录的目录项
        char _cwd_name[256];              // 当前工作目录的名称
        // fs::xv6_file *_ofile[max_open_files]; // 进程打开的文件列表 (文件描述符 -> 文件结构)

        // 进程状态信息
        enum ProcState _state; // 进程当前状态 (unused, used, sleeping, runnable, running, zombie)
        void *_chan;           // 进程睡眠时等待的通道 (例如：某个锁或事件)
        int _killed;           // 进程是否被标记为kill (非零表示被kill)
        int _xstate;           // 进程退出状态，用于父进程wait()获取
        int _pid;              // 进程ID (Process ID)
        Pcb *_parent;           // 父进程的PCB指针

        char _name[16]; // 进程名称 (用于调试)

        // 内存管理相关
        uint64 _kstack = 0;    // 内核栈的虚拟地址
        uint64 _sz;            // 进程用户空间的内存大小 (字节)
        mem::PageTable _pt;     // 用户空间的页表
        TrapFrame *_trapframe; // 保存用户态 TrapFrame 的地址 (用于系统调用和异常处理)

        // 上下文切换
        Context _context; // 保存进程的上下文信息 (寄存器等)，用于进程切换

        // 调度相关
        int _slot;     // 分配给进程的时间片剩余量
        int _priority; // 进程优先级 (0最高，19最低)

        // TODO:共享内存相关
        // uint _shm;             // 共享内存的起始虚拟地址
        // void *_shmva[SHM_NUM]; // 指向共享内存区域的虚拟地址数组
        // uint _shmkeymask;      // 用于标记哪些共享物理内存页被该进程使用

        // 消息队列相关
        uint _mqmask; // 用于标记进程使用的消息队列

        // 运行时间统计
        uint64 _start_tick;     // 进程开始运行时的时钟节拍数
        uint64 _user_ticks;     // 进程在用户态运行的时钟节拍总数
        uint64 _last_user_tick; // 进程上次返回用户态时的时钟节拍数

        uint64 _hp; // 临时堆指针 (注释说明后续会删除)

        // 虚拟内存区域 (VMA) - 注释中提出了疑问，这里保留但需要进一步理解其用途
        struct vma *vm[10]; // 虚拟内存区域数组

    public:
        Pcb();
        void init(const char *lock_name, uint gid);
        void map_kstack(mem::PageTable &pt);

        int get_priority();

        void print_context();

    public:
        Context *get_context() { return &_context; }


    public:
        // fs::Dentry *get_cwd() { return _cwd; }
        void kill() { 
            _lock.acquire();
            _killed = 1; 
            _lock.release();
            }
        Pcb *get_parent() { return _parent; }
        void set_state(ProcState state) { _state = state; }
        void set_xstate(int xstate) { _xstate = xstate; }
        size_t get_sz() { return _sz; }
        // void set_chan(void *chan) { _chan = chan; }
        uint get_pid() { return _pid; }
        uint get_ppid() { return _parent ? _parent->_pid : 0; }
        TrapFrame *get_trapframe() { return _trapframe; }
        uint64 get_kstack() { return _kstack; }
        mem::PageTable& get_pagetable() { return _pt; }
        ProcState get_state() { return _state; }
        char *get_name() { return _name; }
        uint64 get_size() { return _sz; }
        uint64 get_last_user_tick() { return _last_user_tick; }
        uint64 get_user_ticks() { return _user_ticks; }
        // fs::xv6_file *get_open_file(int fd)
        // {
        //     if (fd < 0 || fd >= (int)max_open_files)
        //         return nullptr;
        //     return _ofile[fd];
        // }

        void set_trapframe(TrapFrame *tf) { _trapframe = tf; }

        void set_last_user_tick(uint64 tick) { _last_user_tick = tick; }
        void set_user_ticks(uint64 ticks) { _user_ticks = ticks; }

        bool is_killed() { 
            int k;
            _lock.acquire();
            k = _killed;
            _lock.release();
            return k;
            }
    };

    extern Pcb k_proc_pool[num_process]; // 全局进程池
    void print_context1(Context *ctx); // 打印进程上下文
}

