#include "platform.hh"
#include "printer.hh"
#include "trap/loongarch/pci.h"
#include "virtio_pci.hh"
#ifdef LOONGARCH

/*
pci bar layout:
type 	region 		offset 	length	bar
0x01	COMMON_CFG	0x0000	0x1000	4
0x03	ISR_CFG		0x1000	0x1000	4
0x04	DEVICE_CFG	0x2000	0x1000	4
0x02	NOTIFY_CFG	0x3000	0x1000	4
*/

// virtio-net Feature Bits
#define VIRTIO_NET_F_CSUM 					(0)
#define VIRTIO_NET_F_GUEST_CSUM 			(1)
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS 	(2)
#define VIRTIO_NET_F_MTU					(3)
#define VIRTIO_NET_F_MAC 					(5)
#define VIRTIO_NET_F_GUEST_TSO4 			(7)
#define VIRTIO_NET_F_GUEST_TSO6 			(8)
#define VIRTIO_NET_F_GUEST_ECN  			(9)
#define VIRTIO_NET_F_GUEST_UFO  			(10)
#define VIRTIO_NET_F_HOST_TSO4  			(11)
#define VIRTIO_NET_F_HOST_TSO6  			(12)
#define VIRTIO_NET_F_HOST_ECN   			(13)
#define VIRTIO_NET_F_HOST_UFO   			(14)
#define VIRTIO_NET_F_MRG_RXBUF  			(15)
#define VIRTIO_NET_F_STATUS    				(16)
#define VIRTIO_NET_F_CTRL_VQ   				(17)
#define VIRTIO_NET_F_CTRL_RX   				(18)
#define VIRTIO_NET_F_CTRL_VLAN 				(19)
#define VIRTIO_NET_F_GUEST_ANNOUNCE			(21)
#define VIRTIO_NET_F_MQ						(22)
#define VIRTIO_NET_F_CTRL_MAC_ADDR			(23)
#define VIRTIO_NET_F_HOST_USO 				(56)
#define VIRTIO_NET_F_HASH_REPORT 			(57)
#define VIRTIO_NET_F_GUEST_HDRLEN			(59)
#define VIRTIO_NET_F_RSS					(60)
#define VIRTIO_NET_F_RSC_EXT				(61)
#define VIRTIO_NET_F_STANDBY				(62)
#define VIRTIO_NET_F_SPEED_DUPLEX			(63)
// Reserved Feature Bits
#define VIRTIO_F_INDIRECT_DESC 				(28)
#define VIRTIO_F_EVENT_IDX					(29)
#define VIRTIO_F_VERSION_1					(32)
#define VIRTIO_F_ACCESS_PLATFORM			(33)
#define VIRTIO_F_RING_PACKED				(34)
#define VIRTIO_F_IN_ORDER					(35)
#define VIRTIO_F_ORDER_PLATFORM				(36)
#define VIRTIO_F_SR_IOV						(37)
#define VIRTIO_F_NOTIFICATION_DATA			(38)
#define VIRTIO_F_NOTIF_CONFIG_DATA			(39)
#define VIRTIO_F_RING_RESET					(40)

/* Status byte for guest to report progress. */
#define VIRTIO_CONFIG_STATUS_RESET          0x00
#define VIRTIO_CONFIG_STATUS_ACK            0x01
#define VIRTIO_CONFIG_STATUS_DRIVER         0x02
#define VIRTIO_CONFIG_STATUS_DRIVER_OK      0x04
#define VIRTIO_CONFIG_STATUS_FEATURES_OK    0x08
#define VIRTIO_CONFIG_STATUS_FAILED         0x80

#define PCI_REG8(reg)   (*(volatile uint8 *)(reg))
#define PCI_REG16(reg)  (*(volatile uint16 *)(reg))
#define PCI_REG32(reg)  (*(volatile uint32 *)(reg))
#define PCI_REG64(reg)  (*(volatile uint64 *)(reg))

//struct virtio_pci_hw g_hw = { 0 };

static void *get_cfg_addr(uint64 pci_base, struct virtio_pci_cap *cap)
{
    // 对于 64bit来说是 BAR4 & BAR5
    uint64 reg = pci_base + PCI_ADDR_BAR0 + 4 * cap->bar;
    return (void *)((pci_config_read64(reg) & 0xFFFFFFFFFFFFFFF0) + cap->offset);
}

int virtio_pci_read_caps(virtio_pci_hw_t *hw, uint64 pci_base, trap_handler_fn *msix_isr)
{
    struct virtio_pci_cap cap;
    uint64 pos = 0;
    //struct virtio_pci_hw *hw = &g_hw;

    pos = pci_config_read8(pci_base + PCI_ADDR_CAP);         // 第一个 cap's offset in ECAM
    // printf("cap: 0x%016llx\n", pci_base + PCI_ADDR_CAP);     // cap pointer 在 ECAM 整个区域中的 offset

    // 遍历所有的 capability
    while (pos) {
        pos += pci_base;
        pci_config_read(&cap, sizeof(cap), pos);

        if (cap.cap_vndr != PCI_CAP_ID_VNDR) {         // PCI_CAP_ID_VNDR vendor-specific Cap
			// printf("[%2llx] skipping non VNDR cap id: %02x\n",
				    // pos, cap.cap_vndr);
			goto next;
		}

       //  printf("[%2llx] cfg type: %u, bar: %u, offset: %04x, len: %u\n",
			    // pos, cap.cfg_type, cap.bar, cap.offset, cap.length);

        switch (cap.cfg_type) {
		case VIRTIO_PCI_CAP_COMMON_CFG:
			hw->common_cfg = get_cfg_addr(pci_base, &cap);
            // printf("common_cfg addr: %016llx\n", (uint64)hw->common_cfg);
			break;
		case VIRTIO_PCI_CAP_NOTIFY_CFG:
			pci_config_read(&hw->notify_off_multiplier,
					4, pos + sizeof(cap));
            hw->notify_cfg = get_cfg_addr(pci_base, &cap);
            // printf("notify_cfg addr: %016llx\n", (uint64)hw->notify_cfg);
			break;
		case VIRTIO_PCI_CAP_DEVICE_CFG:
			hw->device_cfg = get_cfg_addr(pci_base, &cap);
            // printf("device_cfg addr: %016llx\n", (uint64)hw->device_cfg);
			break;
		case VIRTIO_PCI_CAP_ISR_CFG:
			hw->isr_cfg = get_cfg_addr(pci_base, &cap);
            // printf("isr_cfg addr: %016llx\n", (uint64)hw->isr_cfg);
			break;
		}
next:
		pos = cap.cap_next;
    }

    if (hw->common_cfg == NULL || hw->notify_cfg == NULL ||
	    hw->device_cfg == NULL    || hw->isr_cfg == NULL) {
		printf("no modern virtio pci device found.\n");
		return -1;
	}

    printf("found modern virtio pci device.\n");
    printf("use_msix: %d\n", hw->use_msix);
	printf("common cfg mapped at: %p\n", hw->common_cfg);
    printf("isr cfg mapped at: %p\n", hw->isr_cfg);
	printf("device cfg mapped at: %p\n", hw->device_cfg);
	printf("notify base: %p, notify off multiplier: %d\n", hw->notify_cfg, hw->notify_off_multiplier);

    return 0;
}

uint64 virtio_pci_get_device_features(virtio_pci_hw_t *hw)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG32(&cfg->device_feature_select) = 0;
    uint64 f1 = PCI_REG32(&cfg->device_feature);

    PCI_REG32(&cfg->device_feature_select) = 1;
    uint64 f2 = PCI_REG32(&cfg->device_feature);

    return (f2 << 32) | f1;
}

uint64 virtio_pci_get_driver_features(virtio_pci_hw_t *hw)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG32(&cfg->driver_feature_select) = 0;
    uint64 f1 = PCI_REG32(&cfg->driver_feature);

    PCI_REG32(&cfg->driver_feature_select) = 1;
    uint64 f2 = PCI_REG32(&cfg->driver_feature);

    return (f2 << 32) | f1;
}

void virtio_pci_set_driver_features(virtio_pci_hw_t *hw, uint64 features)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;
    PCI_REG32(&cfg->driver_feature_select) = 0;
    dsb();
    PCI_REG32(&cfg->driver_feature) = features & 0xFFFFFFFF;

    PCI_REG32(&cfg->driver_feature_select) = 1;
    dsb();
    PCI_REG32(&cfg->driver_feature) = features >> 32;
}

uint16 virtio_pci_get_queue_size(virtio_pci_hw_t *hw, int qid)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    return PCI_REG16(&cfg->queue_size);
}

void virtio_pci_set_queue_size(virtio_pci_hw_t *hw, int qid, int qsize)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    PCI_REG16(&cfg->queue_size) = qsize;
}

void virtio_pci_set_queue_addr(virtio_pci_hw_t *hw, int qid, struct vring *vr)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    PCI_REG64(&cfg->queue_desc) = ((uint64)vr->desc) & (~(DMWIN_MASK));
    PCI_REG64(&cfg->queue_driver) = ((uint64)vr->avail) & (~(DMWIN_MASK));
    PCI_REG64(&cfg->queue_device) = ((uint64)vr->used) & (~(DMWIN_MASK));
}

void virtio_pci_set_queue_addr2(virtio_pci_hw_t *hw, int qid, void *desc, void *avail, void *used)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();
    // printf("%x %x %x\n", ((uint64)desc) & (~(DMWIN_MASK)), ((uint64)avail) & (~(DMWIN_MASK)), ((uint64)used) & (~(DMWIN_MASK)));
    // PCI_REG64(&cfg->queue_desc) = ((uint64)desc) & (~(DMWIN_MASK));
    // PCI_REG64(&cfg->queue_driver) = ((uint64)avail) & (~(DMWIN_MASK));
    // PCI_REG64(&cfg->queue_device) = ((uint64)used) & (~(DMWIN_MASK));

    PCI_REG64(&cfg->queue_desc) = VIRT2PHY((uint64) desc);
    PCI_REG64(&cfg->queue_driver) = VIRT2PHY((uint64) avail);
    PCI_REG64(&cfg->queue_device) = VIRT2PHY((uint64) used);
}

uint32 virtio_pci_get_queue_notify_off(virtio_pci_hw_t *hw, int qid)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    return PCI_REG16(&cfg->queue_notify_off);
}

void *virtio_pci_get_queue_notify_addr(virtio_pci_hw_t *hw, int qid)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    // 对应的 virtqueue
    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    // 获得地址
    uint16 notify_off = PCI_REG16(&cfg->queue_notify_off);
    return (void *)((uint64)hw->notify_cfg + notify_off * hw->notify_off_multiplier);
}

// 通知 qid 对应的 virt queue
void virtio_pci_set_queue_notify(virtio_pci_hw_t *hw, int qid)
{
    // notify addr, 计算 notify 地址, 然后通知
    void *pt = virtio_pci_get_queue_notify_addr(hw, qid);
    PCI_REG32(pt) = 1;
}

void virtio_pci_set_queue_enable(virtio_pci_hw_t *hw, int qid)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    PCI_REG16(&cfg->queue_enable) = 1;;
}

uint16 virtio_pci_get_queue_enable(virtio_pci_hw_t *hw, int qid)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    return PCI_REG16(&cfg->queue_enable);
}

void virtio_pci_disable_queue_msix(virtio_pci_hw_t *hw, int qid)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    PCI_REG16(&cfg->queue_msix_vector) = 0xffff;
}

void virtio_pci_set_queue_msix(virtio_pci_hw_t *hw, int qid, uint16 msix_vector)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    // 首先 Queue Select 选择 对应的 VirtQueue
    PCI_REG16(&cfg->queue_select) = qid;
    dsb();

    // 然后写入中断向量
    PCI_REG16(&cfg->queue_msix_vector) = msix_vector;   // 写入 msix-vector
}

void virtio_pci_disable_config_msix(virtio_pci_hw_t *hw)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    PCI_REG16(&cfg->config_msix_vector) = 0xffff;
    dsb();
}

void virtio_pci_set_config_msix(virtio_pci_hw_t *hw, uint16 msix_vector)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

    // config msix vector 对应的中断向量
    PCI_REG16(&cfg->config_msix_vector) = msix_vector;
    dsb();
}

uint32 virtio_pci_clear_isr(virtio_pci_hw_t *hw)
{
    uint32 irq = PCI_REG32(hw->isr_cfg);
    return irq;
}

uint8 virtio_pci_get_status(virtio_pci_hw_t *hw)
{
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;
    return PCI_REG8(&cfg->device_status);
}

void virtio_pci_set_status(virtio_pci_hw_t *hw, uint8 status)
{
    struct virtio_pci_common_cfg *cfg = (struct virtio_pci_common_cfg *)hw->common_cfg;
    PCI_REG8(&cfg->device_status) = status;

}

void virtio_pci_print_common_cfg(virtio_pci_hw_t *hw)
{
    // volatile uint32 *cap = (volatile uint32 *)hw->common_cfg;
    // 打印 comman cfg 的内容
    // for (int i = 0; i < sizeof(struct virtio_pci_common_cfg)/sizeof(uint32); ++i) {
    //     printf("cap[%d]: 0x%08x\n", i, cap[i]);
    // }

    for (int i = 0; i < 8; ++i) {
        // 每个队列支持的 max size
        uint32 qsize = virtio_pci_get_queue_size(hw, i);
        // 若队列支持的大小为 0, 则跳过
        if (qsize == 0) continue;
        // notify offset
        uint32 notify_off = virtio_pci_get_queue_notify_off(hw, i);
        // vring buffer total size, 根据 qsize 计算需要的 buffer size, notify_offset
        uint32 vsize = virtio_vring_size(qsize);
        printf("queue[%d] qsize: %d, vsize: 0x%x, notify_off: 0x%08x\n", i, qsize, vsize, notify_off);
    }
    // 获取 device 支持的 features
    uint64 features = virtio_pci_get_device_features(hw);
    printf("features: 0x%016llx\n", features);
}

int virtio_pci_setup_queue(virtio_pci_hw_t *hw, struct vring *vr)
{
    uint16 notify_off;
    uint32 desc_addr, avail_addr, used_addr;
    struct virtio_pci_common_cfg *cfg = (virtio_pci_common_cfg *)hw->common_cfg;

	desc_addr = (uint64)vr->desc;
	avail_addr = (uint64)vr->avail;
	used_addr = (uint64)vr->used;

    PCI_REG32(&cfg->queue_select) = vr->qid;
    PCI_REG64(&cfg->queue_desc) = desc_addr;
    PCI_REG64(&cfg->queue_driver) = avail_addr;
    PCI_REG64(&cfg->queue_device) = used_addr;

    notify_off = PCI_REG16(&cfg->queue_notify_off);
    vr->notify_addr = (void *)((uint8 *)hw->notify_cfg +
				notify_off * hw->notify_off_multiplier);

    PCI_REG16(&cfg->queue_enable) = 1;

	printf("queue %u addresses:\n", vr->qid);
	printf("\t desc_addr: 0x%08x\n", desc_addr);
	printf("\t aval_addr: 0x%08x\n", avail_addr);
	printf("\t used_addr: 0x%08x\n", used_addr);
	printf("\t notify addr: %p (notify offset: %u)\n",
		vr->notify_addr, notify_off);

	return 0;
}
#endif