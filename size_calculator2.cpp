#include <iostream>
#include <cstddef>
#include <cstdint>

// 模拟 file 结构体
struct file {
    int f_type;                  // 4 bytes
    uint8_t f_mode;              // 1 byte
    uint8_t f_flags;             // 1 byte
    uint64_t f_pos;              // 8 bytes
    uint16_t f_count;            // 2 bytes
    short f_major;               // 2 bytes
    void *private_data;          // 8 bytes
    int f_owner;                 // 4 bytes
    void *f_ip;                  // 8 bytes
    void *f_pipe;                // 8 bytes
    void *f_extfile;             // 8 bytes
    char f_path[256];            // 256 bytes (MAXPATH)
    uint64_t flagsslow;          // 8 bytes
    uint64_t flagshigh;          // 8 bytes
    uint32_t removed;            // 4 bytes
    // padding for alignment
    char padding[16];
};

// 模拟 devsw 结构体
struct devsw {
    void *read;                  // 8 bytes (函数指针)
    void *write;                 // 8 bytes (函数指针)
};

// 模拟 extended_posix_timer 结构体
struct extended_posix_timer {
    int timer_id;                // 4 bytes
    int clockid;                 // 4 bytes
    struct {
        int sigev_notify;        // 4 bytes
        int sigev_signo;         // 4 bytes
        union {
            int sival_int;       // 4 bytes
            void *sival_ptr;     // 8 bytes
        } sigev_value;
    } event;                     // ~16 bytes
    bool active;                 // 1 byte
    bool armed;                  // 1 byte
    char spec[64];               // ~64 bytes (模拟 itimerspec)
    char expiry_time[32];        // ~32 bytes (模拟 timespec)
    char padding[32];            // padding
};

// 模拟 SpinLock
struct SpinLock {
    char data[64];               // 估计大小
};

// 模拟匿名结构体
struct ftable_struct {
    SpinLock lock;               // ~64 bytes
    file file[100];              // NFILE = 100
};

int main() {
    std::cout << "=== 其他全局变量大小分析 ===" << std::endl;
    
    // 1. extended_posix_timer g_timers[32]
    size_t timer_size = sizeof(extended_posix_timer) * 32;
    std::cout << "1. g_timers[32]:" << std::endl;
    std::cout << "   单个 extended_posix_timer: " << sizeof(extended_posix_timer) << " bytes" << std::endl;
    std::cout << "   总大小: " << timer_size << " bytes" << std::endl;
    
    // 2. devsw devsw[NDEV]  (NDEV = 10)
    size_t devsw_size = sizeof(devsw) * 10;
    std::cout << "2. devsw[10]:" << std::endl;
    std::cout << "   单个 devsw: " << sizeof(devsw) << " bytes" << std::endl;
    std::cout << "   总大小: " << devsw_size << " bytes" << std::endl;
    
    // 3. ftable (匿名结构体)
    size_t ftable_size = sizeof(ftable_struct);
    std::cout << "3. ftable (匿名结构体):" << std::endl;
    std::cout << "   struct file: " << sizeof(file) << " bytes" << std::endl;
    std::cout << "   ftable 总大小: " << ftable_size << " bytes" << std::endl;
    
    // 4. filesystem_t *fs_table[VFS_MAX_FS]  (VFS_MAX_FS = 4)
    size_t fs_table_size = sizeof(void*) * 4;
    std::cout << "4. fs_table[4]:" << std::endl;
    std::cout << "   总大小: " << fs_table_size << " bytes (指针数组)" << std::endl;
    
    // 5. filesystem_op_t *fs_ops_table[VFS_MAX_FS]  (VFS_MAX_FS = 4)
    size_t fs_ops_table_size = sizeof(void*) * 4;
    std::cout << "5. fs_ops_table[4]:" << std::endl;
    std::cout << "   总大小: " << fs_ops_table_size << " bytes (指针数组)" << std::endl;
    
    std::cout << "\n=== 对齐建议 ===" << std::endl;
    
    auto suggest_alignment = [](size_t size, const char* name) {
        std::cout << "- " << name << " (大小: " << size << " bytes)";
        if (size >= 4096) {
            std::cout << " -> 建议对齐到 4096 (页面边界)" << std::endl;
        } else if (size >= 1024) {
            std::cout << " -> 建议对齐到 1024" << std::endl;
        } else if (size >= 512) {
            std::cout << " -> 建议对齐到 512" << std::endl;
        } else if (size >= 256) {
            std::cout << " -> 建议对齐到 256" << std::endl;
        } else if (size >= 128) {
            std::cout << " -> 建议对齐到 128" << std::endl;
        } else if (size >= 64) {
            std::cout << " -> 建议对齐到 64" << std::endl;
        } else {
            std::cout << " -> 建议对齐到 32" << std::endl;
        }
    };
    
    suggest_alignment(timer_size, "g_timers");
    suggest_alignment(devsw_size, "devsw");
    suggest_alignment(ftable_size, "ftable");
    suggest_alignment(fs_table_size, "fs_table");
    suggest_alignment(fs_ops_table_size, "fs_ops_table");
    
    return 0;
}
