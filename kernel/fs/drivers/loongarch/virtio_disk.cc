#include "types.hh"
#include "trap/loongarch/pci.h"
#include "fs/drivers/riscv/virtio2.hh"
#include "printer.hh"
#include "virtio_pci.hh"
#include "libs/string.hh"
#include "proc_manager.hh"
#ifdef LOONGARCH
unsigned char bus1;
unsigned char device1;
unsigned char function1;
unsigned char bus2;
unsigned char device2;
unsigned char function2;

uint8 gs_blk_buf[3*4096] __attribute__((aligned(4096))) = { 0 };
virtio_pci_hw_t gs_virtio_blk_hw = { 0 };
virtio_pci_hw_t gs_virtio_blk_hw2 = { 0 };

uint64 pci_base1;
uint64 pci_base2;

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

void virtio_probe() {
    pci_base1 = pci_device_probe(0x1af4, 0x1001);
    pci_base2 = pci_device_probe(0x1af4, 0x1001);
}

void pci_device_init(uint64 pci_base, unsigned char bus, unsigned char device, unsigned char function) {
   mem::k_vmm.pci_map(bus, device, function, disk.pages);
    // printf("map finish\n");
    unsigned int val = 0;
    val = pci_config_read16(pci_base + PCI_STATUS_COMMAND);
    // printf("%d\n", val);
    val |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_IO;
    pci_config_write16(pci_base + PCI_STATUS_COMMAND, val);
    val = pci_config_read16(pci_base + PCI_STATUS_COMMAND);
    // printf("%d\n",val);
    // uint8 irq = PCIE_IRQ;
    // pci_config_write8(pci_base + 0x3C, irq);
    // // irq = 0;
    // irq = pci_config_read8(pci_base + 0x3C);
    // printf("%d\n", irq);

    uint64 off = (bus << 16) | (device << 11) | (function << 8);
    volatile uint32 *base =(uint32*) (PCIE0_ECAM + (uint64)(off));
    for(int i = 0; i < 6; i++) {
        uint32 old = base[4+i];
        // printf("bar%d origin value: 0x%08x\t", i, old);
        if(old & 0x1) {
            // printf("IO space\n");
            continue;                                      // 未用到 IO 空间, 暂时也不进行分配等
        } else {                                           // 仅为 Mem 空间进行进一步的处理
            // printf("Mem space\t");
        }
        if(old & 0x4) {
            // printf("%x %x\n", base[4 + i], base[4 + i + 1]);             // 64bit 系统映射
            base[4+i] = 0xffffffff;
            base[4+i+1] = 0xffffffff;
            __sync_synchronize();

            uint64 sz = ((uint64)base[4+i+1] << 32) | base[4+i];
            sz = ~(sz & 0xFFFFFFFFFFFFFFF0) + 1;
            // printf("bar%d need size: 0x%x\n", i, sz);
            uint64 mem_addr = pci_alloc_mmio(sz);
            // 写入分配的大小
            base[4+i] = (uint32)(mem_addr);
            base[4+i+1] = (uint32)(mem_addr >> 32);
            __sync_synchronize();
            // printf("%p\n", mem_addr);
            i++;                                    // 跳过下一个 BAR
        }
    }
}

void virtio_disk_init() {
    // pci_scan_buses();

    pci_device_init(pci_base1, bus1, device1, function1);
    if (pci_base1) {
        virtio_pci_read_caps(&gs_virtio_blk_hw, pci_base1, 0);
        // virtio_pci_print_common_cfg(&gs_virtio_blk_hw);
    } else {
        printf("virtion-blk-pci device not found!\n");
        return ;
    }

    // for (int i = 0;i < PGSIZE;i+=8) {
    //     printf("%x ", *(volatile uint64 *)((uint64)gs_virtio_blk_hw.common_cfg + i));
    // }

    // 1. reset device
    virtio_pci_set_status(&gs_virtio_blk_hw, 0);

    uint8 status = 0;
    // 2. set ACKNOWLEDGE status bit
    status |= VIRTIO_STAT_ACKNOWLEDGE;
    virtio_pci_set_status(&gs_virtio_blk_hw, status);

    // 3. set DRIVER status bit
    status |= VIRTIO_STAT_DRIVER;
    virtio_pci_set_status(&gs_virtio_blk_hw, status);

    // 4. negotiate features
    uint64 features = virtio_pci_get_device_features(&gs_virtio_blk_hw);

    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_F_INDIRECT_DESC);

    virtio_pci_set_driver_features(&gs_virtio_blk_hw, features);

    // 5. tell device that feature negotiation is complete.
    status |= VIRTIO_STAT_FEATURES_OK;
    virtio_pci_set_status(&gs_virtio_blk_hw, status);

    // 6. re-read status to ensure FEATURES_OK is set.
    status = virtio_pci_get_status(&gs_virtio_blk_hw);
    if(!(status & VIRTIO_STAT_FEATURES_OK)) {
        printf("virtio disk FEATURES_OK unset");
        return ;
    }

    // 7. initialize queue 0.
    [[maybe_unused]]int qnum = 0;
   [[maybe_unused]] int qsize = BLK_QSIZE;
    // ensure queue 0 is not in use.

    if (virtio_pci_get_queue_enable(&gs_virtio_blk_hw, 0)) {
        printf("virtio disk should not be ready");
        return ;
    }

    // check maximum queue size.
    uint32 max = virtio_pci_get_queue_size(&gs_virtio_blk_hw, 0);
    printf("queue_0 max size: %d\n", max);
    if(max == 0){
        printf("virtio disk has no queue 0");
        return ;
    }
    if(max < NUM){
        printf("virtio disk max queue too short");
        return ;
    }

    disk.vdisk_lock.init( "virtio disk lock");
    virtio_pci_set_queue_size(&gs_virtio_blk_hw, 0, NUM);
    memset(disk.pages, 0, sizeof(disk.pages));
    disk.desc = (struct VRingDesc *) disk.pages;
    disk.avail = (uint16*)(((char*)disk.desc) + NUM*sizeof(struct VRingDesc));
    disk.used = (struct UsedArea *) (disk.pages + PGSIZE);
    for(int i = 0; i < NUM; i++)
        disk.free[i] = 1;

    virtio_pci_set_queue_addr2(&gs_virtio_blk_hw, 0, disk.desc, disk.avail, disk.used);

    virtio_pci_set_queue_enable(&gs_virtio_blk_hw, 0);

    status |= VIRTIO_STAT_DRIVER_OK;
    virtio_pci_set_status(&gs_virtio_blk_hw, status);
    return ;

}

void  virtio_disk_init2(void) {
    pci_device_init(pci_base2, bus2, device2, function2);
    if (pci_base2) {
        virtio_pci_read_caps(&gs_virtio_blk_hw2, pci_base2, 0);
        // virtio_pci_print_common_cfg(&gs_virtio_blk_hw);
    } else {
        printf("virtion-blk-pci device not found!\n");
        return ;
    }

    // 1. reset device
    virtio_pci_set_status(&gs_virtio_blk_hw2, 0);

    uint8 status = 0;
    // 2. set ACKNOWLEDGE status bit
    status |= VIRTIO_STAT_ACKNOWLEDGE;
    virtio_pci_set_status(&gs_virtio_blk_hw2, status);

    // 3. set DRIVER status bit
    status |= VIRTIO_STAT_DRIVER;
    virtio_pci_set_status(&gs_virtio_blk_hw2, status);

    // 4. negotiate features
    uint64 features = virtio_pci_get_device_features(&gs_virtio_blk_hw2);

    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_F_INDIRECT_DESC);

    virtio_pci_set_driver_features(&gs_virtio_blk_hw2, features);

    // 5. tell device that feature negotiation is complete.
    status |= VIRTIO_STAT_FEATURES_OK;
    virtio_pci_set_status(&gs_virtio_blk_hw2, status);

    // 6. re-read status to ensure FEATURES_OK is set.
    status = virtio_pci_get_status(&gs_virtio_blk_hw2);
    if(!(status & VIRTIO_STAT_FEATURES_OK)) {
        printf("virtio disk FEATURES_OK unset");
        return ;
    }

    // 7. initialize queue 0.
   [[maybe_unused]] int qnum = 0;
    [[maybe_unused]]int qsize = BLK_QSIZE;
    // ensure queue 0 is not in use.

    if (virtio_pci_get_queue_enable(&gs_virtio_blk_hw2, qnum)) {
        printf("virtio disk should not be ready");
        return ;
    }

    // check maximum queue size.
    uint32 max = virtio_pci_get_queue_size(&gs_virtio_blk_hw2, qnum);
    printf("queue_0 max size: %d\n", max);
    if(max == 0){
        printf("virtio disk has no queue 0");
        return ;
    }
    if(max < NUM){
        printf("virtio disk max queue too short");
        return ;
    }

    disk2.vdisk_lock.init("virtio disk lock");
    virtio_pci_set_queue_size(&gs_virtio_blk_hw2, 0, NUM);
    memset(disk2.pages, 0, sizeof(disk2.pages));
    disk2.desc = (struct VRingDesc *) disk2.pages;
    disk2.avail = (uint16*)(((char*)disk2.desc) + NUM*sizeof(struct VRingDesc));
    disk2.used = (struct UsedArea *) (disk2.pages + PGSIZE);
    for(int i = 0; i < NUM; i++)
        disk2.free[i] = 1;

    virtio_pci_set_queue_addr2(&gs_virtio_blk_hw2, qnum, disk2.desc, disk2.avail, disk2.used);

    virtio_pci_set_queue_enable(&gs_virtio_blk_hw2, qnum);

    status |= VIRTIO_STAT_DRIVER_OK;
    virtio_pci_set_status(&gs_virtio_blk_hw2, status);
    return ;
}


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


void  virtio_disk_rw(struct buf *b, int write) {
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
  disk.desc[idx[0]].addr = PA2VA((uint64)mem::k_pagetable.walk_addr((uint64) &buf0));

  // disk.desc[idx[0]].addr = (uint64) &buf0;
  disk.desc[idx[0]].len = sizeof(buf0);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = VIRT2PHY((uint64)b->data);

  disk.desc[idx[1]].len = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0;
  disk.desc[idx[2]].addr = VIRT2PHY((uint64) &disk.info[idx[0]].status);
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
  disk.avail[1]++;

  virtio_pci_set_queue_notify(&gs_virtio_blk_hw, 0);

  // Wait for virtio_disk_intr() to say request has finished.
  // printf("Before\n");
  // while(b->disk == 1) {
  //   proc::k_pm.sleep(b, &disk.vdisk_lock);
  // }
    // volatile uint16 *pt_used_idx = &disk.used_idx;
    // volatile uint16 *pt_idx = &disk.used->id;
    //     wait cmd done
    // while (*pt_used_idx == *pt_idx) ;

    while (!virtio_pci_clear_isr(&gs_virtio_blk_hw)) ;

    int id = disk.used->elems[disk.used_idx].id;

    if(disk.info[id].status != 0)
        panic("virtio_disk_intr status");

    proc::k_pm.wakeup(disk.info[id].b);

    disk.used_idx = (disk.used_idx + 1) % NUM;

    b->disk = 0;

  // printf("After\n");
  // for (int i=0; i<512;i++) {
  //     printf("%d ", b->data[i]);
  // }
  // printf("\n");

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

disk.vdisk_lock.release();
  // printf("[virtio_disk_rw] done, cpuid: %d\n", cpuid());
}

void  virtio_disk_rw2(struct buf *b, int write) {
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
  disk2.desc[idx[0]].addr = PA2VA((uint64)mem::k_pagetable.walk_addr((uint64) &buf0));

  // disk2.desc[idx[0]].addr = (uint64) &buf0;
  disk2.desc[idx[0]].len = sizeof(buf0);
  disk2.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk2.desc[idx[0]].next = idx[1];

  disk2.desc[idx[1]].addr = VIRT2PHY((uint64)b->data);
  disk2.desc[idx[1]].len = BSIZE;
  if(write)
    disk2.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk2.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk2.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk2.desc[idx[1]].next = idx[2];

  disk2.info[idx[0]].status = 0;
  disk2.desc[idx[2]].addr = VIRT2PHY((uint64) &disk2.info[idx[0]].status);

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

    virtio_pci_set_queue_notify(&gs_virtio_blk_hw2, 0);

  // Wait for virtio_disk_intr() to say request has finished.
    // volatile uint16 *pt_used_idx = &disk2.used_idx;
    // volatile uint16 *pt_idx = &disk2.used->id;
    //     wait cmd done
    // printf("Before\n");
    // while (*pt_used_idx == *pt_idx) ;
    // printf("After\n");

    while (!virtio_pci_clear_isr(&gs_virtio_blk_hw2)) ;

    int id = disk2.used->elems[disk2.used_idx].id;

    if(disk2.info[id].status != 0)
        panic("virtio_disk2_intr status");

    proc::k_pm.wakeup(disk2.info[id].b);

    disk2.used_idx = (disk2.used_idx + 1) % NUM;


    b->disk = 0;

  disk2.info[idx[0]].b = 0;
  free_chain2(idx[0]);

  disk2.vdisk_lock.release();
  // printf("[virtio_disk_rw] done, cpuid: %d\n", cpuid());
}

/*
 *暂时不使用中断
 *不知道为什么接收不到磁盘中断
 */
void  virtio_disk_intr(void) {
    // disk.vdisk_lock.acquire()
    // 21 号中断直接会到达
    virtio_pci_clear_isr(&gs_virtio_blk_hw);

    dsb();

    while((disk.used_idx % NUM) != (disk.used->id % NUM)){
        int id = disk.used->elems[disk.used_idx].id;

        if(disk.info[id].status != 0)
            panic("virtio_disk_intr status");

        disk.info[id].b->disk = 0;   // disk is done with buf
        proc::k_pm.wakeup(disk.info[id].b);

        disk.used_idx = (disk.used_idx + 1) % NUM;
    }

    // disk.vdisk_lock.release()
    return ;
}
void  virtio_disk_intr2() {
    // disk2.vdisk_lock.acquire()
    virtio_pci_clear_isr(&gs_virtio_blk_hw2);

    dsb();

    while((disk2.used_idx % NUM) != (disk2.used->id % NUM)){
        int id = disk2.used->elems[disk2.used_idx].id;

        if(disk2.info[id].status != 0)
            panic("virtio_disk_intr status");

        disk2.info[id].b->disk = 0;   // disk is done with buf
        proc::k_pm.wakeup(disk2.info[id].b);

        disk2.used_idx = (disk2.used_idx + 1) % NUM;
    }
    // disk2.vdisk_lock.release();
    return ;
}
#endif