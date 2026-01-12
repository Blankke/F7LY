#pragma once
#include "devs/block_device.hh"

namespace dev {
class RamDisk : public BlockDevice {
public:
    RamDisk(uint64 start_addr, uint64 size);
    
    virtual long get_block_size() override;
    virtual int read_blocks_sync(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) override;
    virtual int read_blocks(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) override;
    virtual int write_blocks_sync(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) override;
    virtual int write_blocks(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) override;
    virtual int handle_intr() override;
    
    virtual bool read_ready() override { return true; }
    virtual bool write_ready() override { return true; }

private:
    uint64 _start_addr;
    uint64 _size;
};
}
