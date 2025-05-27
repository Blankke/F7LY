#pragma once
#include "spinlock.hh"
#include "virtio.hh"
#include <virtual_device.hh>

namespace riscv
{
	namespace qemu
	{



		class DiskDriver : public dev::VirtualDevice
		{
		private:
			SpinLock _lock;
			VirtioDriver disk_;

		public:
			virtual dev::DeviceType type() override { return dev::DeviceType::dev_other; }

			virtual bool read_ready() override { return false; }
			virtual bool write_ready() override { return true; }
			virtual int handle_intr() override;
			void identify_device();

		private:
			enum
			{
				mbr_none = 0,
				mbr_chk_ok = 1,
				mbr_gpt = -10,
				mbr_fat32 = 2,
				mbr_ext = 3
			};
			int _check_mbr_partition(u8 *mbr);

		public:
			DiskDriver() = default;
			DiskDriver(const char *lock_name);
		};
		extern DiskDriver disk_driver;
	} // namespace qemu

} // namespace riscv