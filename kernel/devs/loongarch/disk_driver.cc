// #include "loongarch/virtio.hh"
// #include <device_manager.hh>
// #include <mbr.hh>
// #include "riscv/qemu.hh"
// #include "disk_driver.hh"
// #include <new>
// #include "physical_memory_manager.hh"
// #include "printer.hh"
// #include "pci.hh"
// namespace loongarch
// {
// 	namespace qemu
// 	{

// 		DiskDriver disk_driver;
// 		DiskDriver::DiskDriver(const char *lock_name) //: disk_((void *)VIRTIO0, 0)
// 		{
// 			_lock.init(lock_name);

// 			pci_device device = pci_device_probe(PCI_VENDOR_ID_REDHAT_QUMRANET, 0x1001);
// 			new ( &disk_ ) VirtioDriver( device, 0 );

// 			dev::k_devm.register_device(this, "Disk driver");
// 		}

// 		int DiskDriver::handle_intr()
// 		{
// 			return disk_.handle_intr();
// 		}

// 		void DiskDriver::identify_device()
// 		{

// 			[[maybe_unused]] u32 *id_data = (u32 *)mem::k_pmm.alloc_page();

// 			u8 *mbr_data = new u8[520]; // actually just use 512-bytes

// 			dev::BufferDescriptor bdp = {.buf_addr = (u64)mbr_data, .buf_size = 520};
// 			disk_.read_blocks_sync(0, 1, &bdp, 1);

// 			if (int rc = _check_mbr_partition(mbr_data) < 0)
// 			{
// 				if (rc == mbr_gpt)
// 				{
// 					panic("%s设备的硬盘类型是GPT, 但是驱动只支持MBR类型的硬盘! ",
// 						  disk_._dev_name);
// 					mem::k_pmm.free_page((void *)id_data);
// 					delete[] mbr_data;
// 					return;
// 				}
// 				else
// 				{
// 					panic("%s设备硬盘检查失败, 未知的错误码 %d", disk_._dev_name, rc);
// 					mem::k_pmm.free_page((void *)id_data);
// 					delete[] mbr_data;
// 					return;
// 				}
// 			}
// 			mem::k_pmm.free_page((void *)id_data);
// 			delete[] mbr_data;
// 		}

// 		int DiskDriver::_check_mbr_partition(u8 *mbr)
// 		{
// 			dev::Mbr *disk_mbr = (dev::Mbr *)mbr;
// 			dev::DiskPartTableEntry copy_entrys[4] __attribute__((aligned(8)));

// 			u8 *pf = (u8 *)disk_mbr->partition_table, *tf = (u8 *)copy_entrys;
// 			for (ulong i = 0; i < sizeof copy_entrys; ++i)
// 			{
// 				tf[i] = pf[i];
// 			}

// 			// display all partition
// 			// printf("打印 MBR 分区表");
// 			// for (int i = 0; i< 4; ++i)
// 			// {
// 			// 	dev::DiskPartTableEntry &part = copy_entrys[i];
// 			// 	printf("分区 %d : 分区状态=%x 分区类型=%x 起始LBA=%x 分区总扇区数=%x\n", i,
// 			// 		   part.drive_attribute, part.part_type, part.lba_addr_start, part.sector_count);
// 			// }

// 			for (int i = 0; i < 4; ++i)
// 			{
// 				auto &part = copy_entrys[i];
// 				if (part.part_type == 0)
// 					continue;
// 				if (part.part_type == 0xEE) // this MBR is the protective MBR for GPT disk
// 				{
// 					return mbr_gpt;
// 				}

// 				if (part.part_type == dev::mbr_part_ext)
// 				{
// 					disk_._partition_name[i][4] = '0' + i;

// 					new (&disk_._disk_partition[i]) dev::DiskPartitionDevice(
// 						(dev::BlockDevice *)&disk_, part.lba_addr_start,
// 						(const char *)disk_._partition_name[i], dev::mbr_part_ext);

// 					dev::k_devm.register_block_device((dev::BlockDevice *)&disk_._disk_partition[i],
// 													  (const char *)disk_._partition_name[i]);
// 				}

// 				else if (part.part_type == dev::mbr_part_fat32_chs_dos ||
// 						 part.part_type == dev::mbr_part_fat32_lba_dos ||
// 						 part.part_type == dev::mbr_part_fat32_windows)
// 				{
// 					printfRed("fat32 partition not implement");
// 					return mbr_fat32;
// 				}
// 			}
// 			return 0;
// 		}

// 	} // namespace qemu

// } // namespace riscv