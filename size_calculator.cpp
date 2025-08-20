#include <iostream>
#include <cstddef>
#include <cstdint>

// 模拟相关的结构体和类型定义
typedef uint64_t uint64;
typedef uint32_t uint32;
typedef int64_t int64;

struct SpinLock {
    int dummy;
    char padding[60]; // 估计的大小
};

struct TrapFrame {
    char data[1024]; // 估计的大小
};

struct Context {
    char data[256]; // 估计的大小
};

enum ProcState {
    UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE
};

// 模拟Pcb类的大致大小
class Pcb {
public:
    SpinLock _lock;               // ~64 bytes
    int _global_id;               // 4 bytes
    int _pid;                     // 4 bytes  
    int _tid;                     // 4 bytes
    void* _parent;                // 8 bytes
    char _name[30];               // 30 bytes
    // eastl::string exe;          // ~32 bytes
    
    int _ppid, _pgid, _tgid;      // 12 bytes
    int _sid;                     // 4 bytes
    uint32 _uid, _euid, _suid, _fsuid; // 16 bytes
    uint32 _gid, _egid, _sgid, _fsgid; // 16 bytes
    
    ProcState _state;             // 4 bytes
    void* _chan;                  // 8 bytes
    int _killed;                  // 4 bytes
    int _xstate;                  // 4 bytes
    int _slot;                    // 4 bytes
    int _priority;                // 4 bytes
    
    uint64 _kstack;               // 8 bytes
    TrapFrame* _trapframe;        // 8 bytes
    void* _memory_manager;        // 8 bytes
    Context _context;             // ~256 bytes
    
    // 文件系统相关
    void* _cwd;                   // 8 bytes
    // eastl::string _cwd_name;   // ~32 bytes
    void* _ofile;                 // 8 bytes
    uint32 _umask;                // 4 bytes
    
    // 同步相关
    void* _futex_addr;            // 8 bytes
    uint64 _clear_tid_addr;       // 8 bytes
    void* _robust_list;           // 8 bytes
    
    // 信号相关
    void* _sigactions;            // 8 bytes
    uint64 _sigmask;              // 8 bytes
    uint64 _signal;               // 8 bytes
    void* sig_frame;              // 8 bytes
    char _alt_stack[128];         // 估计大小
    bool _on_sigstack;            // 1 byte
    
    // 资源限制数组 (估计)
    char _rlim_vec[20 * 16];      // 估计20个限制，每个16字节
    
    // 时间统计
    uint64 _start_tick;           // 8 bytes
    uint64 _user_ticks;           // 8 bytes  
    uint64 _last_user_tick;       // 8 bytes
    uint64 _kernel_entry_tick;    // 8 bytes
    uint64 _stime;                // 8 bytes
    uint64 _utime;                // 8 bytes
    uint64 _cutime;               // 8 bytes
    uint64 _cstime;               // 8 bytes
    uint64 _start_time;           // 8 bytes
    uint64 _start_boottime;       // 8 bytes
    
    // padding for alignment
    char padding[128];
};

class ProcessManager {
public:
    SpinLock _pid_lock;           // ~64 bytes
    SpinLock _tid_lock;           // ~64 bytes  
    SpinLock _wait_lock;          // ~64 bytes
    uint32 _cur_pid;              // 4 bytes
    uint32 _cur_tid;              // 4 bytes
    void* _init_proc;             // 8 bytes
    uint32 _last_alloc_proc_gid;  // 4 bytes
    
    char padding[128];            // 其他成员的估计大小
};

class Cpu {
private:
    void* _cur_proc;              // 8 bytes
    Context _context;             // ~256 bytes (已对齐128)
    int _num_off;                 // 4 bytes (已对齐128)
    int _int_ena;                 // 4 bytes
    
    char padding[128];            // 其他成员的估计大小
};

int main() {
    std::cout << "=== 全局变量大小分析 ===" << std::endl;
    
    std::cout << "1. Pcb 结构体大小: " << sizeof(Pcb) << " bytes" << std::endl;
    std::cout << "   k_proc_pool[90] 总大小: " << sizeof(Pcb) * 90 << " bytes" << std::endl;
    
    std::cout << "2. ProcessManager 结构体大小: " << sizeof(ProcessManager) << " bytes" << std::endl;
    
    std::cout << "3. Cpu 结构体大小: " << sizeof(Cpu) << " bytes" << std::endl;
    std::cout << "   k_cpus[1] 总大小: " << sizeof(Cpu) * 1 << " bytes" << std::endl;
    
    std::cout << "\n=== 对齐建议 ===" << std::endl;
    std::cout << "根据变量大小，建议的对齐值：" << std::endl;
    
    size_t pcb_array_size = sizeof(Pcb) * 90;
    std::cout << "- k_proc_pool (大小: " << pcb_array_size << " bytes)";
    if (pcb_array_size >= 4096) {
        std::cout << " -> 建议对齐到 4096 (页面边界)" << std::endl;
    } else if (pcb_array_size >= 1024) {
        std::cout << " -> 建议对齐到 1024" << std::endl;
    } else if (pcb_array_size >= 512) {
        std::cout << " -> 建议对齐到 512" << std::endl;
    } else if (pcb_array_size >= 256) {
        std::cout << " -> 建议对齐到 256" << std::endl;
    } else if (pcb_array_size >= 128) {
        std::cout << " -> 建议对齐到 128" << std::endl;
    } else {
        std::cout << " -> 建议对齐到 64" << std::endl;
    }
    
    size_t pm_size = sizeof(ProcessManager);
    std::cout << "- k_pm (大小: " << pm_size << " bytes)";
    if (pm_size >= 512) {
        std::cout << " -> 建议对齐到 512" << std::endl;
    } else if (pm_size >= 256) {
        std::cout << " -> 建议对齐到 256" << std::endl;
    } else if (pm_size >= 128) {
        std::cout << " -> 建议对齐到 128" << std::endl;
    } else {
        std::cout << " -> 建议对齐到 64" << std::endl;
    }
    
    size_t cpu_array_size = sizeof(Cpu) * 1;
    std::cout << "- k_cpus (大小: " << cpu_array_size << " bytes)";
    if (cpu_array_size >= 512) {
        std::cout << " -> 建议对齐到 512" << std::endl;
    } else if (cpu_array_size >= 256) {
        std::cout << " -> 建议对齐到 256" << std::endl;
    } else if (cpu_array_size >= 128) {
        std::cout << " -> 建议对齐到 128" << std::endl;
    } else {
        std::cout << " -> 建议对齐到 64" << std::endl;
    }
    
    return 0;
}
