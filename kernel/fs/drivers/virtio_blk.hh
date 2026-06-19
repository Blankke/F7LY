#pragma once

#include "types.hh"
#include "fs/buf.hh"

namespace virtio_blk
{
    constexpr uint16 k_queue_size = 128;

    struct VRingDesc
    {
        uint64 addr;
        uint32 len;
        uint16 flags;
        uint16 next;
    };

    constexpr uint16 k_desc_flag_next = 1;
    constexpr uint16 k_desc_flag_write = 2;

    struct VRingUsedElem
    {
        uint32 id;
        uint32 len;
    };

    struct UsedArea
    {
        uint16 flags;
        uint16 id;
        VRingUsedElem elems[k_queue_size];
    };

    struct VirtioBlkReq
    {
        uint32 type;
        uint32 reserved;
        uint64 sector;
    };

    constexpr uint32 k_req_type_read = 0;
    constexpr uint32 k_req_type_write = 1;

} // namespace virtio_blk

void virtio_disk_init(void);
void virtio_disk_init2(void);
void virtio_disk_rw(struct buf *b, int write);
void virtio_disk_rw2(struct buf *b, int write);
int virtio_disk_rw_sectors(int dev, void *buf, uint64 start_sector, uint32 sector_count, int write);
void virtio_disk_intr(void);
void virtio_disk_intr2(void);

#ifdef LOONGARCH
void virtio_probe();
#endif
