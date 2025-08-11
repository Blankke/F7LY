//
// Loop Device Implementation for F7LY OS
//

#pragma once

#include "block_device.hh"
#include "char_device.hh"
#include "fs/vfs/file/file.hh"
#include "types.hh"
#include <EASTL/string.h>

namespace dev
{
    // Loop 设备状态结构 (32位版本，用于LOOP_SET_STATUS)
    struct LoopInfo
    {
        uint32_t lo_device;        // 关联的设备号
        uint32_t lo_inode;         // 关联的 inode 号  
        uint32_t lo_rdevice;       // 真实设备号
        uint32_t lo_offset;        // 偏移量
        uint32_t lo_sizelimit;     // 大小限制
        uint32_t lo_number;        // loop 设备编号
        uint32_t lo_encrypt_type;  // 加密类型
        uint32_t lo_encrypt_key_size; // 加密密钥大小
        uint32_t lo_flags;         // 标志位
        uint8_t  lo_file_name[64]; // 文件名
        uint8_t  lo_crypt_name[64];// 加密名
        uint8_t  lo_encrypt_key[32]; // 加密密钥
        uint32_t lo_init[2];       // 初始化数据
    };

    // Loop 设备状态结构 (64位版本，用于LOOP_SET_STATUS64)
    struct LoopInfo64
    {
        uint64_t lo_device;        // 关联的设备号
        uint64_t lo_inode;         // 关联的 inode 号  
        uint64_t lo_rdevice;       // 真实设备号
        uint64_t lo_offset;        // 偏移量
        uint64_t lo_sizelimit;     // 大小限制
        uint32_t lo_number;        // loop 设备编号
        uint32_t lo_encrypt_type;  // 加密类型
        uint32_t lo_encrypt_key_size; // 加密密钥大小
        uint32_t lo_flags;         // 标志位
        uint8_t  lo_file_name[64]; // 文件名
        uint8_t  lo_crypt_name[64];// 加密名
        uint8_t  lo_encrypt_key[32]; // 加密密钥
        uint64_t lo_init[2];       // 初始化数据
    };

    // Loop 设备配置结构
    struct LoopConfig
    {
        uint32_t fd;              // 文件描述符
        uint32_t block_size;      // 块大小
        LoopInfo64 info;          // 设备信息
        uint64_t reserved[8];     // 保留字段
    };

    // Loop 设备标志位
    enum LoopFlags
    {
        LO_FLAGS_READ_ONLY = 1,
        LO_FLAGS_AUTOCLEAR = 4,
        LO_FLAGS_PARTSCAN = 8,
        LO_FLAGS_DIRECT_IO = 16,
    };

    class LoopDevice // : public BlockDevice
    {
    private:
        int _loop_number;                    // loop 设备编号
        bool _is_bound;                      // 是否已绑定文件
        fs::file* _backing_file;             // 后端文件
        uint64_t _offset;                    // 文件偏移
        uint64_t _size_limit;                // 大小限制
        uint32_t _block_size;                // 块大小
        uint32_t _flags;                     // 标志位
        eastl::string _file_name;            // 绑定的文件名
        eastl::string _file_path;            // 绑定的文件路径

        static const uint32_t DEFAULT_BLOCK_SIZE = 512;

    public:
        LoopDevice(int loop_number);
        virtual ~LoopDevice();


        // Loop 设备特有方法
        int set_fd(int fd);                  // 绑定文件描述符
        int clear_fd();                      // 清除绑定
        int set_status(const LoopInfo* info); // 设置状态 (32位版本)
        int get_status(LoopInfo* info);      // 获取状态 (32位版本)
        int set_status(const LoopInfo64* info); // 设置状态 (64位版本)
        int get_status(LoopInfo64* info);    // 获取状态 (64位版本)
        int configure(const LoopConfig* config); // 配置设备
        int set_capacity(uint64_t capacity); // 设置容量
        int set_block_size(uint32_t block_size); // 设置块大小

        // 属性访问
        int get_loop_number() const { return _loop_number; }
        bool is_bound() const { return _is_bound; }
        const eastl::string &get_file_name() const { panic("xxx"); }
        uint64_t get_size() const;


        int _read_write_file(uint64_t offset, void* buffer, size_t size, bool is_write);
        uint64_t _get_file_size();
    };

    // Loop 控制设备
    class LoopControlDevice : public CharDevice
    {
    private:
        static const int MAX_LOOP_DEVICES = 256;
        static LoopDevice* _loop_devices[MAX_LOOP_DEVICES];
        static bool _device_allocated[MAX_LOOP_DEVICES];

    public:
        LoopControlDevice() = default;
        virtual ~LoopControlDevice() = default;

        // CharDevice 接口实现
        virtual DeviceType type() override { return DeviceType::dev_char; }
        virtual int handle_intr() override { return 0; }
        virtual bool read_ready() override { return true; }
        virtual bool write_ready() override { return true; }
        
        // CharDevice 必需的虚函数实现
        virtual bool support_stream() override { return false; }
        virtual int get_char_sync(u8 *c) override { return -1; } // loop-control 不支持字符读取
        virtual int get_char(u8 *c) override { return -1; }
        virtual int put_char_sync(u8 c) override { return -1; }  // loop-control 不支持字符写入
        virtual int put_char(u8 c) override { return -1; }


        // Loop 控制方法
        static int add_loop_device(int number = -1);    // 添加 loop 设备
        static int remove_loop_device(int number);      // 移除 loop 设备
        static int get_free_loop_device();              // 获取空闲 loop 设备编号
        static LoopDevice* get_loop_device(int number); // 获取指定 loop 设备
        static void init_loop_control();                // 初始化 loop 控制

    private:
        static int _find_free_slot();
        static bool _is_valid_loop_number(int number);
    };

    // 全局实例
    extern LoopControlDevice k_loop_control;

} // namespace dev
