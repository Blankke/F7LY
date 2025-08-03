// //
// // driver for qemu's virtio disk device.
// // uses qemu's mmio interface to virtio.
// // qemu presents a "legacy" virtio interface.
// //
// // qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
// //

// #include "loongarch/virtio.hh"
// #include <device_manager.hh>
// #include "printer.hh"
// #include "klib.hh"
// #include "proc_manager.hh"
// #ifdef RISCV
// #include "mem/riscv/pagetable.hh"
// #elif defined (LOONGARCH)
// #include "mem/loongarch/pagetable.hh"
// #endif
// #include "platform.hh"
// #include "pte.hh"
// #include "cpu.hh"
// #include "loongarch/virtio.hh"
// #include "pci.hh"

// namespace loongarch
// {
// 	namespace qemu
// 	{
//     VirtioDriver::VirtioDriver(pci_device device, int port_id)
//     {
//       _pci_dev = pci_config_address( device.bus, device.device, device.function );

//       uint8 status = 0;
//       _port_id	  = port_id;

//       disk.vdisk_lock.init( "virtio_disk" );
//       virtio_pci_read_caps( &virtio_blk_hw, _pci_dev );

// 	  // Check PCI vendor and device ID
//       // 读取设备ID和厂商ID
//       // uint32_t id_reg	   = API_PciDevConfigReadDword( _pci_dev, PCI_VENDOR_ID );
//       // uint16_t vendor_id = id_reg & 0xFFFF;
//       // uint16_t device_id = ( id_reg >> 16 ) & 0xFFFF;
//       // hsai_printf( "vendor_id:%d,device_id:%d", vendor_id, device_id );

//       // if ( vendor_id != PCI_VENDOR_ID_REDHAT_QUMRANET || device_id < 0x1000 ||
//       //   device_id > 0x103f )
//       // {
//       //   panic( "could not find virtio disk" );
//       // }
      
//       // Enable PCI bus mastering
//       uint16_t cmd = API_PciDevConfigReadWord(_pci_dev, PCI_COMMAND);
//       API_PciDevConfigWriteWord(_pci_dev, PCI_COMMAND, cmd | PCI_COMMAND_MASTER | PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
//       volatile uint32 *base = (volatile uint32 *) _pci_dev;

//       for(int i = 0; i < 6; i++) {
//           uint32 old = base[4+i];
//           // printf("bar%d origin value: 0x%08x\t", i, old);
//           if(old & 0x1) {
//               // printf("IO space\n");
//               continue;                                      // 未用到 IO 空间, 暂时也不进行分配等
//           } else {                                           // 仅为 Mem 空间进行进一步的处理
//               // printf("Mem space\t");
//           }
//           if(old & 0x4) {
//               // printf("%x %x\n", base[4 + i], base[4 + i + 1]);             // 64bit 系统映射
//               base[4+i] = 0xffffffff;
//               base[4+i+1] = 0xffffffff;
//               __sync_synchronize();

//               uint64 sz = ((uint64)base[4+i+1] << 32) | base[4+i];
//               sz = ~(sz & 0xFFFFFFFFFFFFFFF0) + 1;
//               uint64 mem_addr = pci_alloc_mmio(sz);
//               // 写入分配的大小
//               base[4+i] = (uint32)(mem_addr);
//               base[4+i+1] = (uint32)(mem_addr >> 32);
//               __sync_synchronize();
//               i++;                                    // 跳过下一个 BAR
//           }
//       }

// 	    // Set device status
//       // Virtio设备初始化
//       // 1 重置设备
//       virtio_pci_set_status(&virtio_blk_hw, 0);
//       // 2 设置状态：已识别设备
//       status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
//       virtio_pci_set_status(&virtio_blk_hw, status);

//       // 3 设置状态：驱动已加载
//       status |= VIRTIO_CONFIG_S_DRIVER;
//       virtio_pci_set_status(&virtio_blk_hw, status);

//       // 特性协商
//       // Negotiate features
//       uint64 features = virtio_pci_get_device_features(&virtio_blk_hw);

//       features &= ~( 1 << VIRTIO_BLK_F_RO );
//       features &= ~( 1 << VIRTIO_BLK_F_SCSI );
//       features &= ~( 1 << VIRTIO_BLK_F_CONFIG_WCE );
//       features &= ~( 1 << VIRTIO_BLK_F_MQ ); // 禁用多队列
//       features &= ~( 1 << VIRTIO_F_ANY_LAYOUT );
//       features &= ~( 1 << VIRTIO_RING_F_EVENT_IDX );
//       features &= ~( 1 << VIRTIO_RING_F_INDIRECT_DESC );

//       virtio_pci_set_driver_features(&virtio_blk_hw, features);

//       // 5. tell device that feature negotiation is complete.
//       status |= VIRTIO_CONFIG_S_FEATURES_OK;
//       virtio_pci_set_status(&virtio_blk_hw, status);

//       // 6. re-read status to ensure FEATURES_OK is set.
//       status = virtio_pci_get_status(&virtio_blk_hw);
//       if ( !( status & VIRTIO_CONFIG_S_FEATURES_OK ) )
//       {
//         panic( "Device did not accept features" );
//       }

// 	  // Initialize queue 0
//       // check maximum queue size.
//       uint32 qsize = virtio_pci_get_queue_size(&virtio_blk_hw, 0);

//       if ( qsize == 0 ) panic( "virtio disk has no queue 0" );
//       if ( qsize < NUM ) panic( "virtio disk max queue too short" );

//       memset( disk.pages, 0, sizeof( disk.pages ) );

//       // Setup the queue
//       virtio_pci_set_queue_size(&virtio_blk_hw, 0, NUM);

//       // desc = pages -- num * VRingDesc
//       // avail = pages + 0x40 -- 2 * uint16, then num * uint16
//       // used = pages + 4096 -- 2 * uint16, then num * vRingUsedElem

//       disk.desc  = (VRingDesc *) disk.pages;
//       disk.avail = (uint16 *) ( ( (char *) disk.desc ) + NUM * sizeof( VRingDesc ) );
//       disk.used  = (UsedArea *) ( disk.pages + PGSIZE );

//       for ( int i = 0; i < NUM; i++ ) disk.free[i] = 1;

//       virtio_pci_set_queue_addr(&virtio_blk_hw, 0, disk.desc, disk.avail, disk.used);

//       virtio_pci_set_queue_enable(&virtio_blk_hw, 0);

//       status |= VIRTIO_CONFIG_S_DRIVER_OK;
//       virtio_pci_set_status(&virtio_blk_hw, status);

//       status = virtio_pci_get_status(&virtio_blk_hw);

// 	    static char _default_dev_name[] = "hd?";
//       for ( ulong i = 0; i < sizeof _default_dev_name; ++i ) _dev_name[i] = _default_dev_name[i];
//       _dev_name[2] = 'a' + (char) _port_id;
//       _dev_name[3] = '\0';
//       for ( auto &pname : _partition_name )
//       {
//         for ( ulong i = 0; i < sizeof _dev_name; ++i ) pname[i] = _dev_name[i];

//         pname[3] = 'p';
//         pname[4] = '?';
//         pname[5] = 0;
//       }
//       dev::k_devm.register_block_device( this, _dev_name );
//     }

//     // find a free descriptor, mark it non-free, return its index.
//     int VirtioDriver::alloc_desc()
//     {
//       for(int i = 0; i < NUM; i++){
//         if(disk.free[i]){
//           disk.free[i] = 0;
//           return i;
//         }
//       }
//       return -1;
//     }

//     // mark a descriptor as free.
//     void VirtioDriver::free_desc(int i)
//     {
//       if(i >= NUM)
//         panic("virtio_disk_intr 1");
//       if(disk.free[i])
//         panic("virtio_disk_intr 2");
//       disk.desc[i].addr = 0;
//       disk.free[i] = 1;
//       proc::k_pm.wakeup(&disk.free[0]);
//     }

//     // free a chain of descriptors.
//     void VirtioDriver::free_chain(int i)
//     {
//       while(1){
//         free_desc(i);
//         if(disk.desc[i].flags & VRING_DESC_F_NEXT)
//           i = disk.desc[i].next;
//         else
//           break;
//       }
//     }

//     int VirtioDriver::alloc3_desc(int *idx)
//     {
//       for(int i = 0; i < 3; i++){
//         idx[i] = alloc_desc();
//         if(idx[i] < 0){
//           for(int j = 0; j < i; j++)
//             free_desc(idx[j]);
//           return -1;
//         }
//       }
//       return 0;
//     }

//     void VirtioDriver::virtio_disk_rw( long start_block, long block_count,
//       dev::BufferDescriptor *buf_list, int buf_count, bool write )
//     {
//       if(buf_count > 1)
//         panic( "buf_count > 1 not implement" );
//       disk.vdisk_lock.acquire();

//       // the spec says that legacy block operations use three
//       // descriptors: one for type/reserved/sector, one for
//       // the data, one for a 1-byte status result.

//       int idx[3];
//       while(1){
//         if(alloc3_desc(idx) == 0) {
//           break;
//         }
//         proc::k_pm.sleep(&disk.free[0], &disk.vdisk_lock);
//       }
      
//       struct virtio_blk_outhdr {
//         uint32 type;
//         uint32 reserved;
//         uint64 sector;
//       } buf0;
      
//       buf0.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
//       buf0.reserved = 0;
//       buf0.sector = start_block;
      
//       // 设置请求头描述符
//       disk.desc[idx[0]].addr = mem::k_pagetable.kwalk_addr((uint64) &buf0);
//       // disk.desc[idx[0]].addr = (uint64)&buf0;
//       // hsai_printf( "%p %p aa\n", &buf0, disk.desc[idx[0]].addr );
//       disk.desc[idx[0]].len	  = sizeof( buf0 );
//       disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
//       // hsai_printf( "%p %p dqqq\n", idx[1], disk.desc[idx[0]].addr );
//       disk.desc[idx[0]].next = idx[1];
      
//       // 设置数据描述符
//       // hsai_printf( "asasa: %d\n", ((uint32 *) (buf_list[0].buf_addr))[0] );
//       disk.desc[idx[1]].addr	   = virt_to_phy_address( buf_list[0].buf_addr );
//       // disk.desc[idx[1]].addr	  = buf_list[0].buf_addr;
//       disk.desc[idx[1]].len		  = _block_size * block_count;
//       disk.desc[idx[1]].flags	 = write ? 0 : VRING_DESC_F_WRITE;
//       disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
//       disk.desc[idx[1]].next = idx[2];
      
//       // 设置状态描述符
//       disk.info[idx[0]].status = 0; // 设置为未完成状态
//       disk.desc[idx[2]].addr   = virt_to_phy_address( (uint64) &disk.info[idx[0]].status );
//       // disk.desc[idx[2]].addr   = (uint64)&disk.info[idx[0]].status;
//       disk.desc[idx[2]].len	   = 1;
//       disk.desc[idx[2]].flags = VRING_DESC_F_WRITE;
//       disk.desc[idx[2]].next = 0;
      
//       // 记录请求信息
//       disk.info[idx[0]].b = buf_list;
//       // hsai_printf( "buf_list:%p\n", buf_list );
//       disk.info[idx[0]].wait = true;

// 	    // 通知设备
//       disk.avail[2 + (disk.avail[1] % NUM)] = idx[0];
//       __sync_synchronize();
//       disk.avail[1]++;
      
//       // PCI方式通知队列
//       virtio_pci_set_queue_notify(&virtio_blk_hw, 0);

// 	    // 等待操作完成
//       // while(disk.info[idx[0]].wait) {
//       //   if(dev::get_cur_proc())
//       //     dev::sleep_at(buf_list, disk.vdisk_lock);
//       // }
//       // hsai_printf("dafafa:%d\n",virtio_pci_clear_isr(&virtio_blk_hw));
//       // volatile uint16 *pt_used_idx = &disk.used_idx;
//       // volatile uint16 *pt_idx = &disk.used->id;
//       // //     wait cmd done
//       // while ( *pt_used_idx == *pt_idx );
// 	    while ( !virtio_pci_clear_isr( &virtio_blk_hw ) );
// 	    int id = disk.used->elems[disk.used_idx].id;

// 	  if ( disk.info[id].status != 0 ) panic( "virtio_disk_intr status" );
//       disk.used_idx		   = ( disk.used_idx + 1 ) % NUM;
//       disk.info[idx[0]].wait = false;
//     // 等待操作完成
//       // while(disk.info[idx[0]].wait) {
//       //   if(dev::get_cur_proc())
//       //     dev::sleep_at(buf_list, disk.vdisk_lock);
//       // }

//       disk.info[idx[0]].b = 0; // disk is done with buf

//       free_chain( idx[0] );
//       disk.vdisk_lock.release();
//     }

//     int	VirtioDriver::read_blocks_sync( long start_block, long block_count, dev::BufferDescriptor *buf_list,
//         int buf_count )
//     {
//       virtio_disk_rw(start_block, block_count, buf_list, buf_count, 0);
//       return 0;
//     }
//     int	VirtioDriver::read_blocks( long start_block, long block_count,
//       dev::BufferDescriptor *buf_list, int buf_count )
//     {
//       panic( "not implement" );
//       while ( 1 );
//     }
//     int	VirtioDriver::write_blocks_sync( long start_block, long block_count,
//       dev::BufferDescriptor *buf_list, int buf_count ) 
//     {
//       virtio_disk_rw(start_block, block_count, buf_list, buf_count, 1);
//       return 0;
//     }
//     int	VirtioDriver::write_blocks( long start_block, long block_count,
//       dev::BufferDescriptor *buf_list, int buf_count )
//     {
//       panic( "not implement" );
//       while ( 1 );
//     }

//     int VirtioDriver::handle_intr()
//     {
//       disk.vdisk_lock.acquire();

//       // Read ISR to acknowledge the interrupt
//       virtio_pci_clear_isr(&virtio_blk_hw);
//       __sync_synchronize();

//       while((disk.used_idx % NUM) != (disk.used->id % NUM)){
//         int id = disk.used->elems[disk.used_idx].id;

//         if(disk.info[id].status != 0)
//             panic("virtio_disk_intr status");

//         disk.info[id].wait = false;   // disk is done with buf
//         proc::k_pm.wakeup(disk.info[id].b);

//         disk.used_idx = (disk.used_idx + 1) % NUM;
//       }

//       disk.vdisk_lock.release();
//       return 0;
//     }
// 	} // namespace qemu

// } // namespace loongarch