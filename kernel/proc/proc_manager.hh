#pragma once
#include "proc.hh"
#include "spinlock.hh"
#include "prlimit.hh"
#include "futex.hh"
namespace tmm
{
    struct tms;
} // namespace tmm
namespace proc
{
    constexpr int default_proc_slot = 1; // 默认进程槽位 TODO:TBD

#define MAXARG 32

    // TODO: 文件系统相关
    class ProcessManager
    {
    private:
        SpinLock _pid_lock;        // 进程ID锁
        SpinLock _tid_lock;        // 线程ID锁
        SpinLock _wait_lock;       // 等待锁
        int _cur_pid;              // 当前分配的最大PID
        int _cur_tid;              // 当前分配的最大TID
        Pcb *_init_proc;           // 用户init进程
        uint _last_alloc_proc_gid; // 上次分配的进程组ID

    public:
        ProcessManager() = default;

        void init(const char *pid_lock_name, const char *tid_lock_name, const char *wait_lock_name);

        Pcb *get_cur_pcb();
        bool change_state(Pcb *p, ProcState state);
        void alloc_pid(Pcb *p);
        void alloc_tid(Pcb *p);
        Pcb *alloc_proc();

        void set_slot(Pcb *p, int slot);
        void set_priority(Pcb *p, int priority);
        void set_shm(Pcb *p);
        // void set_vma( Pcb *p );
        int set_trapframe(Pcb *p);
        void set_killed(Pcb *p);

        mem::PageTable proc_pagetable(Pcb *p);
        void proc_freepagetable(mem::PageTable &pt, uint64 sz);
        void freeproc(Pcb *p);
        void sche_proc(Pcb *p);

        // 这些先不管，要用再写回来
        // void set_slot(Pcb *p, int slot); // 设置进程槽位
        // void set_priority(Pcb *p, int priority); // 设置进程优先级
        // void set_shm(Pcb *p); // 设置共享内存
        // void set_vma(Pcb *p); // 设置虚拟内存区域
        // int set_trapframe(Pcb *p); // 设置陷阱帧
        // bool change_state(Pcb *p, ProcState state); // 改变进程状态

        int either_copy_in(void *dst, int user_src, uint64 src, uint64 len);
        int either_copy_out(void *src, int user_dst, uint64 dst, uint64 len);

        void procdump(); // 打印进程列表 debug

        int exec(eastl::string path, eastl::vector<eastl::string> argv); // 执行新程序
        int growproc(int n);                                             // 扩展进程内存
        int execve(eastl::string path, eastl::vector<eastl::string> argv, eastl::vector<eastl::string> envs);
        int wait4(int child_pid, uint64 addr, int option);
        int load_seg(mem::PageTable &pt, uint64 va, eastl::string &path, uint offset, uint size);

        void sleep(void *chan, SpinLock *lock);
        void wakeup(void *chan);
        int wakeup2(uint64 uaddr, int val, void *uaddr2, int val2);
        void exit_proc(Pcb *p, int state);
        void exit(int state);
        int clone(uint64 flags, uint64 stack_ptr, uint64 ptid, uint64 tls, uint64 ctid);
        Pcb *fork(Pcb *p, uint64 flags, uint64 stack_ptr, uint64 ctid, bool is_clone3);
        void fork_ret();
        long brk(long n);
        long sbrk(long increment);
        int open(int dir_fd, eastl::string path, uint flags, int mode = 0644);
        int mkdir(int dir_fd, eastl::string path, uint mode);
        int mknod(int dir_fd, eastl::string path, mode_t mode, dev_t dev);
        int close(int fd);
        int fstat(int fd, fs::Kstat *buf);
        int chdir(eastl::string &path);
        int getcwd(char *out_buf);
        int validate_mmap_params(void *addr, size_t length, int prot, int flags, int fd, int offset);
        void *mmap(void *addr, size_t length, int prot, int flags, int fd, int offset, int *errno);
        int munmap(void *addr, size_t length);
        int mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address, void **result_addr);
        int unlink(int fd, eastl::string path, int flags);
        int pipe(int *fd, int);
        int set_tid_address(uint64 tidptr);
        int set_robust_list(robust_list_head *head, size_t len);
        int prlimit64(int pid, int resource, rlimit64 *new_limit, rlimit64 *old_limit);
        void exit_group(int status);
        void reparent(Pcb *p);

        void user_init();

        int alloc_fd(Pcb *p, fs::file *f);
        int alloc_fd(Pcb *p, fs::file *f, int fd);

        void get_cur_proc_tms(tmm::tms *tsv);
        int get_cur_cpuid();

        // 信号相关
        int kill_signal(int pid, int sig);
        int tkill(int tid, int sig);
        int tgkill(int tgid, int tid, int sig);  // 发送信号给指定线程组中的指定线程

    public:
        void kill_proc(Pcb *p) { p->_killed = 1; }
        int kill_proc(int pid);

    private:
        void _proc_create_vm(Pcb *p, mem::PageTable &pt);
        void _proc_create_vm(Pcb *p);
    };

    extern ProcessManager k_pm; // 全局进程管理器实例

}
