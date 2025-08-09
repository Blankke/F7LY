#include "fs/drivers/riscv/disk.hh"
#include "fs/drivers/riscv/sdcard.hh"
#include "fs/drivers/riscv/virtio2.hh"

void disk_init(void)
{
#ifdef QEMU
    virtio_disk_init();
#else
    sd_init();
#endif
}

void disk_rw(buf *buf, bool write)
{
#ifdef QEMU
    virtio_disk_rw(buf, write);
#else
    if (write)
    {
        sd_write((uint32 *)buf->data, 128, buf->blockno);
    }
    else
    {
        printfOrange("disk_rw: read blockno %u\n", buf->blockno);
        sd_read((uint32 *)buf->data, 128, buf->blockno);
    }
#endif
}

void disk_intr(void)
{
#ifdef QEMU
    virtio_disk_intr();
#else
    printf("should not have disk intr");
// dmac_intr(DMAC_CHANNEL0);
#endif
}
