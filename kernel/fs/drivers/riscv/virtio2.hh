#pragma once

#ifdef RISCV
//
// virtio device definitions.
// for both the mmio interface, and virtio descriptors.
// only tested with qemu.
// this is the "legacy" virtio interface.
//
// the virtio spec:
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
//

// virtio mmio control registers, mapped starting at 0x10001000.
// from qemu virtio_mmio.h
#define VIRTIO_MMIO_MAGIC_VALUE		0x000 // 0x74726976
#define VIRTIO_MMIO_VERSION		0x004 // version; 1 is legacy
#define VIRTIO_MMIO_DEVICE_ID		0x008 // device type; 1 is net, 2 is disk
#define VIRTIO_MMIO_VENDOR_ID		0x00c // 0x554d4551
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE	0x028 // page size for PFN, write-only
#define VIRTIO_MMIO_QUEUE_SEL		0x030 // select queue, write-only
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034 // max size of current queue, read-only
#define VIRTIO_MMIO_QUEUE_NUM		0x038 // size of current queue, write-only
#define VIRTIO_MMIO_QUEUE_ALIGN		0x03c // used ring alignment, write-only
#define VIRTIO_MMIO_QUEUE_PFN		0x040 // physical page number for queue, read/write
#define VIRTIO_MMIO_QUEUE_READY		0x044 // ready bit
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050 // write-only
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060 // read-only
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064 // write-only
#define VIRTIO_MMIO_STATUS		0x070 // read/write

// status register bits, from qemu virtio_config.h
#define VIRTIO_CONFIG_S_ACKNOWLEDGE	1
#define VIRTIO_CONFIG_S_DRIVER		2
#define VIRTIO_CONFIG_S_DRIVER_OK	4
#define VIRTIO_CONFIG_S_FEATURES_OK	8

// device feature bits
#define VIRTIO_BLK_F_RO              5	/* Disk is read-only */
#define VIRTIO_BLK_F_SCSI            7	/* Supports scsi command passthru */
#define VIRTIO_BLK_F_CONFIG_WCE     11	/* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ             12	/* support more than one vq */
#define VIRTIO_F_ANY_LAYOUT         27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX     29

// this many virtio descriptors.
// must be a power of two.
#define NUM 8

struct VRingDesc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
};
#define VRING_DESC_F_NEXT  1 // chained with another descriptor
#define VRING_DESC_F_WRITE 2 // device writes (vs read)

struct VRingUsedElem {
  uint32 id;   // index of start of completed descriptor chain
  uint32 len;
};

// for disk ops
#define VIRTIO_BLK_T_IN  0 // read the disk
#define VIRTIO_BLK_T_OUT 1 // write the disk

struct UsedArea {
  uint16 flags;
  uint16 id;
  struct VRingUsedElem elems[NUM];
};

struct virtio_blk_req {
  uint32 type; // VIRTIO_BLK_T_IN or ..._OUT
  uint32 reserved;
  uint64 sector;
};

void  virtio_disk_init(void);
void  virtio_disk_init2(void);
void  virtio_disk_rw(struct buf *, int);
void  virtio_disk_rw2(struct buf *, int);
void  virtio_disk_intr(void);
void  virtio_disk_intr2();

#elif defined(LOONGARCH)
#include "fs/buf.hh"
#include "fs/drivers/loongarch/virtio_ring.hh"


#define NUM 8

struct VRingDesc {
	uint64 addr;
	uint32 len;
	uint16 flags;
	uint16 next;
};
#define VRING_DESC_F_NEXT  1 // chained with another descriptor
#define VRING_DESC_F_WRITE 2 // device writes (vs read)

struct VRingUsedElem {
	uint32 id;   // index of start of completed descriptor chain
	uint32 len;
};

// for disk ops
#define VIRTIO_BLK_T_IN  0 // read the disk
#define VIRTIO_BLK_T_OUT 1 // write the disk

struct UsedArea {
	uint16 flags;
	uint16 id;
	struct VRingUsedElem elems[NUM];
};

struct virtio_blk_req {
	uint32 type; // VIRTIO_BLK_T_IN or ..._OUT
	uint32 reserved;
	uint64 sector;
};

#define virtio_blk_vendor 0x1af4
#define virtio_blk_device 0x1001

#define BLK_QSIZE       (128)   // blk queue0 size
#define SECTOR_SZIE     (512)   // blk sector size

// block device feature bits
#define VIRTIO_BLK_F_RO              5	/* Disk is read-only */
#define VIRTIO_BLK_F_SCSI            7	/* Supports scsi command passthru */
#define VIRTIO_BLK_F_FLUSH           9  // Cache flush command support
#define VIRTIO_BLK_F_CONFIG_WCE     11	/* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ             12	/* support more than one vq */

#define VIRTIO_STAT_ACKNOWLEDGE		1
#define VIRTIO_STAT_DRIVER		    2
#define VIRTIO_STAT_DRIVER_OK		4
#define VIRTIO_STAT_FEATURES_OK		8
#define VIRTIO_STAT_NEEDS_RESET		64
#define VIRTIO_STAT_FAILED		    128

/* VIRTIO 1.0 Device independent feature bits */
#define VIRTIO_F_ANY_LAYOUT         (27)
#define VIRTIO_F_INDIRECT_DESC	    (28)
#define VIRTIO_F_EVENT_IDX	        (29)
#define VIRTIO_F_VERSION_1	        (32)
#define VIRTIO_F_RING_PACKED	    (34)
#define VIRTIO_F_IN_ORDER	        (35)
#define VIRTIO_F_ORDER_PLATFORM	    (36)
#define VIRTIO_F_SR_IOV	            (37)
#define VIRTIO_F_NOTIFICATION_DATA  (38)
#define VIRTIO_F_NOTIF_CONFIG_DATA  (39)
#define VIRTIO_F_RING_RESET	        (40)

// As per VirtIO spec Version 1.1: 5.2.4 Device configuration layout
struct virtio_blk_cfg {
	uint64	capacity;
	uint32	size_max;
	uint32	seg_max;
	struct	virtio_blk_geometry {
		uint16	cylinders;
		uint8 	heads;
		uint8 	sectors;
	} geometry;
	uint32	blk_size;
	struct virtio_blk_topology {
		// # of logical blocks per physical block (log2)
		uint8 physical_block_exp;
		// offset of first aligned logical block
		uint8 alignment_offset;
		// suggested minimum I/O size in blocks
			uint16 min_io_size;
		// optimal (suggested maximum) I/O size in blocks
		uint32 opt_io_size;
	} topology;
	uint8 writeback;
	uint8 unused0[3];
	uint32 max_discard_sectors;
	uint32 max_discard_seg;
	uint32 discard_sector_alignment;
	uint32 max_write_zeroes_sectors;
	uint32 max_write_zeroes_seg;
	uint8 write_zeroes_may_unmap;
	uint8 unused1[3];
} __attribute__((packed));

// these are specific to virtio block devices, e.g. disks,
// described in Section 5.2 of the spec.

#define VIRTIO_BLK_T_IN     0 // read the disk
#define VIRTIO_BLK_T_OUT    1 // write the disk
#define VIRTIO_BLK_T_FLUSH  4 // flush the disk


struct virtio_blk {
    uint8  status[BLK_QSIZE];
    void *info[BLK_QSIZE];
    // disk command headers.
    // one-for-one with descriptors, for convenience.
    struct virtio_blk_req ops[BLK_QSIZE];
    struct vring vr;
	uint32 capacity;
	uint32 qsize;	// queue0 size
    uint16 used_idx;
	uint16 avail_idx;
};


//代表第一个virtio块设备
extern unsigned char bus1;
extern unsigned char device1;
extern unsigned char function1;

extern unsigned char bus2;
extern unsigned char device2;
extern unsigned char function2;

extern uint64 pci_base1;
extern uint64 pci_base2;


void  virtio_disk_init(void);
void  virtio_disk_init2(void);
void  virtio_disk_rw(struct buf *, int);
void  virtio_disk_rw2(struct buf *, int);
void  virtio_disk_intr(void);
void  virtio_disk_intr2();
void virtio_probe();

#endif