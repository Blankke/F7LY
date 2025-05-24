#pragma once
#include "proc.hh"
#include "spinlock.hh"


namespace proc
{
    constexpr int default_proc_slot = 1; // 默认进程槽位

    #define MAXARG 32

    //TODO: 文件系统相关
    class ProcessManager
    {
    private:
        SpinLock _pid_lock; // 进程ID锁
        SpinLock _wait_lock; // 等待锁
        int _cur_pid; // 当前分配的最大PID
        Pcb * _init_proc; // 用户init进程
        uint _last_alloc_proc_gid; // 上次分配的进程组ID

    public:
        ProcessManager() = default;

        void init(const char *pid_lock_name, const char *wait_lock_name); // 初始化进程管理器
        Pcb *get_cur_pcb(); // 获取当前进程控制块   xv6:myproc()

        int get_cur_cpuid(); // 获取当前CPU ID    xv6:cpu_id()
        void alloc_pid(Pcb *p); // 分配PID
        Pcb *alloc_proc(); // 分配新的进程控制块
        void freeproc(Pcb *p); // 释放进程
        mem::PageTable proc_pagetable(Pcb *p); // 创建进程页表
        void proc_freepagetable(mem::PageTable pt, uint64 sz); // 释放进程页表
        void user_init(); // 初始化用户进程

        // 这些先不管，要用再写回来
        // void set_slot(Pcb *p, int slot); // 设置进程槽位
        // void set_priority(Pcb *p, int priority); // 设置进程优先级
        // void set_shm(Pcb *p); // 设置共享内存
        // void set_vma(Pcb *p); // 设置虚拟内存区域
        // int set_trapframe(Pcb *p); // 设置陷阱帧
        // bool change_state(Pcb *p, ProcState state); // 改变进程状态

        int either_copy_in( void *dst, int user_src, uint64 src, uint64 len );
        int either_copy_out( void *src, int user_dst, uint64 dst, uint64 len );

        void procdump(); // 打印进程列表 debug


        int exec(const char *path, const char *argv[]); // 执行新程序
        int wait(int child_pid, uint64 addr); // 等待子进程
        // int load_seg(mem::PageTable &pt, uint64 va, fs::fat::Fat32DirEntry *de, uint offset, uint size); // 加载段到页表
        void sleep(void *chan, SpinLock *lock); // 进程睡眠
        void wakeup(void *chan); // 唤醒进程
        int kill_proc(int pid); // 杀死进程
        void set_killed(Pcb *p); // 设置进程被杀死


        void exit(int state); // 进程退出
        int fork(uint64 usp); // 创建子进程（带用户栈指针）
        int fork(); // 创建子进程
        void fork_ret(); // fork返回处理
        int brk(int n); // 扩展/收缩数据段
        int open(int dir_fd, const char *path, uint flags); // 打开文件
        int close(int fd); // 关闭文件
        // int fstat(int fd, struct fs::Kstat *buf); // 获取文件状态
        int chdir(const char *path); // 改变当前目录
        int getcwd(char *out_buf); // 获取当前工作目录
        int mmap(int fd, int map_size); // 内存映射
        int unlink(int fd, const char *path, int flags); // 删除文件
        int pipe(int *fd, int); // 管道
        int alloc_fd(Pcb *p, fs::file *f); // 分配文件描述符
        // int alloc_fd(Pcb *p, fs::xv6_file *f, int fd); // 分配文件描述符
        // void get_cur_proc_tms(tmm::tms *tsv); // 获取当前进程时间信息



    private:
        void _proc_create_vm(Pcb *p); // 创建进程虚拟内存

    public: // ================ 测试函数 ================
        void vectortest();
        void stringtest();
        void maptest();
        void hashtest();
    };

    extern ProcessManager k_pm; // 全局进程管理器实例

}
