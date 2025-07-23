//
// Loop Device Implementation for F7LY OS
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#include "loop_device.hh"
#include "device_manager.hh"
#include "proc.hh"
#include "proc_manager.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_utils.hh"
#include "klib.hh"
#include "printer.hh"
#include "mem/mem.hh"
#include <EASTL/algorithm.h>

namespace dev
{
    // 静态成员初始化
    LoopDevice* LoopControlDevice::_loop_devices[LoopControlDevice::MAX_LOOP_DEVICES] = {nullptr};
    bool LoopControlDevice::_device_allocated[LoopControlDevice::MAX_LOOP_DEVICES] = {false};
    LoopControlDevice k_loop_control;

    // LoopDevice 实现
    LoopDevice::LoopDevice(int loop_number)
        : _loop_number(loop_number)
        , _is_bound(false)
        , _backing_file(nullptr)
        , _offset(0)
        , _size_limit(0)
        , _block_size(DEFAULT_BLOCK_SIZE)
        , _flags(0)
    {
    }

    LoopDevice::~LoopDevice()
    {
        clear_fd();
    }



    int LoopDevice::set_fd(int fd)
    {
        if (_is_bound) {
            return -1; // 已绑定
        }

        // 从当前进程获取文件对象
        auto pcb = proc::k_pm.get_cur_pcb();
        if (!pcb) {
            return -1;
        }

        auto file = pcb->get_open_file(fd);
        if (!file) {
            return -1; // 无效的文件描述符
        }

        // 检查文件类型


        _backing_file = file;
        _is_bound = true;
        _file_name = "unknown"; // 可以后续改进获取文件名

        return 0;
    }

    int LoopDevice::clear_fd()
    {
        if (!_is_bound) {
            return -1; // 未绑定
        }

        _backing_file = nullptr;
        _is_bound = false;
        _offset = 0;
        _size_limit = 0;
        _flags = 0;
        _file_name.clear();

        return 0;
    }

    int LoopDevice::set_status(const LoopInfo* info)
    {
        if (!info) {
            return -1;
        }

        _offset = info->lo_offset;
        _size_limit = info->lo_sizelimit;
        _flags = info->lo_flags;
        
        // 复制文件名
        if (info->lo_file_name[0] != 0) {
            size_t name_len = 0;
            // 手动计算字符串长度，最大63个字符
            for (size_t i = 0; i < 63 && info->lo_file_name[i] != 0; i++) {
                name_len++;
            }
            _file_name = eastl::string((const char*)info->lo_file_name, name_len);
        }

        return 0;
    }

    int LoopDevice::set_status(const LoopInfo64* info)
    {
        if (!info) {
            return -1;
        }

        _offset = info->lo_offset;
        _size_limit = info->lo_sizelimit;
        _flags = info->lo_flags;
        
        // 复制文件名
        if (info->lo_file_name[0] != 0) {
            size_t name_len = 0;
            // 手动计算字符串长度，最大63个字符
            for (size_t i = 0; i < 63 && info->lo_file_name[i] != 0; i++) {
                name_len++;
            }
            _file_name = eastl::string((const char*)info->lo_file_name, name_len);
        }

        return 0;
    }

    int LoopDevice::get_status(LoopInfo* info)
    {
        if (!info) {
            return -1;
        }

        memset(info, 0, sizeof(LoopInfo));
        
        info->lo_number = _loop_number;
        info->lo_offset = (uint32_t)_offset;
        info->lo_sizelimit = (uint32_t)_size_limit;
        info->lo_flags = _flags;
        
        // 复制文件名
        if (!_file_name.empty()) {
            size_t name_len = eastl::min(_file_name.length(), size_t(63));
            memcpy(info->lo_file_name, _file_name.c_str(), name_len);
            info->lo_file_name[name_len] = 0;
        }

        return 0;
    }

    int LoopDevice::get_status(LoopInfo64* info)
    {
        if (!info) {
            return -1;
        }

        memset(info, 0, sizeof(LoopInfo64));
        
        info->lo_number = _loop_number;
        info->lo_offset = _offset;
        info->lo_sizelimit = _size_limit;
        info->lo_flags = _flags;
        
        // 复制文件名
        if (!_file_name.empty()) {
            size_t name_len = eastl::min(_file_name.length(), size_t(63));
            memcpy(info->lo_file_name, _file_name.c_str(), name_len);
            info->lo_file_name[name_len] = 0;
        }

        return 0;
    }

    int LoopDevice::configure(const LoopConfig* config)
    {
        if (!config) {
            return -1;
        }

        // 设置文件描述符
        int ret = set_fd(config->fd);
        if (ret < 0) {
            return ret;
        }

        // 设置块大小
        if (config->block_size > 0) {
            _block_size = config->block_size;
        }

        // 设置状态信息
        return set_status(&config->info);
    }

    int LoopDevice::set_capacity(uint64_t capacity)
    {
        _size_limit = capacity;
        return 0;
    }

    int LoopDevice::set_block_size(uint32_t block_size)
    {
        if (block_size == 0 || (block_size & (block_size - 1)) != 0) {
            return -1; // 块大小必须是2的幂
        }
        _block_size = block_size;
        return 0;
    }

    uint64_t LoopDevice::get_size() const
    {
        panic("LoopDevice::get_size() not implemented yet");
    }

    int LoopDevice::_read_write_file(uint64_t offset, void* buffer, size_t size, bool is_write)
    {
        panic("LoopDevice::_read_write_file() not implemented yet");
    }

    uint64_t LoopDevice::_get_file_size()
    {
        panic("LoopDevice::_get_file_size() not implemented yet");
    }

    // LoopControlDevice 实现
    void LoopControlDevice::init_loop_control()
    {
        // 注册 loop-control 设备
        k_devm.register_char_device(&k_loop_control, "loop-control");
        
        // 初始化数组
        for (int i = 0; i < MAX_LOOP_DEVICES; i++) {
            _loop_devices[i] = nullptr;
            _device_allocated[i] = false;
        }
    }

    int LoopControlDevice::add_loop_device(int number)
    {
        if (number < 0) {
            number = _find_free_slot();
        }
        
        if (!_is_valid_loop_number(number)) {
            return -1;
        }
        
        if (_device_allocated[number]) {
            return -1; // 已存在
        }

        // 创建新的 loop 设备
        LoopDevice* loop_dev = new LoopDevice(number);
        if (!loop_dev) {
            return -1;
        }

        _loop_devices[number] = loop_dev;
        _device_allocated[number] = true;

        // 注册到设备管理器
        [[maybe_unused]]char dev_name[32];
        dev_name[0] = 'l'; dev_name[1] = 'o'; dev_name[2] = 'o'; dev_name[3] = 'p';
        
        // 手动转换数字到字符串
        int temp_num = number;
        int digits = 0;
        int temp = temp_num;
        do {
            digits++;
            temp /= 10;
        } while (temp > 0);
        
        dev_name[4 + digits] = '\0';
        for (int i = digits - 1; i >= 0; i--) {
            dev_name[4 + i] = '0' + (temp_num % 10);
            temp_num /= 10;
        }
        //TODO
        // k_devm.register_block_device(loop_dev, dev_name);

        return number;
    }

    int LoopControlDevice::remove_loop_device(int number)
    {
        if (!_is_valid_loop_number(number) || !_device_allocated[number]) {
            return -1;
        }

        LoopDevice* loop_dev = _loop_devices[number];
        if (loop_dev && loop_dev->is_bound()) {
            return -1; // 设备正在使用中
        }

        // 从设备管理器移除
        char dev_name[32];
        dev_name[0] = 'l'; dev_name[1] = 'o'; dev_name[2] = 'o'; dev_name[3] = 'p';
        
        // 手动转换数字到字符串
        int temp_num = number;
        int digits = 0;
        int temp = temp_num;
        do {
            digits++;
            temp /= 10;
        } while (temp > 0);
        
        dev_name[4 + digits] = '\0';
        for (int i = digits - 1; i >= 0; i--) {
            dev_name[4 + i] = '0' + (temp_num % 10);
            temp_num /= 10;
        }
        
        k_devm.remove_block_device(dev_name);

        // 释放设备
        delete loop_dev;
        _loop_devices[number] = nullptr;
        _device_allocated[number] = false;

        return 0;
    }

    int LoopControlDevice::get_free_loop_device()
    {
        return _find_free_slot();
    }

    LoopDevice* LoopControlDevice::get_loop_device(int number)
    {
        if (!_is_valid_loop_number(number) || !_device_allocated[number]) {
            return nullptr;
        }
        return _loop_devices[number];
    }

    int LoopControlDevice::_find_free_slot()
    {
        for (int i = 0; i < MAX_LOOP_DEVICES; i++) {
            if (!_device_allocated[i]) {
                return i;
            }
        }
        return -1; // 没有空闲槽位
    }

    bool LoopControlDevice::_is_valid_loop_number(int number)
    {
        return number >= 0 && number < MAX_LOOP_DEVICES;
    }

} // namespace dev
