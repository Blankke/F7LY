#pragma once
#include "proc.hh"
#include "spinlock.hh"
#include "prlimit.hh"
#include "futex.hh"
#include "fs/vfs/file/normal_file.hh"

namespace tmm
{
    struct tms;
}

namespace proc
{
    constexpr int default_proc_slot = 1; // 默认进程槽位

    class ProcessManager
    {
    private:
        // 核心成员变量
        SpinLock _pid_lock;        // 进程ID锁
        SpinLock _tid_lock;        // 线程ID锁
        SpinLock _wait_lock;       // 等待锁
        int _cur_pid;              // 当前分配的最大PID
        int _cur_tid;              // 当前分配的最大TID
        Pcb *_init_proc;           // 用户init进程
        uint _last_alloc_proc_gid; // 上次分配的进程组ID

    public:
        ProcessManager() = default;
        
        // ==================== 初始化 ====================
        void init(const char *pid_lock_name, const char *tid_lock_name, const char *wait_lock_name);
        void user_init();

        // ==================== 进程基础管理 ====================
        Pcb *get_cur_pcb();
        bool change_state(Pcb *p, ProcState state);
        void alloc_pid(Pcb *p);
        void alloc_tid(Pcb *p);
        Pcb *alloc_proc();
        void freeproc(Pcb *p);
        void freeproc_creation_failed(Pcb *p);
        void sche_proc(Pcb *p);
        Pcb *find_proc_by_pid(int pid);

        // ==================== 进程属性设置 ====================
        void set_slot(Pcb *p, int slot);
        void set_priority(Pcb *p, int priority);
        void set_shm(Pcb *p);
        int set_trapframe(Pcb *p);
        void set_killed(Pcb *p);

        // ==================== 内存管理 ====================
        int growproc(int n);
        long brk(long n);
        long sbrk(long increment);
        
        // 内存映射相关
        int validate_mmap_params(void *addr, size_t length, int prot, int flags, int fd, int offset);
        void *mmap(void *addr, size_t length, int prot, int flags, int fd, int offset, int *errno);
        int munmap(void *addr, size_t length);
        int mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address, void **result_addr);

        // ==================== 进程生命周期 ====================
        int exec(eastl::string path, eastl::vector<eastl::string> argv);
        int execve(eastl::string path, eastl::vector<eastl::string> argv, eastl::vector<eastl::string> envs);
        int load_seg(mem::PageTable &pt, uint64 va, eastl::string &path, uint offset, uint size);
        int clone(uint64 flags, uint64 stack_ptr, uint64 ptid, uint64 tls, uint64 ctid);
        Pcb *fork(Pcb *p, uint64 flags, uint64 stack_ptr, uint64 ctid, bool is_clone3);
        void fork_ret();
        void exit_proc(Pcb *p);           // 底层退出逻辑，不设置xstate
        void do_exit(Pcb *p, int state);  // 正常退出，设置xstate后调用exit_proc
        void do_signal_exit(Pcb *p, int signal_num, bool coredump = false); // 信号退出，设置signal相关xstate后调用exit_proc
        void exit(int state);
        void exit_group(int status);
        int wait4(int child_pid, uint64 addr, int option);
        void reparent(Pcb *p);

        // ==================== 进程调度与同步 ====================
        void sleep(void *chan, SpinLock *lock);
        void wakeup(void *chan);
        int wakeup2(uint64 uaddr, int val, void *uaddr2, int val2);

        // ==================== 文件系统相关 ====================
        int open(int dir_fd, eastl::string path, uint flags, int mode = 0644);
        int close(int fd);
        int fstat(int fd, fs::Kstat *buf);
        int mkdir(int dir_fd, eastl::string path, uint mode);
        int mknod(int dir_fd, eastl::string path, mode_t mode, dev_t dev);
        int unlink(int fd, eastl::string path, int flags);
        int chdir(eastl::string &path);
        int getcwd(char *out_buf);
        int pipe(int *fd, int);
        int alloc_fd(Pcb *p, fs::file *f);
        int alloc_fd(Pcb *p, fs::file *f, int fd);

        // ==================== 信号处理 ====================
        int kill_signal(int pid, int sig);
        int tkill(int tid, int sig);
        int tgkill(int tgid, int tid, int sig);
        void kill_proc(Pcb *p) { p->_killed = 1; }
        int kill_proc(int pid);

        // ==================== 系统调用支持 ====================
        int set_tid_address(uint64 tidptr);
        int set_robust_list(robust_list_head *head, size_t len);
        int prlimit64(int pid, int resource, rlimit64 *new_limit, rlimit64 *old_limit);
        
        // ==================== 工具函数 ====================
        int either_copy_in(void *dst, int user_src, uint64 src, uint64 len);
        int either_copy_out(void *src, int user_dst, uint64 dst, uint64 len);
        void get_cur_proc_tms(tmm::tms *tsv);
        int get_cur_cpuid();

        // ==================== 调试与验证 ====================
        void procdump();
        void debug_process_states();
        bool verify_process_cleanup(int pid);

    private:
        // 私有辅助函数
        bool is_target_child(Pcb *child, Pcb *parent, int child_pid);
        bool has_remaining_threads(Pcb *parent, int target_pid);
    };

    extern ProcessManager k_pm; // 全局进程管理器实例

}