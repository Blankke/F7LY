// //
// // Created by Li Shuang ( pseudonym ) on 2024-07-16
// // --------------------------------------------------------------
// // | Note: This code file just for study, not for commercial use
// // | Contact Author: lishuang.mk@whu.edu.cn
// // --------------------------------------------------------------
// //

// #pragma once
// #include "platform.hh"
// #include <types.hh>

// namespace loongarch
// {
// 	namespace qemu
// 	{
//         // PCI设备结构体
//         struct pci_device {
//             uint8 bus;      // 总线号
//             uint8 device;   // 设备号
//             uint8 function; // 功能号
//             // uint16_t vendor_id;  // 厂商ID
//             // uint16_t device_id;  // 设备ID
//         };  // PCI设备句柄;

//         struct pci_msix {
//             int bar_num;            // bar number
//             int irq_num;            // interrupt number
//             uint32 tbl_addr;           // tbl address
//             uint32 pba_addr;           // pba address
//         };

//         struct virtio_pci_cap {
//             uint8 cap_vndr; /* Generic PCI field: PCI_CAP_ID_VNDR */
//             uint8 cap_next; /* Generic PCI field: next ptr. */
//             uint8 cap_len; /* Generic PCI field: capability length */
//             uint8 cfg_type; /* Identifies the structure. */
//             uint8 bar; /* Where to find it. */
//             uint8 id; /* Multiple capabilities of the same type */
//             uint8 padding[2]; /* Pad to full dword. */
//             uint32 offset; /* Offset within bar. */
//             uint32 length; /* Length of the structure, in bytes. */
//         };

//         struct virtio_pci_hw {
//             uint8      bar;
//             uint8	    use_msix;
//             //uint8      modern;
//             uint32     notify_off_multiplier;
//             void    *common_cfg;
//             void    *isr_cfg;
//             void    *device_cfg;
//             void    *notify_cfg;
//             pci_msix msix;
//         };
// 		using virtio_pci_hw_t = virtio_pci_hw;

//         struct virtio_pci_common_cfg {
//             /* About the whole device. */
//             uint32 device_feature_select; /* read-write */
//             uint32 device_feature; /* read-only for driver */
//             uint32 driver_feature_select; /* read-write */
//             uint32 driver_feature; /* read-write */
//             uint16 config_msix_vector; /* read-write */
//             uint16 num_queues; /* read-only for driver */
//             uint8 device_status; /* read-write */
//             uint8 config_generation; /* read-only for driver */

//             /* About a specific virtqueue. */
//             uint16 queue_select; /* read-write */
//             uint16 queue_size; /* read-write */
//             uint16 queue_msix_vector; /* read-write */
//             uint16 queue_enable; /* read-write */
//             uint16 queue_notify_off; /* read-only for driver */
//             uint64 queue_desc; /* read-write */
//             uint64 queue_driver; /* read-write avail ring */
//             uint64 queue_device; /* read-write used ring */
//             uint16 queue_notify_data; /* read-only for driver */
//             uint16 queue_reset;       /* read-write */
//         };

// 		// constexpr ulong PCIE0_ECAM				   = ( 0x20000000 | DMWIN1_MASK);
//         // constexpr ulong PCIE0_ECAM_V			   =  0x20000000;
// 		// constexpr ulong PCIE0_MMIO				   = 0x40000000;
// 		// constexpr ulong PCIE0_MMIO_V			   = ( 0x40000000 | DMWIN1_MASK);
// 		constexpr ulong pci_bus_num_shift		   = 16;
// 		constexpr ulong pci_dev_num_shift		   = 11;
// 		constexpr ulong pci_fun_num_shift		   = 8;
//         #define PCI_ADDR_BAR0    		0x10
//         #define PCI_ADDR_CAP    		0x34

//         #define PCI_CAP_ID_VNDR		    0x09
//         #define PCI_CAP_ID_MSIX		    0x11
//         /* Common configuration */
//         #define VIRTIO_PCI_CAP_COMMON_CFG 1
//         /* Notifications */
//         #define VIRTIO_PCI_CAP_NOTIFY_CFG 2
//         /* ISR Status */
//         #define VIRTIO_PCI_CAP_ISR_CFG 3
//         /* Device specific configuration */
//         #define VIRTIO_PCI_CAP_DEVICE_CFG 4
//         /* PCI configuration access */
//         #define VIRTIO_PCI_CAP_PCI_CFG 5
//         /* Shared memory region */
//         #define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8
//         /* Vendor-specific data */
//         #define VIRTIO_PCI_CAP_VENDOR_CFG 9

//         uint64 pci_alloc_mmio(uint64 sz);
// 		pci_device pci_device_probe( uint16 vendor_id, uint16 device_id );
//         int virtio_pci_read_caps(virtio_pci_hw_t* virtio_blk_hw, uint64 pci_base);
//         constexpr ulong pci_config_address( uint bus_num, uint dev_num, uint fun_num )
// 		{
// 			return PCIE0_ECAM | ( (ulong) bus_num << pci_bus_num_shift ) |
// 				   ( (ulong) dev_num << pci_dev_num_shift ) |
// 				   ( (ulong) fun_num << pci_fun_num_shift );
// 		}

//         // API函数实现
// 		void API_PciDevConfigWriteByte( uint64 pdev, int reg, u8 value );
// 		void  API_PciDevConfigWriteWord( uint64 pdev, int reg, uint16 value );
// 		void  API_PciDevConfigWriteDword( uint64 pdev, int reg, uint32 value );
// 		void  API_PciDevConfigWriteQword( uint64 pdev, int reg, uint64 value );
// 		uint8 API_PciDevConfigReadByte( uint64 pdev, int reg );
// 		uint16 API_PciDevConfigReadWord( uint64 pdev, int reg );
// 		uint32 API_PciDevConfigReadDword( uint64 pdev, int reg );
// 		uint64 API_PciDevConfigReadQword( uint64 pdev, int reg );

// 		void virtio_pci_set_status(virtio_pci_hw_t *hw, uint8 status);
//         uint8 virtio_pci_get_status(virtio_pci_hw_t *hw);
//         uint64 virtio_pci_get_device_features(virtio_pci_hw_t *hw);
//         void virtio_pci_set_driver_features(virtio_pci_hw_t *hw, uint64 features);
//         uint16 virtio_pci_get_queue_size(virtio_pci_hw_t *hw, int qid);
//         void virtio_pci_set_queue_size(virtio_pci_hw_t *hw, int qid, int qsize);
//         void virtio_pci_set_queue_addr(virtio_pci_hw_t *hw, int qid, void *desc, void *avail, void *used);
//         void virtio_pci_set_queue_enable(virtio_pci_hw_t *hw, int qid);
//         void *virtio_pci_get_queue_notify_addr(virtio_pci_hw_t *hw, int qid);
//         void virtio_pci_set_queue_notify(virtio_pci_hw_t *hw, int qid);
//         uint32 virtio_pci_clear_isr(virtio_pci_hw_t *hw);

//         // #define PCI_REG8(reg)   (*(volatile uint8 *)(reg))
//         // #define PCI_REG16(reg)  (*(volatile uint16 *)(reg))
//         // #define PCI_REG32(reg)  (*(volatile uint32 *)(reg))
//         // #define PCI_REG64(reg)  (*(volatile uint64 *)(reg))

// 	} // namespace qemu

// } // namespace loongarch
