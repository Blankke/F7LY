#pragma once

#include "types.hh"
#include "libs/common.hh"
#include "devs/spinlock.hh"
#include "proc/pipe.hh"
#include <EASTL/string.h>
#include <EASTL/unordered_map.h>

namespace fs {

struct FifoInfo {
    proc::ipc::Pipe *pipe;
    int reader_count;
    int writer_count;
    
    FifoInfo() : pipe(nullptr), reader_count(0), writer_count(0) {}
    FifoInfo(proc::ipc::Pipe *p) : pipe(p), reader_count(0), writer_count(0) {}
};

class FifoManager {
private:
    SpinLock _lock;
    eastl::unordered_map<eastl::string, FifoInfo>* _fifo_map;
    
public:
    FifoManager() { }
    
    ~FifoManager() = default;
    
    // 初始化 FIFO 管理器
    void init() {
        _lock.init("fifo_manager");
        _fifo_map = new eastl::unordered_map<eastl::string, FifoInfo>();
    }
    // 获取或创建 FIFO
    proc::ipc::Pipe* get_or_create_fifo(const eastl::string& path);
    
    // 打开 FIFO（增加读者或写者计数）
    bool open_fifo(const eastl::string& path, bool is_writer);
    
    // 关闭 FIFO（减少读者或写者计数）
    void close_fifo(const eastl::string& path, bool is_writer);
    
    // 检查 FIFO 是否有读者
    bool has_readers(const eastl::string& path);
    
    // 检查 FIFO 是否有写者
    bool has_writers(const eastl::string& path);
    
    // 获取 FIFO 信息
    FifoInfo get_fifo_info(const eastl::string& path);
};

// 全局 FIFO 管理器实例
extern FifoManager k_fifo_manager;

} // namespace fs
