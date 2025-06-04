#pragma once

#ifdef LOONGARCH
#include "types.hh"

/* Definitions for vring_desc.flags */
#define VRING_DESC_F_NEXT	    1	/* buffer continues via the next field */
#define VRING_DESC_F_WRITE	    2	/* buffer is write-only (otherwise read-only) */
#define VRING_DESC_F_INDIRECT	4	/* buffer contains a list of buffer descriptors */
/* Descriptor table entry - see Virtio Spec chapter 2.3.2 */
struct vring_desc {
    uint64 addr;		/* Address (guest-physical) */
    uint32 len;		/* Length */
    uint16 flags;		/* The flags as indicated above */
    uint16 next;		/* Next field if flags & NEXT */
};

/* Definitions for vring_avail.flags */
#define VRING_AVAIL_F_NO_INTERRUPT	1
/* Available ring - see Virtio Spec chapter 2.3.4 */
struct vring_avail {
    uint16 flags;
    uint16 idx;
    uint16 ring[];
};

/* Definitions for vring_used.flags */
#define VRING_USED_F_NO_NOTIFY		1
struct vring_used_elem {
    uint32 id;		    /* Index of start of used descriptor chain */
    uint32 len;		/* Total length of the descriptor chain which was used */
};
struct vring_used {
    uint16 flags;
    uint16 idx;
    struct vring_used_elem ring[];
};

struct vring {
    uint32                 size;   // power of 2
    int					qid;
    struct vring_desc   *desc;
    struct vring_avail  *avail;
    struct vring_used   *used;
    void 				*notify_addr;
};

/*
 * We publish the used event index at the end of the available ring, and vice
 * versa. They are at the end for backwards compatibility.
 */
#define vring_used_event(vr)  ((vr)->avail->ring[(vr)->num])
#define vring_avail_event(vr) (*(uint16_t *)&(vr)->used->ring[(vr)->num])

uint32 virtio_vring_size(uint32 qsize);
int virtio_vring_init(struct vring *vr, uint8 *buf, uint32 buf_len, uint32 qsize, int qid);
void virtio_vring_fill_desc(struct vring_desc *desc, uint64 addr, uint32 len, uint16 flags, uint16 next);
void virtio_vring_add_avail(struct vring_avail *avail, uint16 idx, uint32 qsize);

#endif