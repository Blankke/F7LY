#include "fifo_manager.hh"
#include "printer.hh"

namespace fs {

FifoManager k_fifo_manager;

proc::ipc::Pipe* FifoManager::get_or_create_fifo(const eastl::string& path) {
    _lock.acquire();
    printfGreen("FifoManager: get_or_create_fifo called for path: %s\n", path.c_str());
 if(!_fifo_map->empty())   
{    auto it = _fifo_map->find(path);
    if (it != _fifo_map->end()) {
        // FIFO 已存在，返回现有的 pipe
        proc::ipc::Pipe* pipe = it->second.pipe;
        _lock.release();
        return pipe;
    }
    }
    
    // 创建新的 FIFO
    proc::ipc::Pipe* new_pipe = new proc::ipc::Pipe();
    _fifo_map->insert({path,FifoInfo(new_pipe)});
    
    printfGreen("FifoManager: Created new FIFO: %s\n", path.c_str());
    _lock.release();
    return new_pipe;
}

bool FifoManager::open_fifo(const eastl::string& path, bool is_writer) {
    _lock.acquire();
    
    auto it = _fifo_map->find(path);
    if (it == _fifo_map->end()) {
        // FIFO 不存在，无法打开
        _lock.release();
        return false;
    }
    
    if (is_writer) {
        it->second.writer_count++;
        it->second.pipe->_write_is_open = true;
        printfCyan("FifoManager: Opened FIFO %s for writing, writer_count: %d\n", 
                   path.c_str(), it->second.writer_count);
    } else {
        it->second.reader_count++;
        it->second.pipe->_read_is_open = true;
        printfCyan("FifoManager: Opened FIFO %s for reading, reader_count: %d\n", 
                   path.c_str(), it->second.reader_count);
    }
    
    _lock.release();
    return true;
}

void FifoManager::close_fifo(const eastl::string& path, bool is_writer) {
    _lock.acquire();
    
    auto it = _fifo_map->find(path);
    if (it == _fifo_map->end()) {
        _lock.release();
        return;
    }
    
    if (is_writer) {
        it->second.writer_count--;
        if (it->second.writer_count <= 0) {
            it->second.writer_count = 0;
            it->second.pipe->_write_is_open = false;
        }
        printfMagenta("FifoManager: Closed FIFO %s writer, writer_count: %d\n", 
                      path.c_str(), it->second.writer_count);
    } else {
        it->second.reader_count--;
        if (it->second.reader_count <= 0) {
            it->second.reader_count = 0;
            it->second.pipe->_read_is_open = false;
        }
        printfMagenta("FifoManager: Closed FIFO %s reader, reader_count: %d\n", 
                      path.c_str(), it->second.reader_count);
    }
    
    // 如果没有读者和写者了，删除 FIFO
    if (it->second.reader_count == 0 && it->second.writer_count == 0) {
        delete it->second.pipe;
        _fifo_map->erase(it);
        printfRed("FifoManager: Removed FIFO: %s\n", path.c_str());
    }
    
    _lock.release();
}

bool FifoManager::has_readers(const eastl::string& path) {
    _lock.acquire();
    
    auto it = _fifo_map->find(path);
    bool result = (it != _fifo_map->end()) && (it->second.reader_count > 0);
    
    _lock.release();
    return result;
}

bool FifoManager::has_writers(const eastl::string& path) {
    _lock.acquire();
    
    auto it = _fifo_map->find(path);
    bool result = (it != _fifo_map->end()) && (it->second.writer_count > 0);
    
    _lock.release();
    return result;
}

FifoInfo FifoManager::get_fifo_info(const eastl::string& path) {
    _lock.acquire();
    
    auto it = _fifo_map->find(path);
    FifoInfo result;
    if (it != _fifo_map->end()) {
        result = it->second;
    }
    
    _lock.release();
    return result;
}

} // namespace fs
