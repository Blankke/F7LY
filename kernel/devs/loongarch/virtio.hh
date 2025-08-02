// #pragma once

// #include <block_device.hh>
// #include <disk_partition_device.hh>
// #include "spinlock.hh"
// #include "platform.hh"
// #include <types.hh>
// #include "printer.hh"
// #include "pci.hh"
// //
// // virtio device definitions.
// // for both the mmio interface, and virtio descriptors.
// // only tested with qemu.
// // this is the "legacy" virtio interface.
// //
// // the virtio spec:
// // https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
// //
// namespace loongarch
// {
// 	namespace qemu
// 	{
// 		class VirtioDriver : public dev::BlockDevice
// 		{
// 			friend class DiskDriver;
//       // // virtio PCI common configuration
//       // #define VIRTIO_PCI_HOST_FEATURES        0  // host/device features
//       // #define VIRTIO_PCI_GUEST_FEATURES       4  // guest features
//       // #define VIRTIO_PCI_QUEUE_PFN            8  // physical page number for queue
//       // #define VIRTIO_PCI_QUEUE_NUM           12  // number of ring entries
//       // #define VIRTIO_PCI_QUEUE_SEL           14  // queue select
//       // #define VIRTIO_PCI_QUEUE_NOTIFY        16  // queue notify
//       // #define VIRTIO_PCI_STATUS              18  // device status register
//       // #define VIRTIO_PCI_ISR                 19  // interrupt status register
//       // #define VIRTIO_PCI_CONFIG              20  // configuration vector
//       // #define VIRTIO_PCI_QUEUE_NUM_MAX       0x20

//       // // status register bits, from qemu virtio_config.h
//       // #define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
//       // #define VIRTIO_CONFIG_S_DRIVER		2
//       // #define VIRTIO_CONFIG_S_DRIVER_OK	4
//       // #define VIRTIO_CONFIG_S_FEATURES_OK 8

//       // // device feature bits
//       // #define VIRTIO_BLK_F_RO				5  /* Disk is read-only */
//       // #define VIRTIO_BLK_F_SCSI			7  /* Supports scsi command passthru */
//       // #define VIRTIO_BLK_F_CONFIG_WCE		11 /* Writeback mode available in config */
//       // #define VIRTIO_BLK_F_MQ				12 /* support more than one vq */
//       // #define VIRTIO_F_ANY_LAYOUT			27
//       // #define VIRTIO_RING_F_INDIRECT_DESC 28
//       // #define VIRTIO_RING_F_EVENT_IDX		29

//       // this many virtio descriptors.
//       // must be a power of two.
//       #define NUM 8

// 			struct VRingDesc
// 			{
// 				uint64 addr;
// 				uint32 len;
// 				uint16 flags;
// 				uint16 next;
// 			};
//       #define VRING_DESC_F_NEXT  1 // chained with another descriptor
//       #define VRING_DESC_F_WRITE 2 // device writes (vs read)

//       struct VRingUsedElem {
//         uint32 id;   // index of start of completed descriptor chain
//         uint32 len;
//       };

//       // for disk ops
//       #define VIRTIO_BLK_T_IN  0 // read the disk
//       #define VIRTIO_BLK_T_OUT 1 // write the disk

//       struct UsedArea {
//         uint16 flags;
//         uint16 id;
//         struct VRingUsedElem elems[NUM];
//       };

//       // the address of virtio mmio register r.
//       volatile uint64 _pci_dev;
//       static constexpr int _block_size = 512;

//       private:
//         char _dev_name[8];
//         char _partition_name[4][8];
//         dev::DiskPartitionDevice _disk_partition[4]; // MBR 硬盘只支持最多4个分区
//         virtio_pci_hw_t			  virtio_blk_hw;

//         int		   _port_id = 0;
//         struct Disk {
//           // memory for virtio descriptors &c for queue 0.
//           // this is a global instead of allocated because it must
//           // be multiple contiguous pages, which kalloc()
//           // doesn't support, and page aligned.
//           char pages[2*PGSIZE];
//           struct VRingDesc *desc;
//           uint16 *avail;
//           struct UsedArea *used;
           
//           // our own book-keeping.
//           char free[NUM];  // is a descriptor free?
//           uint16 used_idx; // we've looked this far in used[2..NUM].
         
//           // track info about in-flight operations,
//           // for use when completion interrupt arrives.
//           // indexed by first descriptor index of chain.
//           struct {
//             dev::BufferDescriptor *b;
//             char status;
//             bool wait;
//           } info[NUM];
             
//           SpinLock vdisk_lock;
             
//         } __attribute__ ((aligned (PGSIZE))) disk;

//         void virtio_disk_rw( long start_block, long block_count,
//         dev::BufferDescriptor *buf_list, int buf_count, bool write );
//         void free_desc( int i );
//         void free_chain( int i );
//         int	alloc3_desc( int *idx );
//         int	alloc_desc();

//         // PCI中断相关标志
//         static constexpr uint8 VIRTIO_PCI_ISR_INTR = 0x1;    // 数据中断
//         static constexpr uint8 VIRTIO_PCI_ISR_CONFIG = 0x2;  // 配置改变中断

//       public:
//         virtual long get_block_size() override { return (long) _block_size; }
//         virtual int  read_blocks_sync( long start_block, long block_count,
//                       dev::BufferDescriptor *buf_list, int buf_count ) override;
//         virtual int read_blocks( long start_block, long block_count, dev::BufferDescriptor *buf_list,
//                     int buf_count ) override;
//         virtual int write_blocks_sync( long start_block, long block_count,
//                       dev::BufferDescriptor *buf_list, int buf_count ) override;
//         virtual int write_blocks( long start_block, long block_count,
//                     dev::BufferDescriptor *buf_list, int buf_count ) override;
//         virtual int handle_intr() override;

// 		    virtual bool read_ready() override {
//           // 检查设备状态
//           // uint32 status = *R(VIRTIO_MMIO_STATUS);
//           // if ((status & VIRTIO_CONFIG_S_DRIVER_OK) == 0) {
//           //   return false;  // 设备未就绪
//           // }
//           return true;
//         }
//         virtual bool write_ready() override {
//           // 检查设备状态
//           // uint32 status = *R(VIRTIO_MMIO_STATUS);
//           // if ((status & VIRTIO_CONFIG_S_DRIVER_OK) == 0) {
//           //   return false;  // 设备未就绪
//           // }

//           // // 检查设备是否只读
//           // uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
//           // if (features & (1 << VIRTIO_BLK_F_RO)) {
//           //   return false;  // 设备只读
//           // }
//           return true;
//         }

// 	    public:
//         VirtioDriver() = default;
// 		    VirtioDriver(pci_device device, int port_id);
// 		};

//     // PCI配置空间寄存器偏移
//     #define PCI_VENDOR_ID           0x00    // 厂商ID寄存器
//     #define PCI_DEVICE_ID          0x02    // 设备ID寄存器
//     #define PCI_COMMAND            0x04    // 命令寄存器
//     #define PCI_STATUS             0x06    // 状态寄存器
//     #define PCI_REVISION_ID        0x08    // 版本ID寄存器
//     #define PCI_CLASS_PROG         0x09    // 编程接口寄存器
//     #define PCI_CLASS_DEVICE       0x0a    // 设备类寄存器
//     #define PCI_HEADER_TYPE        0x0e    // 头类型寄存器
//     #define PCI_INTERRUPT_LINE     0x3c    // 中断线寄存器
//     #define PCI_INTERRUPT_PIN      0x3d    // 中断引脚寄存器

//     // PCI命令寄存器位定义
//     #define PCI_COMMAND_IO         0x1     // I/O空间访问使能
//     #define PCI_COMMAND_MEMORY    0x2     // 内存空间访问使能
//     #define PCI_COMMAND_MASTER    0x4     // 总线主控使能
//     #define PCI_COMMAND_SPECIAL   0x8     // 特殊周期使能
//     #define PCI_COMMAND_INVALIDATE 0x10    // 内存写与使能位
//     #define PCI_COMMAND_VGA_PALETTE 0x20   // VGA调色板窥探
//     #define PCI_COMMAND_PARITY    0x40    // 奇偶校验错误应答使能
//     #define PCI_COMMAND_WAIT      0x80    // SERR#使能
//     #define PCI_COMMAND_SERR      0x100   // 快速返回使能
//     #define PCI_CAP_ID_MSI            0x05  // MSI能力ID
//     #define MSIX_CONTROL                  0x02
//     #define MSIX_TABLE_PTR                0x04
//     #define MSIX_PBA_PTR                  0x08
//     #define MSIX_CONTROL_ENABLE           0x8000
//     #define MSIX_CONTROL_MASKALL          0x4000

// 	} // namespace qemu

// } // namespace loongarch