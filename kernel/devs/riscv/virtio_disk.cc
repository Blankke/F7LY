//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
// qemu presents a "legacy" virtio interface.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//


#include "virtio.hh"
#include <device_manager.hh>
#include "printer.hh"
#include "klib.hh"
#include "proc_manager.hh"
#include "pagetable.hh"
#include "platform.hh"
#include "pte.hh"
#include "cpu.hh"
namespace riscv
{
	namespace qemu
	{
  VirtioDriver::VirtioDriver( void *base_addr, int port_id)
    {

      uint32 status = 0;
      _port_id = port_id;
      virtio_addr = (uint64) base_addr;

      disk.vdisk_lock.init("virtio_disk");

      if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(VIRTIO_MMIO_VERSION) != 1 ||
        *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
        *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
          panic("could not find virtio disk");
      }
      
      status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
      *R(VIRTIO_MMIO_STATUS) = status;

      status |= VIRTIO_CONFIG_S_DRIVER;
      *R(VIRTIO_MMIO_STATUS) = status;

      // negotiate features
      uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
      features &= ~(1 << VIRTIO_BLK_F_RO);
      features &= ~(1 << VIRTIO_BLK_F_SCSI);
      features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
      features &= ~(1 << VIRTIO_BLK_F_MQ);
      features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
      features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
      features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
      *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

      // tell device that feature negotiation is complete.
      status |= VIRTIO_CONFIG_S_FEATURES_OK;
      *R(VIRTIO_MMIO_STATUS) = status;

      // tell device we're completely ready.
      status |= VIRTIO_CONFIG_S_DRIVER_OK;
      *R(VIRTIO_MMIO_STATUS) = status;

      *R(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;

      // initialize queue 0.
      *R(VIRTIO_MMIO_QUEUE_SEL) = 0;
      uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
      if(max == 0)
        panic("virtio disk has no queue 0");
      if(max < NUM)
        panic("virtio disk max queue too short");
      *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;
      memset(disk.pages, 0, sizeof(disk.pages));
      *R(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk.pages) >> 12;

      // desc = pages -- num * VRingDesc
      // avail = pages + 0x40 -- 2 * uint16, then num * uint16
      // used = pages + 4096 -- 2 * uint16, then num * vRingUsedElem

      disk.desc = (VRingDesc *) disk.pages;
      disk.avail = (uint16 *) ( ( (char *) disk.desc ) + NUM * sizeof( VRingDesc ) );
      disk.used	 = (UsedArea *) ( disk.pages + PGSIZE );

      for(int i = 0; i < NUM; i++)
        disk.free[i] = 1;

      static char _default_dev_name[] = "hd?";
      for ( ulong i = 0; i < sizeof _default_dev_name; ++i ) _dev_name[i] = _default_dev_name[i];
      _dev_name[2] = 'a' + (char) _port_id;
      _dev_name[3] = '\0';
      for ( auto &pname : _partition_name )
      {
        for ( ulong i = 0; i < sizeof _dev_name; ++i ) pname[i] = _dev_name[i];

        pname[3] = 'p';
        pname[4] = '?';
        pname[5] = 0;
      }
      dev::k_devm.register_block_device( this, _dev_name );
    }

    // find a free descriptor, mark it non-free, return its index.
    int VirtioDriver::alloc_desc()
    {
      for(int i = 0; i < NUM; i++){
        if(disk.free[i]){
          disk.free[i] = 0;
          return i;
        }
      }
      return -1;
    }

    // mark a descriptor as free.
    void VirtioDriver::free_desc(int i)
    {
      if(i >= NUM)
        panic("virtio_disk_intr 1");
      if(disk.free[i])
        panic("virtio_disk_intr 2");
      disk.desc[i].addr = 0;
      disk.free[i] = 1;
      proc::k_pm.wakeup(&disk.free[0]);
    }

    // free a chain of descriptors.
    void VirtioDriver::free_chain(int i)
    {
      while(1){
        free_desc(i);
        if(disk.desc[i].flags & VRING_DESC_F_NEXT)
          i = disk.desc[i].next;
        else
          break;
      }
    }

    int VirtioDriver::alloc3_desc(int *idx)
    {
      for(int i = 0; i < 3; i++){
        idx[i] = alloc_desc();
        if(idx[i] < 0){
          for(int j = 0; j < i; j++)
            free_desc(idx[j]);
          return -1;
        }
      }
      return 0;
    }

    void VirtioDriver::virtio_disk_rw( long start_block, long block_count,
      dev::BufferDescriptor *buf_list, int buf_count, bool write )
    {
      if(buf_count > 1)
        panic( "buf_count > 1 not implement" );
      disk.vdisk_lock.acquire();

      // the spec says that legacy block operations use three
      // descriptors: one for type/reserved/sector, one for
      // the data, one for a 1-byte status result.

      int idx[3];
      while(1){
        if(alloc3_desc(idx) == 0) {
          break;
        }
        proc::k_pm.sleep(&disk.free[0], &disk.vdisk_lock);
      }
      
      struct virtio_blk_outhdr {
        uint32 type;
        uint32 reserved;
        uint64 sector;
      } buf0;
      
      buf0.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
      buf0.reserved = 0;
      buf0.sector = start_block;
      
      // 设置请求头描述符
      disk.desc[idx[0]].addr = PTE2PA(mem::k_pagetable.kwalkaddr((uint64) &buf0).get_data());
      disk.desc[idx[0]].len = sizeof(buf0);
      disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
      disk.desc[idx[0]].next = idx[1];
      
      // 设置数据描述符
      disk.desc[idx[1]].addr = buf_list[0].buf_addr;
      disk.desc[idx[1]].len	 = _block_size * block_count;
      disk.desc[idx[1]].flags	 = write ? 0 : VRING_DESC_F_WRITE;
      disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
      disk.desc[idx[1]].next = idx[2];
      
      // 设置状态描述符
      disk.info[idx[0]].status = 0; // 设置为未完成状态
      disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
      disk.desc[idx[2]].len = 1;
      disk.desc[idx[2]].flags = VRING_DESC_F_WRITE;
      disk.desc[idx[2]].next = 0;
      
      // 记录请求信息
      disk.info[idx[0]].b = buf_list;
      disk.info[idx[0]].wait = true;

      // 通知设备
      disk.avail[2 + (disk.avail[1] % NUM)] = idx[0];
      __sync_synchronize();
      disk.avail[1]++;
      
      *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
      // 等待操作完成
      while(disk.info[idx[0]].wait) {
        if (Cpu::get_cpu()->get_cur_proc())
          proc::k_pm.sleep(buf_list, &disk.vdisk_lock);
        else 
        {
          disk.vdisk_lock.release();
          asm("wfi");
          disk.vdisk_lock.acquire();
        }
      }
      disk.info[idx[0]].b = 0;   // disk is done with buf

      free_chain(idx[0]);
      disk.vdisk_lock.release();
    }

    int	VirtioDriver::read_blocks_sync( long start_block, long block_count, dev::BufferDescriptor *buf_list,
        int buf_count )
    {
      virtio_disk_rw(start_block, block_count, buf_list, buf_count, 0);
      return 0;
    }
    int	VirtioDriver::read_blocks( long start_block, long block_count,
      dev::BufferDescriptor *buf_list, int buf_count )
    {
      panic( "not implement" );
      while ( 1 );
    }
    int	VirtioDriver::write_blocks_sync( long start_block, long block_count,
      dev::BufferDescriptor *buf_list, int buf_count ) 
    {
      virtio_disk_rw(start_block, block_count, buf_list, buf_count, 1);
      return 0;
    }
    int	VirtioDriver::write_blocks( long start_block, long block_count,
      dev::BufferDescriptor *buf_list, int buf_count )
    {
      panic( "not implement" );
      while ( 1 );
    }

    int VirtioDriver::handle_intr()
    {
      disk.vdisk_lock.acquire();

      while((disk.used_idx % NUM) != (disk.used->id % NUM)){
        int id = disk.used->elems[disk.used_idx].id;

        if(disk.info[id].status != 0)
          panic("virtio_disk_intr status");

        disk.info[id].wait = false;   // disk is done with buf
        proc::k_pm.wakeup(disk.info[id].b);

        disk.used_idx = (disk.used_idx + 1) % NUM;
      }
      *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

      disk.vdisk_lock.release();
      return 0;
    }
    #undef R
	} // namespace qemu

} // namespace riscv