#include "ls2k1000_ahci.hh"

#ifdef BOARD_LS2K1000

#include "hal/loongarch/platform_board.hh"
#include "libs/klib.hh"
#include "platform.hh"
#include "printer.hh"
#include "spinlock.hh"
#include "tm/time.hh"

namespace loongarch::ls2k1000::ahci
{
namespace
{
    constexpr uint32 k_sector_size = 512;
    constexpr uint32 k_bounce_sectors = 128;
    constexpr uint32 k_bounce_bytes = k_bounce_sectors * k_sector_size;
    constexpr uint64 k_hba_timeout_us = 1'000'000;
    constexpr uint64 k_link_timeout_us = 5'000'000;
    constexpr uint64 k_command_timeout_us = 5'000'000;
    constexpr uint64 k_comreset_hold_us = 1'000;

    constexpr uint64 k_hba_cap = 0x00;
    constexpr uint64 k_hba_ghc = 0x04;
    constexpr uint64 k_hba_is = 0x08;
    constexpr uint64 k_hba_pi = 0x0c;
    constexpr uint64 k_port_base = 0x100;

    constexpr uint64 k_port_clb = 0x00;
    constexpr uint64 k_port_clbu = 0x04;
    constexpr uint64 k_port_fb = 0x08;
    constexpr uint64 k_port_fbu = 0x0c;
    constexpr uint64 k_port_is = 0x10;
    constexpr uint64 k_port_ie = 0x14;
    constexpr uint64 k_port_cmd = 0x18;
    constexpr uint64 k_port_tfd = 0x20;
    constexpr uint64 k_port_sig = 0x24;
    constexpr uint64 k_port_ssts = 0x28;
    constexpr uint64 k_port_sctl = 0x2c;
    constexpr uint64 k_port_serr = 0x30;
    constexpr uint64 k_port_sact = 0x34;
    constexpr uint64 k_port_ci = 0x38;

    constexpr uint32 k_ghc_reset = 1U << 0;
    constexpr uint32 k_ghc_interrupt_enable = 1U << 1;
    constexpr uint32 k_ghc_ahci_enable = 1U << 31;
    constexpr uint32 k_cap_staggered_spinup = 1U << 27;

    constexpr uint32 k_cmd_start = 1U << 0;
    constexpr uint32 k_cmd_spinup = 1U << 1;
    constexpr uint32 k_cmd_fis_receive = 1U << 4;
    constexpr uint32 k_cmd_fis_running = 1U << 14;
    constexpr uint32 k_cmd_command_running = 1U << 15;
    constexpr uint32 k_cmd_icc_active = 1U << 28;
    constexpr uint32 k_task_busy = 1U << 7;
    constexpr uint32 k_task_drq = 1U << 3;
    constexpr uint32 k_task_error = 1U << 0;
    constexpr uint32 k_port_irq_error_mask =
        (1U << 30) | // task file error
        (1U << 29) | // host bus fatal error
        (1U << 28) | // host bus data error
        (1U << 27) | // interface fatal error
        (1U << 26) | // interface non-fatal error
        (1U << 24) | // overflow
        (1U << 23);  // incorrect port multiplier status
    constexpr uint32 k_sata_signature = 0x00000101U;

    constexpr uint8 k_fis_register_h2d = 0x27;
    constexpr uint8 k_fis_command = 1U << 7;
    constexpr uint8 k_device_lba = 1U << 6;
    constexpr uint8 k_ata_identify = 0xec;
    constexpr uint8 k_ata_read_dma = 0xc8;
    constexpr uint8 k_ata_write_dma = 0xca;
    constexpr uint8 k_ata_read_dma_ext = 0x25;
    constexpr uint8 k_ata_write_dma_ext = 0x35;

    struct CommandHeader
    {
        uint32 options;
        uint32 transferred_bytes;
        uint32 table_address_low;
        uint32 table_address_high;
        uint32 reserved[4];
    };

    struct PhysicalRegionDescriptor
    {
        uint32 address_low;
        uint32 address_high;
        uint32 reserved;
        uint32 byte_count_and_flags;
    };

    struct CommandTable
    {
        uint8 command_fis[64];
        uint8 atapi_command[16];
        uint8 reserved[48];
        PhysicalRegionDescriptor descriptor;
    };

    struct RegisterHostToDeviceFis
    {
        uint8 fis_type;
        uint8 flags;
        uint8 command;
        uint8 feature_low;
        uint8 lba_low;
        uint8 lba_mid;
        uint8 lba_high;
        uint8 device;
        uint8 lba_low_exp;
        uint8 lba_mid_exp;
        uint8 lba_high_exp;
        uint8 feature_high;
        uint8 count_low;
        uint8 count_high;
        uint8 icc;
        uint8 control;
        uint8 reserved[4];
    };

    static_assert(sizeof(CommandHeader) == 32);
    static_assert(sizeof(CommandTable) == 144);
    static_assert(sizeof(RegisterHostToDeviceFis) == 20);

    alignas(1024) uint8 g_command_list_storage[1024]
        __attribute__((section(".dma_uncached")));
    alignas(256) uint8 g_received_fis_storage[256]
        __attribute__((section(".dma_uncached")));
    alignas(128) uint8 g_command_table_storage[256]
        __attribute__((section(".dma_uncached")));
    alignas(4096) uint8 g_bounce_storage[k_bounce_bytes]
        __attribute__((section(".dma_uncached")));

    SpinLock g_lock;
    bool g_initialized = false;
    bool g_lba48 = false;
    uint64 g_capacity_sectors = 0;

    volatile uint8 *hba()
    {
        return reinterpret_cast<volatile uint8 *>(board::mmio_address(board::k_ahci_physical));
    }

    uint32 read32(uint64 offset)
    {
        return *reinterpret_cast<volatile uint32 *>(hba() + offset);
    }

    void write32(uint64 offset, uint32 value)
    {
        *reinterpret_cast<volatile uint32 *>(hba() + offset) = value;
    }

    uint64 port_offset(uint64 reg)
    {
        return k_port_base + reg;
    }

    template <typename T>
    T *dma_alias(T *pointer)
    {
        return reinterpret_cast<T *>(board::mmio_address(reinterpret_cast<uint64>(pointer)));
    }

    uint64 dma_physical(const void *pointer)
    {
        return board::physical_address(reinterpret_cast<uint64>(pointer));
    }

    bool timeout_reached(uint64 start, uint64 timeout_cycles)
    {
        return rdtime() - start >= timeout_cycles;
    }

    bool wait_clear(uint64 offset, uint32 mask, uint64 timeout_us)
    {
        const uint64 start = rdtime();
        const uint64 timeout_cycles = tmm::qemu_fre_cal_cycles(timeout_us);
        while (!timeout_reached(start, timeout_cycles))
        {
            if ((read32(offset) & mask) == 0)
            {
                return true;
            }
            asm volatile("nop");
        }
        return false;
    }

    bool wait_link_ready()
    {
        const uint64 start = rdtime();
        const uint64 timeout_cycles = tmm::qemu_fre_cal_cycles(k_link_timeout_us);
        while (!timeout_reached(start, timeout_cycles))
        {
            const uint32 status = read32(port_offset(k_port_ssts));
            const uint32 detect = status & 0xf;
            const uint32 power = (status >> 8) & 0xf;
            const uint32 task = read32(port_offset(k_port_tfd));
            if (detect == 3 && power == 1 && (task & (k_task_busy | k_task_drq)) == 0)
            {
                return true;
            }
            asm volatile("nop");
        }
        return false;
    }

    bool stop_port_engine()
    {
        uint32 command = read32(port_offset(k_port_cmd));
        write32(port_offset(k_port_cmd), command & ~k_cmd_start);
        if (!wait_clear(port_offset(k_port_cmd), k_cmd_command_running,
                        k_hba_timeout_us))
        {
            return false;
        }

        command = read32(port_offset(k_port_cmd));
        write32(port_offset(k_port_cmd), command & ~k_cmd_fis_receive);
        return wait_clear(port_offset(k_port_cmd), k_cmd_fis_running,
                          k_hba_timeout_us);
    }

    void clear_dma_areas()
    {
        memset(dma_alias(g_command_list_storage), 0, sizeof(g_command_list_storage));
        memset(dma_alias(g_received_fis_storage), 0, sizeof(g_received_fis_storage));
        memset(dma_alias(g_command_table_storage), 0, sizeof(g_command_table_storage));
        memset(dma_alias(g_bounce_storage), 0, sizeof(g_bounce_storage));
        asm volatile("dbar 0" ::: "memory");
    }

    bool dma_range_is_32bit(const void *pointer, uint64 size)
    {
        const uint64 physical = dma_physical(pointer);
        return size != 0 && physical <= 0xffffffffULL &&
               size - 1 <= 0xffffffffULL - physical;
    }

    bool program_port()
    {
        if (!stop_port_engine())
        {
            return false;
        }

        clear_dma_areas();
        if (!dma_range_is_32bit(g_command_list_storage, sizeof(g_command_list_storage)) ||
            !dma_range_is_32bit(g_received_fis_storage, sizeof(g_received_fis_storage)) ||
            !dma_range_is_32bit(g_command_table_storage, sizeof(g_command_table_storage)) ||
            !dma_range_is_32bit(g_bounce_storage, sizeof(g_bounce_storage)))
        {
            printfRed("[ahci] DMA storage is outside the LS2K1000 32-bit window\n");
            return false;
        }
        const uint64 command_list = dma_physical(g_command_list_storage);
        const uint64 received_fis = dma_physical(g_received_fis_storage);

        write32(port_offset(k_port_clb), static_cast<uint32>(command_list));
        write32(port_offset(k_port_clbu), 0);
        write32(port_offset(k_port_fb), static_cast<uint32>(received_fis));
        write32(port_offset(k_port_fbu), 0);
        write32(port_offset(k_port_ie), 0);
        write32(port_offset(k_port_is), 0xffffffffU);
        write32(port_offset(k_port_serr), 0xffffffffU);
        write32(k_hba_is, 0xffffffffU);

        uint32 command = read32(port_offset(k_port_cmd));
        command |= k_cmd_spinup | k_cmd_icc_active | k_cmd_fis_receive | k_cmd_start;
        write32(port_offset(k_port_cmd), command);
        asm volatile("dbar 0" ::: "memory");

        if (wait_link_ready())
        {
            return true;
        }

        // 固件未拉起 SATA PHY 时补一次 COMRESET，然后重新等待链路。
        uint32 control = read32(port_offset(k_port_sctl));
        write32(port_offset(k_port_sctl), (control & ~0xfU) | 1U);
        const uint64 reset_start = rdtime();
        const uint64 reset_cycles = tmm::qemu_fre_cal_cycles(k_comreset_hold_us);
        while (!timeout_reached(reset_start, reset_cycles))
        {
            asm volatile("nop");
        }
        write32(port_offset(k_port_sctl), control & ~0xfU);
        return wait_link_ready();
    }

    void prepare_command(uint8 command, uint64 lba, uint32 sectors, bool write, bool identify)
    {
        auto *headers = dma_alias(reinterpret_cast<CommandHeader *>(g_command_list_storage));
        auto *table = dma_alias(reinterpret_cast<CommandTable *>(g_command_table_storage));
        memset(headers, 0, sizeof(g_command_list_storage));
        memset(table, 0, sizeof(CommandTable));

        const uint64 table_physical = dma_physical(g_command_table_storage);
        const uint64 bounce_physical = dma_physical(g_bounce_storage);
        headers[0].options = 5U | (write ? (1U << 6) : 0U) | (1U << 16);
        headers[0].table_address_low = static_cast<uint32>(table_physical);
        headers[0].table_address_high = static_cast<uint32>(table_physical >> 32);

        table->descriptor.address_low = static_cast<uint32>(bounce_physical);
        table->descriptor.address_high = static_cast<uint32>(bounce_physical >> 32);
        table->descriptor.byte_count_and_flags =
            ((identify ? k_sector_size : sectors * k_sector_size) - 1U) | (1U << 31);

        RegisterHostToDeviceFis fis{};
        fis.fis_type = k_fis_register_h2d;
        fis.flags = k_fis_command;
        fis.command = command;
        if (!identify)
        {
            fis.lba_low = static_cast<uint8>(lba);
            fis.lba_mid = static_cast<uint8>(lba >> 8);
            fis.lba_high = static_cast<uint8>(lba >> 16);
            fis.device = k_device_lba;
            if (g_lba48)
            {
                fis.lba_low_exp = static_cast<uint8>(lba >> 24);
                fis.lba_mid_exp = static_cast<uint8>(lba >> 32);
                fis.lba_high_exp = static_cast<uint8>(lba >> 40);
                fis.count_high = static_cast<uint8>(sectors >> 8);
            }
            else
            {
                fis.device |= static_cast<uint8>((lba >> 24) & 0xf);
            }
            fis.count_low = static_cast<uint8>(sectors);
        }
        memmove(table->command_fis, &fis, sizeof(fis));
        asm volatile("dbar 0" ::: "memory");
    }

    bool issue_slot_zero(uint32 expected_bytes)
    {
        if (!wait_clear(port_offset(k_port_tfd), k_task_busy | k_task_drq,
                        k_command_timeout_us))
        {
            printfRed("[ahci] port remained busy before command: tfd=0x%x\n",
                      read32(port_offset(k_port_tfd)));
            return false;
        }

        write32(port_offset(k_port_is), 0xffffffffU);
        write32(port_offset(k_port_serr), 0xffffffffU);
        write32(port_offset(k_port_sact), 0);
        asm volatile("dbar 0" ::: "memory");
        write32(port_offset(k_port_ci), 1U);

        const uint64 start = rdtime();
        const uint64 timeout_cycles = tmm::qemu_fre_cal_cycles(k_command_timeout_us);
        while (!timeout_reached(start, timeout_cycles))
        {
            const uint32 irq_status = read32(port_offset(k_port_is));
            const uint32 task_status = read32(port_offset(k_port_tfd));
            const uint32 sata_error = read32(port_offset(k_port_serr));
            if ((irq_status & k_port_irq_error_mask) != 0 ||
                (task_status & k_task_error) != 0 || sata_error != 0)
            {
                write32(port_offset(k_port_is), irq_status);
                write32(port_offset(k_port_serr), sata_error);
                printfRed("[ahci] command error: is=0x%x tfd=0x%x serr=0x%x\n",
                          irq_status, task_status, sata_error);
                return false;
            }
            if ((read32(port_offset(k_port_ci)) & 1U) == 0)
            {
                asm volatile("dbar 0" ::: "memory");
                auto *headers = dma_alias(
                    reinterpret_cast<CommandHeader *>(g_command_list_storage));
                const uint32 transferred = headers[0].transferred_bytes;
                write32(port_offset(k_port_is), irq_status);
                if (transferred != expected_bytes)
                {
                    printfRed("[ahci] short DMA transfer: expected=%u actual=%u\n",
                              expected_bytes, transferred);
                    return false;
                }
                return true;
            }
            asm volatile("nop");
        }
        printfRed("[ahci] command timeout: ci=0x%x is=0x%x tfd=0x%x serr=0x%x\n",
                  read32(port_offset(k_port_ci)), read32(port_offset(k_port_is)),
                  read32(port_offset(k_port_tfd)), read32(port_offset(k_port_serr)));
        return false;
    }

    uint16 identify_word(const uint8 *identify, uint32 index)
    {
        return static_cast<uint16>(identify[index * 2]) |
               (static_cast<uint16>(identify[index * 2 + 1]) << 8);
    }

    bool identify_disk()
    {
        prepare_command(k_ata_identify, 0, 1, false, true);
        if (!issue_slot_zero(k_sector_size))
        {
            return false;
        }

        const uint8 *identify = dma_alias(g_bounce_storage);
        const uint16 command_set = identify_word(identify, 83);
        g_lba48 = (command_set & 0xc000U) == 0x4000U && (command_set & (1U << 10)) != 0;
        if (g_lba48)
        {
            g_capacity_sectors = static_cast<uint64>(identify_word(identify, 100)) |
                                 (static_cast<uint64>(identify_word(identify, 101)) << 16) |
                                 (static_cast<uint64>(identify_word(identify, 102)) << 32) |
                                 (static_cast<uint64>(identify_word(identify, 103)) << 48);
        }
        else
        {
            g_capacity_sectors = static_cast<uint64>(identify_word(identify, 60)) |
                                 (static_cast<uint64>(identify_word(identify, 61)) << 16);
        }
        return g_capacity_sectors != 0;
    }

    int transfer_locked(void *buffer, uint64 start_sector, uint32 sector_count, bool write)
    {
        uint8 *cursor = reinterpret_cast<uint8 *>(buffer);
        uint64 sector = start_sector;
        uint32 remaining = sector_count;
        auto *bounce = dma_alias(g_bounce_storage);

        while (remaining != 0)
        {
            const uint32 chunk = remaining > k_bounce_sectors ? k_bounce_sectors : remaining;
            const uint32 bytes = chunk * k_sector_size;
            if (write)
            {
                memmove(bounce, cursor, bytes);
                asm volatile("dbar 0" ::: "memory");
            }

            const uint8 command = g_lba48
                                      ? (write ? k_ata_write_dma_ext : k_ata_read_dma_ext)
                                      : (write ? k_ata_write_dma : k_ata_read_dma);
            prepare_command(command, sector, chunk, write, false);
            if (!issue_slot_zero(bytes))
            {
                // 停止并重新建立端口 DMA 状态，避免下一次访问复用仍由控制器
                // 持有的命令表。当前请求仍返回失败，不对写请求做隐式重放。
                if (!program_port())
                {
                    printfRed("[ahci] port recovery failed after I/O error\n");
                }
                return -1;
            }

            if (!write)
            {
                asm volatile("dbar 0" ::: "memory");
                memmove(cursor, bounce, bytes);
            }
            cursor += bytes;
            sector += chunk;
            remaining -= chunk;
        }
        return 0;
    }
} // namespace

bool initialize()
{
    g_lock.init("ls2k ahci");
    g_initialized = false;
    g_capacity_sectors = 0;

    uint32 control = read32(k_hba_ghc);
    write32(k_hba_ghc, control | k_ghc_ahci_enable | k_ghc_reset);
    if (!wait_clear(k_hba_ghc, k_ghc_reset, k_hba_timeout_us))
    {
        printfRed("[ahci] HBA reset timeout\n");
        return false;
    }
    write32(k_hba_ghc, (read32(k_hba_ghc) | k_ghc_ahci_enable) & ~k_ghc_interrupt_enable);

    // LS2K1000 固件可能把 PI/CAP.SSS 留空；采用 StarryOS 的 LS2K profile 修复。
    uint32 capabilities = read32(k_hba_cap);
    if ((capabilities & k_cap_staggered_spinup) == 0)
    {
        write32(k_hba_cap, capabilities | k_cap_staggered_spinup);
    }
    uint32 ports = read32(k_hba_pi);
    if (ports == 0)
    {
        ports = board::k_ahci_port_fallback;
        write32(k_hba_pi, ports);
    }
    if ((ports & 1U) == 0)
    {
        printfRed("[ahci] LS2K1000 port0 is not implemented: PI=0x%x\n", ports);
        return false;
    }

    if (!program_port())
    {
        printfRed("[ahci] SATA port0 link/engine initialization failed\n");
        return false;
    }
    const uint32 signature = read32(port_offset(k_port_sig));
    if (signature != 0 && signature != k_sata_signature)
    {
        printfRed("[ahci] unsupported SATA signature=0x%x\n", signature);
        return false;
    }
    if (!identify_disk())
    {
        printfRed("[ahci] IDENTIFY DEVICE failed\n");
        return false;
    }

    g_initialized = true;
    printfGreen("[ahci] SATA disk ready: sectors=%lu lba48=%d\n",
                g_capacity_sectors, static_cast<int>(g_lba48));
    return true;
}

int transfer(void *buffer, uint64 start_sector, uint32 sector_count, bool write)
{
    if (!g_initialized || buffer == nullptr || sector_count == 0)
    {
        return sector_count == 0 ? 0 : -1;
    }
    if (start_sector >= g_capacity_sectors ||
        static_cast<uint64>(sector_count) > g_capacity_sectors - start_sector)
    {
        return -1;
    }
    if (!g_lba48 && (start_sector >= (1ULL << 28) ||
                     static_cast<uint64>(sector_count) > (1ULL << 28) - start_sector))
    {
        return -1;
    }

    g_lock.acquire();
    const int result = transfer_locked(buffer, start_sector, sector_count, write);
    g_lock.release();
    return result;
}

uint64 capacity_bytes()
{
    return g_capacity_sectors * static_cast<uint64>(k_sector_size);
}
} // namespace loongarch::ls2k1000::ahci

#endif
