//
// Copy from Li Shuang ( pseudonym ) on 2024-07-15
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#pragma once

#include "virtual_device.hh"
#include "types.hh"
namespace dev
{
	struct BufferDescriptor
	{
		u64 buf_addr;
		u32 buf_size;
	};

	class BlockDevice : public VirtualDevice
	{
	public:
		BlockDevice() = default;
		virtual DeviceType type() override { return DeviceType::dev_block; }
		// 容量与逻辑块大小都是设备自身属性。上层不能拿平台根盘容量替代
		// ramdisk/loop 等已注册设备，否则边界检查会作用在错误的设备上。
		virtual uint64 capacity_bytes() const = 0;
		virtual long get_block_size() = 0;
		virtual int read_blocks_sync(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) = 0;
		virtual int read_blocks(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) = 0;
		virtual int write_blocks_sync(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) = 0;
		virtual int write_blocks(long start_block, long block_count, BufferDescriptor *buf_list, int buf_count) = 0;
		virtual int handle_intr() = 0;
	};

} // namespace dev
