//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.hh"
#include "platform.hh"

#include "param.h"
#include "mem/memlayout.hh"
#include "spinlock.hh"
#include "sleeplock.hh"
#include "fs/vfs/fs.hh"
#include "fs/buf.hh"
#include "drivers/riscv/virtio2.hh"
#include "libs/string.hh"
#include "virtual_memory_manager.hh"
#include "proc_manager.hh"
// the address of virtio mmio register r.
#ifdef RISCV
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))
#define R2(r) ((volatile uint32 *)(VIRTIO1 + (r)))
#elif defined(LOONGARCH)

#define R(r) ((volatile uint32 *)(r))
#define R2(r) ((volatile uint32 *)(r))
#endif


static struct disk {
 // memory for virtio descriptors &c for queue 0.
 // this is a global instead of allocated because it must
 // be multiple contiguous pages, which kalloc()
 // doesn't support, and page aligned.
  char pages[2*PGSIZE];
  struct VRingDesc *desc;
  uint16 *avail;
  struct UsedArea *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  SpinLock vdisk_lock;

} __attribute__ ((aligned (PGSIZE))) disk, disk2;

void
virtio_disk_init(void)
{
  uint32 status = 0;

  disk.vdisk_lock.init( "virtio_disk_lock");

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
  *R(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk.pages) >> PGSHIFT;

  // desc = pages -- num * VRingDesc
  // avail = pages + 0x40 -- 2 * uint16, then num * uint16
  // used = pages + 4096 -- 2 * uint16, then num * vRingUsedElem

  disk.desc = (struct VRingDesc *) disk.pages;
  disk.avail = (uint16*)(((char*)disk.desc) + NUM*sizeof(struct VRingDesc));
  disk.used = (struct UsedArea *) (disk.pages + PGSIZE);

  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
  #ifdef DEBUG
  printf("virtio_disk_init\n");
  #endif
}

void
virtio_disk_init2(void)
{
  uint32 status = 0;

  disk2.vdisk_lock.init("virtio_disk2");

  if(*R2(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R2(VIRTIO_MMIO_VERSION) != 1 ||
     *R2(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R2(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk2");
  }

  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R2(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER;
  *R2(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R2(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R2(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R2(VIRTIO_MMIO_STATUS) = status;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R2(VIRTIO_MMIO_STATUS) = status;

  *R2(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;

  // initialize queue 0.
  *R2(VIRTIO_MMIO_QUEUE_SEL) = 0;
  uint32 max = *R2(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk2 has no queue 0");
  if(max < NUM)
    panic("virtio disk2 max queue too short");
  *R2(VIRTIO_MMIO_QUEUE_NUM) = NUM;
  memset(disk2.pages, 0, sizeof(disk2.pages));
  *R2(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk2.pages) >> PGSHIFT;

  // desc = pages -- num * VRingDesc
  // avail = pages + 0x40 -- 2 * uint16, then num * uint16
  // used = pages + 4096 -- 2 * uint16, then num * vRingUsedElem

  disk2.desc = (struct VRingDesc *) disk2.pages;
  disk2.avail = (uint16*)(((char*)disk2.desc) + NUM*sizeof(struct VRingDesc));
  disk2.used = (struct UsedArea *) (disk2.pages + PGSIZE);

  for(int i = 0; i < NUM; i++)
    disk2.free[i] = 1;

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
  #ifdef DEBUG
  printf("virtio_disk_init\n");
  #endif
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

static int
alloc_desc2()
{
  for(int i = 0; i < NUM; i++){
    if(disk2.free[i]){
      disk2.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("virtio_disk_intr 1");
  if(disk.free[i])
    panic("virtio_disk_intr 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  proc::k_pm.wakeup(&disk.free[0]);
}

static void
free_desc2(int i)
{
  if(i >= NUM)
    panic("virtio_disk2_intr 1");
  if(disk2.free[i])
    panic("virtio_disk2_intr 2");
  disk2.desc[i].addr = 0;
  disk2.desc[i].len = 0;
  disk2.desc[i].flags = 0;
  disk2.desc[i].next = 0;
  disk2.free[i] = 1;
  proc::k_pm.wakeup(&disk2.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

static void
free_chain2(int i)
{
  while(1){
    int flag = disk2.desc[i].flags;
    int nxt = disk2.desc[i].next;
    free_desc2(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

static int
alloc3_desc(int *idx)
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

static int
alloc3_desc2(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc2();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc2(idx[j]);
      return -1;
    }
  }
  return 0;
}


void
virtio_disk_rw(struct buf *b, int write)
{
  // printf("[virtio_disk_rw] virtio disk rw, cpuid: %d\n", cpuid());
  uint64 sector = b->blockno;

  disk.vdisk_lock.acquire();

  // the spec says that legacy block operations use three
  // descriptors: one for type/reserved/sector, one for
  // the data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0) {
      break;
    }
    proc::k_pm.sleep(&disk.free[0], &disk.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_outhdr {
    uint32 type;
    uint32 reserved;
    uint64 sector;
  } buf0;

  if(write)
    buf0.type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0.type = VIRTIO_BLK_T_IN; // read the disk
  buf0.reserved = 0;
  buf0.sector = sector;

  // buf0 is on a kernel stack, which is not direct mapped,
  // thus the call to kvmpa().
  // disk.desc[idx[0]].addr = (uint64)mem::k_pagetable.kwalkaddr((uint64) &buf0).get_data();
  disk.desc[idx[0]].addr = (uint64) mem::k_pagetable.kwalk_addr((uint64)&buf0);
  // disk.desc[idx[0]].addr = (uint64) &buf0;
  disk.desc[idx[0]].len = sizeof(buf0);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64)b->data;
  disk.desc[idx[1]].len = BSIZE;
  disk.desc[idx[1]].flags = write ? 0 : VRING_DESC_F_WRITE;
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0;
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  b->disk = 1;
  disk.info[idx[0]].b = b;

  // avail[0] is flags
  // avail[1] tells the device how far to look in avail[2...].
  // avail[2...] are desc[] indices the device should process.
  // we only tell device the first index in our chain of descriptors.
  disk.avail[2 + (disk.avail[1] % NUM)] = idx[0];
  __sync_synchronize();
  disk.avail[1] = disk.avail[1] + 1;

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    proc::k_pm.sleep(b, &disk.vdisk_lock);
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  printf("b->data: %p, b->blockno: %d\n", b->data, b->blockno);
  for (int i = 0; i < BSIZE; ++i) {
    printfMagenta("%02x ", ((unsigned char*)b->data)[i]);
    if ((i + 1) % 16 == 0) printf("\n");
  }
  disk.vdisk_lock.release();
  // printf("[virtio_disk_rw] done, cpuid: %d\n", cpuid());
}

void
virtio_disk_rw2(struct buf *b, int write)
{
  // printf("[virtio_disk_rw] virtio disk rw, cpuid: %d\n", cpuid());
  uint64 sector = b->blockno;

  disk2.vdisk_lock.acquire();

  // the spec says that legacy block operations use three
  // descriptors: one for type/reserved/sector, one for
  // the data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc2(idx) == 0) {
      break;
    }
    proc::k_pm.sleep(&disk2.free[0], &disk2.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_outhdr {
    uint32 type;
    uint32 reserved;
    uint64 sector;
  } buf0;

  if(write)
    buf0.type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0.type = VIRTIO_BLK_T_IN; // read the disk
  buf0.reserved = 0;
  buf0.sector = sector;

  // buf0 is on a kernel stack, which is not direct mapped,
  // thus the call to kvmpa().
  disk2.desc[idx[0]].addr = (uint64)mem::k_pagetable.kwalkaddr((uint64) &buf0).get_data();
  // disk2.desc[idx[0]].addr = (uint64) &buf0;
  disk2.desc[idx[0]].len = sizeof(buf0);
  disk2.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk2.desc[idx[0]].next = idx[1];

  disk2.desc[idx[1]].addr = (uint64)b->data;
  disk2.desc[idx[1]].len = BSIZE;
  if(write)
    disk2.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk2.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk2.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk2.desc[idx[1]].next = idx[2];

  disk2.info[idx[0]].status = 0;
  disk2.desc[idx[2]].addr = (uint64) &disk2.info[idx[0]].status;
  disk2.desc[idx[2]].len = 1;
  disk2.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk2.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk2_intr().
  b->disk = 2;
  disk2.info[idx[0]].b = b;

  // avail[0] is flags
  // avail[1] tells the device how far to look in avail[2...].
  // avail[2...] are desc[] indices the device should process.
  // we only tell device the first index in our chain of descriptors.
  disk2.avail[2 + (disk2.avail[1] % NUM)] = idx[0];
  __sync_synchronize();
  disk2.avail[1] = disk2.avail[1] + 1;

  *R2(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 2) {
    proc::k_pm.sleep(b, &disk2.vdisk_lock);
  }

  disk2.info[idx[0]].b = 0;
  free_chain2(idx[0]);

  disk2.vdisk_lock.release();
  // printf("[virtio_disk_rw] done, cpuid: %d\n", cpuid());
}

void
virtio_disk_intr()
{
  // printf("[virtio_disk_intr] virtio_disk_intr!\n");
  disk.vdisk_lock.acquire();

  while((disk.used_idx % NUM) != (disk.used->id % NUM)){
    int id = disk.used->elems[disk.used_idx].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    disk.info[id].b->disk = 0;   // disk is done with buf
    proc::k_pm.wakeup(disk.info[id].b);

    disk.used_idx = (disk.used_idx + 1) % NUM;
  }
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  disk.vdisk_lock.release();
  // printf("[virtio_disk_intr] done!\n");
}

void
virtio_disk_intr2()
{
  // printf("[virtio_disk_intr] virtio_disk_intr!\n");
  disk2.vdisk_lock.acquire();

  while((disk2.used_idx % NUM) != (disk2.used->id % NUM)){
    int id = disk2.used->elems[disk2.used_idx].id;

    if(disk2.info[id].status != 0)
      panic("virtio_disk_intr status");

    disk2.info[id].b->disk = 0;   // disk is done with buf
    proc::k_pm.wakeup(disk2.info[id].b);

    disk2.used_idx = (disk2.used_idx + 1) % NUM;
  }
  *R2(VIRTIO_MMIO_INTERRUPT_ACK) = *R2(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  disk2.vdisk_lock.release();
  // printf("[virtio_disk_intr] done!\n");
}