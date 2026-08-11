#include "net/drivers/virtio_net_transport.hh"

#include "hal/riscv/platform_board.hh"
#include "libs/printer.hh"
#include "hal/arch.hh"

namespace net::virtio_transport
{
namespace
{
constexpr uint32 k_magic_value = 0x000;
constexpr uint32 k_version = 0x004;
constexpr uint32 k_device_id = 0x008;
constexpr uint32 k_vendor_id = 0x00c;
constexpr uint32 k_device_features = 0x010;
constexpr uint32 k_driver_features = 0x020;
constexpr uint32 k_guest_page_size = 0x028;
constexpr uint32 k_queue_select = 0x030;
constexpr uint32 k_queue_size_max = 0x034;
constexpr uint32 k_queue_size = 0x038;
constexpr uint32 k_queue_align = 0x03c;
constexpr uint32 k_queue_pfn = 0x040;
constexpr uint32 k_queue_notify = 0x050;
constexpr uint32 k_interrupt_status = 0x060;
constexpr uint32 k_interrupt_ack = 0x064;
constexpr uint32 k_status = 0x070;
constexpr uint32 k_device_config = 0x100;

constexpr uint32 k_virtio_magic = 0x74726976;
constexpr uint32 k_qemu_vendor = 0x554d4551;
constexpr uint32 k_network_device = 1;
constexpr uint32 k_legacy_version = 1;

constexpr uint8 k_status_acknowledge = 1;
constexpr uint8 k_status_driver = 2;
constexpr uint8 k_status_driver_ok = 4;
constexpr uint8 k_status_features_ok = 8;

uint64 g_mmio_base;
uint32 g_interrupt_source;
uint8 g_status;
bool g_prepared;

volatile uint32 *mmio_register(uint64 base, uint32 offset)
{
    return reinterpret_cast<volatile uint32 *>(base + offset);
}

uint32 read_register(uint32 offset)
{
    return *mmio_register(g_mmio_base, offset);
}

void write_register(uint32 offset, uint32 value)
{
    *mmio_register(g_mmio_base, offset) = value;
}

uint32 interrupt_for_base(uint64 base)
{
    return riscv::board::k_virtio_interrupt_first +
           static_cast<uint32>(
               (base - riscv::board::k_virtio_mmio.physical_base) /
               riscv::board::k_virtio_mmio_stride);
}

bool find_network_device()
{
    const uint64 first = riscv::board::k_virtio_mmio.physical_base;
    const uint64 last = first + riscv::board::k_virtio_mmio.size -
                        riscv::board::k_virtio_mmio_stride;
    for (uint64 base = first; base <= last;
         base += riscv::board::k_virtio_mmio_stride)
    {
        if (*mmio_register(base, k_magic_value) == k_virtio_magic &&
            *mmio_register(base, k_version) == k_legacy_version &&
            *mmio_register(base, k_device_id) == k_network_device &&
            *mmio_register(base, k_vendor_id) == k_qemu_vendor)
        {
            g_mmio_base = base;
            g_interrupt_source = interrupt_for_base(base);
            return true;
        }
    }
    return false;
}

bool configure_queue(uint16 index, const QueueMemory &memory,
                     uint16 requested_size)
{
    write_register(k_queue_select, index);
    const uint32 maximum_size = read_register(k_queue_size_max);
    if (maximum_size < requested_size)
    {
        printf("virtio net: mmio queue %u too small, max=%u\n",
               index, maximum_size);
        return false;
    }

    write_register(k_queue_size, requested_size);
    write_register(k_queue_align, PGSIZE);
    write_register(k_queue_pfn,
                   static_cast<uint32>(memory.descriptor_dma / PGSIZE));
    return true;
}
} // namespace

bool prepare(const PrepareArgs &args, uint64 *negotiated_features)
{
    if (negotiated_features == nullptr || args.mac_address == nullptr ||
        args.mac_address_length == 0 || args.queue_size == 0)
    {
        return false;
    }

    g_mmio_base = 0;
    g_interrupt_source = 0;
    g_status = 0;
    g_prepared = false;
    if (!find_network_device())
    {
        printf("virtio net: mmio device not found in %p..%p\n",
               riscv::board::k_virtio_mmio.physical_base,
               riscv::board::k_virtio_mmio.physical_base +
                   riscv::board::k_virtio_mmio.size - 1);
        return false;
    }

    // QEMU 的 RISC-V virt 画像仍暴露 legacy virtio-mmio，因此这里只协商
    // 低 32 位 feature，并用 PFN 描述整块 split ring。
    write_register(k_status, 0);
    g_status = k_status_acknowledge;
    write_register(k_status, g_status);
    g_status |= k_status_driver;
    write_register(k_status, g_status);

    const uint64 features = read_register(k_device_features) &
                            args.driver_features;
    write_register(k_driver_features, static_cast<uint32>(features));
    g_status |= k_status_features_ok;
    write_register(k_status, g_status);
    if ((read_register(k_status) & k_status_features_ok) == 0)
    {
        printf("virtio net: mmio FEATURES_OK was rejected\n");
        return false;
    }

    write_register(k_guest_page_size, PGSIZE);
    if (!configure_queue(k_receive_queue_index, args.receive_queue,
                         args.queue_size) ||
        !configure_queue(k_transmit_queue_index, args.transmit_queue,
                         args.queue_size))
    {
        return false;
    }

    if ((features & (1ULL << k_feature_mac)) != 0)
    {
        volatile uint8 *config = reinterpret_cast<volatile uint8 *>(
            g_mmio_base + k_device_config);
        for (uint32 index = 0; index < args.mac_address_length; ++index)
        {
            args.mac_address[index] = config[index];
        }
    }

    *negotiated_features = features;
    g_prepared = true;
    printf("virtio net: mmio base=%p irq=%u\n", g_mmio_base,
           g_interrupt_source);
    return true;
}

void activate()
{
    if (!g_prepared)
    {
        panic("virtio net: activate mmio transport before prepare");
    }
    g_status |= k_status_driver_ok;
    write_register(k_status, g_status);
}

void notify_queue(uint16 queue_index)
{
    write_register(k_queue_notify, queue_index);
}

void acknowledge_interrupt()
{
    if (g_mmio_base == 0)
    {
        return;
    }
    const uint32 pending = read_register(k_interrupt_status);
    if (pending != 0)
    {
        write_register(k_interrupt_ack, pending);
    }
}

uint32 interrupt_source()
{
    return g_interrupt_source;
}
} // namespace net::virtio_transport
