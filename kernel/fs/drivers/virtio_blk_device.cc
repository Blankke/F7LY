#include "virtio_blk_device.hh"

namespace virtio_blk
{
    void VirtioBlkDevice::initialize(const InitArgs &args)
    {
        transport_ = args.transport;
        queue_.initialize({args.lock_name, args.owner_token, transport_});
    }

    void *VirtioBlkDevice::pages_base()
    {
        return queue_.pages_base();
    }

    uint64 VirtioBlkDevice::pages_dma_base() const
    {
        return queue_.pages_dma_base();
    }

    VRingDesc *VirtioBlkDevice::desc_area()
    {
        return queue_.desc_area();
    }

    uint16 *VirtioBlkDevice::avail_area()
    {
        return queue_.avail_area();
    }

    UsedArea *VirtioBlkDevice::used_area()
    {
        return queue_.used_area();
    }

    void VirtioBlkDevice::submit_and_wait(struct buf *b, int write)
    {
        queue_.submit_and_wait(b, write);
    }

    int VirtioBlkDevice::submit_transfer_and_wait(void *data, uint32 data_len, uint64 start_sector, bool write)
    {
        return queue_.submit_transfer_and_wait(data, data_len, start_sector, write);
    }

    void VirtioBlkDevice::handle_interrupt()
    {
        queue_.handle_interrupt();
    }

    void VirtioBlkDevice::poll_once()
    {
        queue_.poll_once();
    }
} // namespace virtio_blk
