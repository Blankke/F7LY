// #include "pci.hh"
// #include "printer.hh"

// namespace loongarch
// {
// 	namespace qemu
// 	{
//         // PCI配置空间访问的底层实现
//         static void pci_config_write(uint64 pdev, int reg, uint64 value, int size) {
//             uint64 cfg_addr = pdev + (reg & 0xFC);
//             switch(size) {
//                 case 1:
//                     *(volatile u8*)cfg_addr = value;
//                     break;
//                 case 2:
//                     *(volatile uint16*)cfg_addr = value;
//                     break;
//                 case 4:
//                     *(volatile uint32*)cfg_addr = value;
//                     break;
//                 case 8:
//                     *(volatile uint64*)cfg_addr = value;
//                     break;
//             }
//         }

//         static uint64 pci_config_read(uint64 pdev, int reg, int size) {
//             uint64 cfg_addr = pdev + (reg & 0xFC);
//             switch(size) {
//                 case 1:
//                     return *(volatile u8*)cfg_addr;
//                 case 2:
//                     return *(volatile uint16*)cfg_addr;
//                 case 4:
//                     return *(volatile uint32*)cfg_addr;
//                 case 8:
//                     return *(volatile uint64*)cfg_addr;
//                 default:
//                     return 0;
//             }
//         }

//         void pci_config_read(void *buf, uint64 len, uint64 offset)
//         {
//             volatile uint8 *dst = (volatile uint8 *)buf;
//             while (len) {
//                 *dst = PCI_REG8(offset);
//                 --len;
//                 ++dst;
//                 ++offset;
//             }
//         }

//         // API函数实现
//         void API_PciDevConfigWriteByte(uint64 pdev, int reg, u8 value) {
//             pci_config_write(pdev, reg, value, 1);
//         }

//         void API_PciDevConfigWriteWord(uint64 pdev, int reg, uint16 value) {
//             pci_config_write(pdev, reg, value, 2);
//         }

//         void API_PciDevConfigWriteDword(uint64 pdev, int reg, uint32 value) {
//             pci_config_write(pdev, reg, value, 4);
//         }

//         void API_PciDevConfigWriteQword(uint64 pdev, int reg, uint64 value) {
//             pci_config_write(pdev, reg, value, 8);
//         }

//         uint8 API_PciDevConfigReadByte(uint64 pdev, int reg) {
//             return pci_config_read(pdev, reg, 1);
//         }

//         uint16 API_PciDevConfigReadWord(uint64 pdev, int reg) {
//             return pci_config_read(pdev, reg, 2);
//         }

//         uint32 API_PciDevConfigReadDword(uint64 pdev, int reg) {
//             return pci_config_read(pdev, reg, 4);
//         }

//         uint64 API_PciDevConfigReadQword(uint64 pdev, int reg) {
//             return pci_config_read(pdev, reg, 8);
//         }

//         static void *get_cfg_addr(uint64 pci_base, struct virtio_pci_cap *cap)
//         {
//             // 对于 64bit来说是 BAR4 & BAR5
//             return (void *)((API_PciDevConfigReadQword(pci_base, PCI_ADDR_BAR0 + 4 * cap->bar)
//                  & 0xFFFFFFFFFFFFFFF0) + cap->offset + PCIE0_MMIO_V);
//         }

// 		pci_device pci_device_probe(uint16 vendor_id, uint16 device_id)
//         {
//             uint32 pci_id = (((uint32)device_id) << 16) | vendor_id;

// 			for (int bus = 0; bus < 255; bus++) {
//                 for (int dev = 0; dev < 32; dev++) {
// 					volatile uint32 *base = (volatile uint32 *)pci_config_address( bus, dev, 0 );
// 					uint32			 id	  = base[0]; // device_id + vendor_id

// 					if (id != pci_id) continue;

//                     // command and status register.
//                     // bit 0 : I/O access enable
//                     // bit 1 : memory access enable
//                     // bit 2 : enable mastering
//                     base[1] = 7;
//                     __sync_synchronize();
// 					pci_device device;
// 					device.bus = bus;
//                     device.device = dev;
//                     device.function = 0;
// 					return device;
// 				}
//             }
//             pci_device device;
// 			return device;
// 		}

//         #define PAGE_ALIGN(sz)	((((uint64)sz) + 0x0fff) & ~0x0fff)
//         uint64 pci_alloc_mmio(uint64 sz)
//         {
//             static uint64 s_off = 0;           // static 变量, 共用一个
//             // MMIO 区域 + s_off
//             // uint64 addr = PHYSTOP + s_off + 2 * PGSIZE;
//             uint64 addr = PCIE0_MMIO + s_off;
//             s_off += PAGE_ALIGN(sz);        // 按页 4k 对齐
//             // printf("addr: 0x%016llx, 0x%016llx\n", addr, s_off);
//             return addr;
//         }

//        int virtio_pci_read_caps(virtio_pci_hw_t* virtio_blk_hw, uint64 pci_base)
//         {
//             struct virtio_pci_cap cap;
//             uint64 pos = 0;
//             //struct virtio_pci_hw *hw = &g_hw;

//             pos = API_PciDevConfigReadByte(pci_base, PCI_ADDR_CAP);         // 第一个 cap's offset in ECAM
//             // printf("cap: 0x%016llx\n", pci_base + PCI_ADDR_CAP);     // cap pointer 在 ECAM 整个区域中的 offset

//             // 遍历所有的 capability
//             while (pos) {
//                 pos += pci_base;
//                 // hsai_printf("cap: 0x%016llx\n", pos); 
//                 pci_config_read(&cap, sizeof(cap), pos);

//                 if (cap.cap_vndr != PCI_CAP_ID_VNDR) {         // PCI_CAP_ID_VNDR vendor-specific Cap
//                     goto next;
//                 }
//                 // hsai_printf("cap.offset: %p\n", cap.offset); 

//                 switch (cap.cfg_type) {
//                 case VIRTIO_PCI_CAP_COMMON_CFG:
//                     virtio_blk_hw->common_cfg = get_cfg_addr(pci_base, &cap);
//                     // hsai_printf("common_cfg addr: %016llx\n", (uint64)virtio_blk_hw->common_cfg);
//                     break;
//                 case VIRTIO_PCI_CAP_NOTIFY_CFG:
//                     pci_config_read(&virtio_blk_hw->notify_off_multiplier,
//                             4, pos + sizeof(cap));
//                     virtio_blk_hw->notify_cfg = get_cfg_addr(pci_base, &cap);
//                     // hsai_printf("notify_cfg addr: %016llx\n", (uint64)virtio_blk_hw->notify_cfg);
//                     break;
//                 case VIRTIO_PCI_CAP_DEVICE_CFG:
//                     virtio_blk_hw->device_cfg = get_cfg_addr(pci_base, &cap);
//                     // hsai_printf("device_cfg addr: %016llx\n", (uint64)virtio_blk_hw->device_cfg);
//                     break;
//                 case VIRTIO_PCI_CAP_ISR_CFG:
//                     virtio_blk_hw->isr_cfg = get_cfg_addr(pci_base, &cap);
//                     // hsai_printf("isr_cfg addr: %016llx\n", (uint64)virtio_blk_hw->isr_cfg);
//                     break;
//                 }
//         next:
//                 pos = cap.cap_next;
//             }
//             if (virtio_blk_hw->common_cfg == nullptr || virtio_blk_hw->notify_cfg == nullptr ||
//                 virtio_blk_hw->device_cfg == nullptr || virtio_blk_hw->isr_cfg == nullptr) {
//                 panic("no modern virtio pci device found.\n");
//                 return -1;
//             }
//             return 0;
//         }
//         void virtio_pci_set_status(virtio_pci_hw_t *hw, uint8 status)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;
//             PCI_REG8(&cfg->device_status) = status;
//         }

//         uint8 virtio_pci_get_status(virtio_pci_hw_t *hw)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;
//             return PCI_REG8(&cfg->device_status);
//         }

//         uint64 virtio_pci_get_device_features(virtio_pci_hw_t *hw)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

//             PCI_REG32(&cfg->device_feature_select) = 0;
//             uint64 f1 = PCI_REG32(&cfg->device_feature);

//             PCI_REG32(&cfg->device_feature_select) = 1;
//             uint64 f2 = PCI_REG32(&cfg->device_feature);

//             return (f2 << 32) | f1;
//         }

//         void virtio_pci_set_driver_features(virtio_pci_hw_t *hw, uint64 features)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;
//             PCI_REG32(&cfg->driver_feature_select) = 0;
// 			__sync_synchronize();
// 			PCI_REG32( &cfg->driver_feature ) = features & 0xFFFFFFFF;

// 			PCI_REG32(&cfg->driver_feature_select) = 1;
// 			__sync_synchronize();
// 			PCI_REG32( &cfg->driver_feature ) = features >> 32;
// 		}

//         uint16 virtio_pci_get_queue_size(virtio_pci_hw_t *hw, int qid)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

//             PCI_REG16(&cfg->queue_select) = qid;
// 			__sync_synchronize();

// 			return PCI_REG16(&cfg->queue_size);
//         }

//         void virtio_pci_set_queue_size(virtio_pci_hw_t *hw, int qid, int qsize)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

//             PCI_REG16(&cfg->queue_select) = qid;
//             __sync_synchronize();

//             PCI_REG16(&cfg->queue_size) = qsize;
//         }

//         void virtio_pci_set_queue_addr(virtio_pci_hw_t *hw, int qid, void *desc, void *avail, void *used)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

//             PCI_REG16(&cfg->queue_select) = qid;
//             __sync_synchronize();

// 			PCI_REG64( &cfg->queue_desc )	= virt_to_phy_address( (uint64) desc );
// 			PCI_REG64( &cfg->queue_driver ) = virt_to_phy_address( (uint64) avail );
// 			PCI_REG64( &cfg->queue_device ) = virt_to_phy_address( (uint64) used );
// 		}

// 		void virtio_pci_set_queue_enable(virtio_pci_hw_t *hw, int qid)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

//             PCI_REG16(&cfg->queue_select) = qid;
//             __sync_synchronize();

//             PCI_REG16(&cfg->queue_enable) = 1;
//         }

//         void *virtio_pci_get_queue_notify_addr(virtio_pci_hw_t *hw, int qid)
//         {
//             virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

//             // 对应的 virtqueue
//             PCI_REG16(&cfg->queue_select) = qid;
//             __sync_synchronize();

//             // 获得地址
//             uint16 notify_off = PCI_REG16(&cfg->queue_notify_off);
//             return (void *)((uint64)hw->notify_cfg + notify_off * hw->notify_off_multiplier);
//         }

//         void virtio_pci_set_queue_notify(virtio_pci_hw_t *hw, int qid)
//         {
//             // notify addr, 计算 notify 地址, 然后通知
//             void *pt = virtio_pci_get_queue_notify_addr(hw, qid);
//             PCI_REG32(pt) = 1;
//         }

//         uint32 virtio_pci_clear_isr(virtio_pci_hw_t *hw)
//         {
//             uint32 irq = PCI_REG32(hw->isr_cfg);
//             return irq;
//         }
// 	} // namespace qemu

// } // namespace loongarch
