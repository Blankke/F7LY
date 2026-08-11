#include "jh7110_dwmmc.hh"

#include "hal/riscv/platform_board.hh"
#include "platform/block_backend.hh"
#include "platform/memory.hh"
#include "printer.hh"
#include "spinlock.hh"
#include "tm/time.hh"

namespace riscv::jh7110::dwmmc
{
namespace
{
    constexpr uint32 k_sector_size = platform::block_backend::k_sector_size_bytes;
    constexpr uint32 k_words_per_sector = k_sector_size / sizeof(uint32);
    static_assert(k_sector_size == 512);
    static_assert(k_words_per_sector == 128);

    // Synopsys DesignWare Mobile Storage Host Controller 寄存器偏移。
    // 所有响应槽和 FIFO 都必须使用独立的 32 位 MMIO 访问；JH7110 不接受
    // 编译器把相邻响应槽合并为 64 位 load/store。
    constexpr uint64 k_ctrl = 0x000;
    constexpr uint64 k_power_enable = 0x004;
    constexpr uint64 k_clock_divider = 0x008;
    constexpr uint64 k_clock_source = 0x00c;
    constexpr uint64 k_clock_enable = 0x010;
    constexpr uint64 k_timeout = 0x014;
    constexpr uint64 k_card_type = 0x018;
    constexpr uint64 k_block_size = 0x01c;
    constexpr uint64 k_byte_count = 0x020;
    constexpr uint64 k_interrupt_mask = 0x024;
    constexpr uint64 k_command_argument = 0x028;
    constexpr uint64 k_command = 0x02c;
    constexpr uint64 k_response0 = 0x030;
    constexpr uint64 k_raw_interrupt_status = 0x044;
    constexpr uint64 k_status = 0x048;
    constexpr uint64 k_fifo_threshold = 0x04c;
    constexpr uint64 k_card_detect = 0x050;
    constexpr uint64 k_version_id = 0x06c;
    constexpr uint64 k_hardware_config = 0x070;
    constexpr uint64 k_bus_mode = 0x080;
    constexpr uint64 k_fifo = 0x200;

    constexpr uint32 k_ctrl_reset = 1U << 0;
    constexpr uint32 k_ctrl_fifo_reset = 1U << 1;
    constexpr uint32 k_ctrl_dma_reset = 1U << 2;
    constexpr uint32 k_bus_mode_software_reset = 1U << 0;

    constexpr uint32 k_command_start = 1U << 31;
    constexpr uint32 k_command_use_hold_register = 1U << 29;
    constexpr uint32 k_command_update_clock = 1U << 21;
    constexpr uint32 k_command_send_initialization = 1U << 15;
    constexpr uint32 k_command_wait_previous_data = 1U << 13;
    constexpr uint32 k_command_write = 1U << 10;
    constexpr uint32 k_command_data_expected = 1U << 9;
    constexpr uint32 k_command_check_response_crc = 1U << 8;
    constexpr uint32 k_command_long_response = 1U << 7;
    constexpr uint32 k_command_response_expected = 1U << 6;

    constexpr uint32 k_interrupt_response_error = 1U << 1;
    constexpr uint32 k_interrupt_command_done = 1U << 2;
    constexpr uint32 k_interrupt_data_over = 1U << 3;
    constexpr uint32 k_interrupt_tx_ready = 1U << 4;
    constexpr uint32 k_interrupt_rx_ready = 1U << 5;
    constexpr uint32 k_interrupt_response_crc = 1U << 6;
    constexpr uint32 k_interrupt_data_crc = 1U << 7;
    constexpr uint32 k_interrupt_response_timeout = 1U << 8;
    constexpr uint32 k_interrupt_data_timeout = 1U << 9;
    constexpr uint32 k_interrupt_host_timeout = 1U << 10;
    constexpr uint32 k_interrupt_fifo_error = 1U << 11;
    constexpr uint32 k_interrupt_hardware_locked = 1U << 12;
    constexpr uint32 k_interrupt_start_bit = 1U << 13;
    constexpr uint32 k_interrupt_end_bit = 1U << 15;

    constexpr uint32 k_command_error_mask =
        k_interrupt_response_error | k_interrupt_response_crc |
        k_interrupt_response_timeout | k_interrupt_hardware_locked |
        k_interrupt_start_bit | k_interrupt_end_bit;
    constexpr uint32 k_data_error_mask =
        k_interrupt_data_crc | k_interrupt_data_timeout |
        k_interrupt_host_timeout | k_interrupt_fifo_error |
        k_interrupt_start_bit | k_interrupt_end_bit;
    constexpr uint32 k_all_error_mask = k_command_error_mask | k_data_error_mask;

    constexpr uint32 k_status_fifo_full = 1U << 3;
    constexpr uint32 k_status_data_busy = 1U << 9;
    constexpr uint32 k_status_fifo_count_shift = 17;
    constexpr uint32 k_status_fifo_count_mask = 0x1fffU;

    // StarryOS 的 JH7110 profile 与旧 visionfive2 实机代码都确认该集成是
    // 32-word、32-bit FIFO。基准阈值 0x200f0010 与 Linux DWMMC 策略一致。
    constexpr uint32 k_fifo_depth_words = 32;
    constexpr uint32 k_fifo_threshold_value =
        (2U << 28) | ((k_fifo_depth_words / 2U - 1U) << 16) |
        (k_fifo_depth_words / 2U);

    constexpr uint64 k_reset_timeout_us = 1'000'000;
    constexpr uint64 k_command_timeout_us = 1'000'000;
    constexpr uint64 k_data_timeout_us = 5'000'000;
    constexpr uint64 k_card_busy_timeout_us = 5'000'000;
    constexpr uint64 k_power_up_timeout_us = 1'000'000;
    constexpr uint64 k_power_up_poll_delay_us = 1'000;
    constexpr uint32 k_identification_clock_hz = 400'000;
    constexpr uint32 k_transfer_clock_hz = 25'000'000;

    constexpr uint32 k_cmd_go_idle = 0;
    constexpr uint32 k_cmd_all_send_cid = 2;
    constexpr uint32 k_cmd_send_relative_address = 3;
    constexpr uint32 k_cmd_send_interface_condition = 8;
    constexpr uint32 k_cmd_send_csd = 9;
    constexpr uint32 k_cmd_send_status = 13;
    constexpr uint32 k_cmd_select_card = 7;
    constexpr uint32 k_cmd_set_block_length = 16;
    constexpr uint32 k_cmd_read_single_block = 17;
    constexpr uint32 k_cmd_write_single_block = 24;
    constexpr uint32 k_cmd_application = 55;
    constexpr uint32 k_acmd_send_operating_condition = 41;

    constexpr uint32 k_ocr_powered_up = 1U << 31;
    constexpr uint32 k_ocr_high_capacity = 1U << 30;
    constexpr uint32 k_ocr_voltage_window = 0x00ff8000U;
    // 与 Linux MMC 核心的 R1_STATUS 保持一致。除高位通用错误外，
    // CID/CSD_OVERWRITE、WP_ERASE_SKIP 和 ERASE_RESET 也属于必须上报的
    // 卡状态；READY_FOR_DATA 与 CURRENT_STATE 不在此掩码内。
    constexpr uint32 k_r1_error_mask = 0xfff9a000U;
    constexpr uint32 k_r6_error_mask = 0x0000e000U;

    enum class ResponseType
    {
        none,
        r1,
        r1_busy,
        r2,
        r3,
        r6,
        r7,
    };

    enum class CommandResult
    {
        success,
        response_timeout,
        error,
    };

    struct Response
    {
        uint32 words[4];
    };

    volatile uint8 *g_registers = nullptr;
    SpinLock g_lock;
    bool g_initialized = false;
    bool g_high_capacity = false;
    uint16 g_relative_card_address = 0;
    uint64 g_capacity_sectors = 0;

    volatile uint32 *register32(uint64 offset)
    {
        return reinterpret_cast<volatile uint32 *>(g_registers + offset);
    }

    uint32 read32(uint64 offset)
    {
        return *register32(offset);
    }

    void write32(uint64 offset, uint32 value)
    {
        *register32(offset) = value;
    }

    void io_fence()
    {
        asm volatile("fence iorw, iorw" ::: "memory");
    }

    uint64 current_ticks()
    {
        return platform::clock_backend::read_ticks();
    }

    bool timeout_reached(uint64 start, uint64 timeout_cycles)
    {
        return current_ticks() - start >= timeout_cycles;
    }

    template <typename Predicate>
    bool wait_until(Predicate predicate, uint64 timeout_us)
    {
        const uint64 start = current_ticks();
        const uint64 timeout_cycles = tmm::microseconds_to_cycles(timeout_us);
        do
        {
            if (predicate())
            {
                return true;
            }
            asm volatile("nop");
        } while (!timeout_reached(start, timeout_cycles));
        return predicate();
    }

    void delay_us(uint64 duration_us)
    {
        const uint64 start = current_ticks();
        const uint64 duration_cycles = tmm::microseconds_to_cycles(duration_us);
        while (!timeout_reached(start, duration_cycles))
        {
            asm volatile("nop");
        }
    }

    uint32 fifo_word_count()
    {
        return (read32(k_status) >> k_status_fifo_count_shift) &
               k_status_fifo_count_mask;
    }

    void log_controller_error(const char *stage, uint32 command_index,
                              uint32 argument)
    {
        platformDiagnosticError(
            "[dwmmc] %s cmd=%u arg=0x%x cmdreg=0x%x status=0x%x "
            "rintsts=0x%x ctrl=0x%x cdetect=0x%x\n",
            stage, command_index, argument, read32(k_command), read32(k_status),
            read32(k_raw_interrupt_status), read32(k_ctrl), read32(k_card_detect));
    }

    bool reset_fifo()
    {
        write32(k_ctrl, read32(k_ctrl) | k_ctrl_fifo_reset);
        io_fence();
        return wait_until(
            [] { return (read32(k_ctrl) & k_ctrl_fifo_reset) == 0; },
            k_reset_timeout_us);
    }

    void clear_interrupts()
    {
        write32(k_raw_interrupt_status, 0xffffffffU);
        io_fence();
    }

    bool wait_data_idle(uint64 timeout_us)
    {
        return wait_until(
            [] { return (read32(k_status) & k_status_data_busy) == 0; },
            timeout_us);
    }

    bool update_clock()
    {
        clear_interrupts();
        write32(k_command_argument, 0);
        write32(k_command,
                k_command_start | k_command_update_clock |
                    k_command_wait_previous_data);
        io_fence();

        if (!wait_until(
                [] { return (read32(k_command) & k_command_start) == 0; },
                k_command_timeout_us))
        {
            return false;
        }
        const uint32 raw = read32(k_raw_interrupt_status);
        if ((raw & k_interrupt_hardware_locked) != 0)
        {
            write32(k_raw_interrupt_status, raw);
            return false;
        }
        return true;
    }

    bool set_clock(uint32 target_hz)
    {
        const uint64 reference_hz = board::k_dw_mmc_reference_clock_hz;
        if (target_hz == 0 || reference_hz == 0)
        {
            return false;
        }

        // DWMMC 输出频率为 reference/(2*divider)。向上取整 divider 可保证
        // 初始化时钟不超过 SD 规范要求的 400 kHz。
        uint64 divider =
            (reference_hz + 2ULL * target_hz - 1ULL) / (2ULL * target_hz);
        if (divider == 0)
        {
            divider = 1;
        }
        if (divider > 0xff)
        {
            return false;
        }

        write32(k_clock_enable, 0);
        if (!update_clock())
        {
            return false;
        }
        write32(k_clock_source, 0);
        write32(k_clock_divider, static_cast<uint32>(divider));
        if (!update_clock())
        {
            return false;
        }
        write32(k_clock_enable, 1);
        return update_clock();
    }

    uint32 response_flags(ResponseType type)
    {
        switch (type)
        {
        case ResponseType::none:
            return 0;
        case ResponseType::r2:
            return k_command_response_expected | k_command_long_response |
                   k_command_check_response_crc;
        case ResponseType::r3:
            // OCR 的 R3 没有有效 CRC，设置 CRC 检查会把每次 ACMD41 判错。
            return k_command_response_expected;
        case ResponseType::r1:
        case ResponseType::r1_busy:
        case ResponseType::r6:
        case ResponseType::r7:
            return k_command_response_expected | k_command_check_response_crc;
        }
        return 0;
    }

    bool is_r1(ResponseType type)
    {
        return type == ResponseType::r1 || type == ResponseType::r1_busy;
    }

    CommandResult send_command(uint32 index, uint32 argument, ResponseType type,
                               bool data_expected, bool write,
                               Response *response = nullptr)
    {
        if (data_expected && !wait_data_idle(k_data_timeout_us))
        {
            return CommandResult::error;
        }

        clear_interrupts();
        write32(k_command_argument, argument);
        uint32 command = k_command_start | k_command_use_hold_register |
                         k_command_wait_previous_data | response_flags(type) |
                         (index & 0x3fU);
        if (index == k_cmd_go_idle)
        {
            // 让控制器在 CMD0 前发送至少 80 个初始化时钟。
            command |= k_command_send_initialization;
        }
        if (data_expected)
        {
            command |= k_command_data_expected;
            if (write)
            {
                command |= k_command_write;
            }
        }
        write32(k_command, command);
        io_fence();

        if (!wait_until(
                [] { return (read32(k_command) & k_command_start) == 0; },
                k_command_timeout_us))
        {
            return CommandResult::error;
        }

        uint32 raw = 0;
        const bool completed = wait_until(
            [&raw] {
                raw = read32(k_raw_interrupt_status);
                return (raw & (k_interrupt_command_done | k_all_error_mask)) != 0;
            },
            k_command_timeout_us);
        if (!completed)
        {
            return CommandResult::error;
        }

        const uint32 observed = raw & (k_interrupt_command_done | k_all_error_mask);
        write32(k_raw_interrupt_status, observed);
        io_fence();
        if ((raw & k_all_error_mask) != 0)
        {
            const uint32 errors_without_response_timeout =
                raw & (k_all_error_mask & ~k_interrupt_response_timeout);
            return errors_without_response_timeout == 0
                       ? CommandResult::response_timeout
                       : CommandResult::error;
        }
        if ((raw & k_interrupt_command_done) == 0)
        {
            return CommandResult::error;
        }

        Response local{};
        // 保持四次独立的 volatile uint32 读取，不能改成聚合结构读取。
        for (uint32 slot = 0; slot < 4; ++slot)
        {
            local.words[slot] = read32(k_response0 + slot * sizeof(uint32));
        }
        if (response != nullptr)
        {
            *response = local;
        }
        if (is_r1(type) && (local.words[0] & k_r1_error_mask) != 0)
        {
            return CommandResult::error;
        }
        if (type == ResponseType::r6 &&
            (local.words[0] & k_r6_error_mask) != 0)
        {
            return CommandResult::error;
        }
        if (type == ResponseType::r1_busy &&
            !wait_data_idle(k_card_busy_timeout_us))
        {
            return CommandResult::error;
        }
        return CommandResult::success;
    }

    bool command_ok(uint32 index, uint32 argument, ResponseType type,
                    Response *response = nullptr)
    {
        const CommandResult result =
            send_command(index, argument, type, false, false, response);
        if (result == CommandResult::success)
        {
            return true;
        }
        log_controller_error("command failed", index, argument);
        return false;
    }

    bool card_ready_for_transfer()
    {
        Response response{};
        const uint32 argument =
            static_cast<uint32>(g_relative_card_address) << 16;
        if (!command_ok(k_cmd_send_status, argument, ResponseType::r1,
                        &response))
        {
            return false;
        }

        constexpr uint32 k_ready_for_data = 1U << 8;
        constexpr uint32 k_state_shift = 9;
        constexpr uint32 k_state_mask = 0x0fU;
        constexpr uint32 k_transfer_state = 4;
        const uint32 state =
            (response.words[0] >> k_state_shift) & k_state_mask;
        if ((response.words[0] & k_ready_for_data) == 0 ||
            state != k_transfer_state)
        {
            log_controller_error("card did not return to transfer state",
                                 k_cmd_send_status, argument);
            return false;
        }
        return true;
    }

    void response_r2_bytes(const Response &response, uint8 output[16])
    {
        // DWMMC 把总线上的 R2 高位放在 RESP3。协议解析采用 MSB-first，
        // 因此按 RESP3..RESP0、每个 word 大端拆成 16 字节。
        for (uint32 output_word = 0; output_word < 4; ++output_word)
        {
            const uint32 value = response.words[3U - output_word];
            output[output_word * 4U + 0U] = static_cast<uint8>(value >> 24);
            output[output_word * 4U + 1U] = static_cast<uint8>(value >> 16);
            output[output_word * 4U + 2U] = static_cast<uint8>(value >> 8);
            output[output_word * 4U + 3U] = static_cast<uint8>(value);
        }
    }

    uint64 capacity_from_csd(const Response &response)
    {
        uint8 csd[16]{};
        response_r2_bytes(response, csd);
        const uint32 version = (csd[0] >> 6) & 0x3U;
        if (version == 1)
        {
            // CSD v2：C_SIZE[69:48]，容量扇区数=(C_SIZE+1)*1024。
            const uint32 c_size =
                (static_cast<uint32>(csd[7] & 0x3fU) << 16) |
                (static_cast<uint32>(csd[8]) << 8) |
                static_cast<uint32>(csd[9]);
            return (static_cast<uint64>(c_size) + 1ULL) * 1024ULL;
        }
        if (version == 0)
        {
            // CSD v1：容量=(C_SIZE+1)*2^(C_SIZE_MULT+2)*2^READ_BL_LEN。
            const uint32 read_block_length = csd[5] & 0x0fU;
            const uint32 c_size =
                (static_cast<uint32>(csd[6] & 0x03U) << 10) |
                (static_cast<uint32>(csd[7]) << 2) |
                (static_cast<uint32>(csd[8]) >> 6);
            const uint32 c_size_multiplier =
                (static_cast<uint32>(csd[9] & 0x03U) << 1) |
                (static_cast<uint32>(csd[10]) >> 7);
            const uint64 bytes =
                (static_cast<uint64>(c_size) + 1ULL) *
                (1ULL << (c_size_multiplier + 2U)) *
                (1ULL << read_block_length);
            return bytes / k_sector_size;
        }

        // SDUC 使用 CSD v3，地址/容量契约不同；在明确实现前不能猜容量。
        return 0;
    }

    bool initialize_card()
    {
        if (!command_ok(k_cmd_go_idle, 0, ResponseType::none))
        {
            return false;
        }

        Response response{};
        const CommandResult interface_result =
            send_command(k_cmd_send_interface_condition, 0x1aa,
                         ResponseType::r7, false, false, &response);
        bool sd_v2 = false;
        if (interface_result == CommandResult::success)
        {
            sd_v2 = (response.words[0] & 0xfffU) == 0x1aaU;
            if (!sd_v2)
            {
                log_controller_error("CMD8 echo mismatch",
                                     k_cmd_send_interface_condition, 0x1aa);
                return false;
            }
        }
        else if (interface_result != CommandResult::response_timeout)
        {
            log_controller_error("CMD8 failed", k_cmd_send_interface_condition,
                                 0x1aa);
            return false;
        }

        const uint64 power_start = current_ticks();
        const uint64 power_timeout_cycles =
            tmm::microseconds_to_cycles(k_power_up_timeout_us);
        uint32 ocr = 0;
        do
        {
            if (!command_ok(k_cmd_application, 0, ResponseType::r1))
            {
                return false;
            }
            const uint32 argument =
                (sd_v2 ? k_ocr_high_capacity : 0U) | k_ocr_voltage_window;
            if (!command_ok(k_acmd_send_operating_condition, argument,
                            ResponseType::r3, &response))
            {
                return false;
            }
            ocr = response.words[0];
            if ((ocr & k_ocr_powered_up) != 0)
            {
                break;
            }
            delay_us(k_power_up_poll_delay_us);
        } while (!timeout_reached(power_start, power_timeout_cycles));

        if ((ocr & k_ocr_powered_up) == 0 ||
            (ocr & k_ocr_voltage_window) == 0)
        {
            log_controller_error("ACMD41 power-up timeout",
                                 k_acmd_send_operating_condition, ocr);
            return false;
        }
        g_high_capacity = (ocr & k_ocr_high_capacity) != 0;

        // CMD2 是从 ready 进入 identification 状态所必需的广播步骤；CID
        // 本身不影响块设备容量，因此这里只验证命令成功。
        if (!command_ok(k_cmd_all_send_cid, 0, ResponseType::r2))
        {
            return false;
        }
        if (!command_ok(k_cmd_send_relative_address, 0, ResponseType::r6,
                        &response))
        {
            return false;
        }
        g_relative_card_address = static_cast<uint16>(response.words[0] >> 16);
        if (g_relative_card_address == 0)
        {
            log_controller_error("card returned zero RCA",
                                 k_cmd_send_relative_address, 0);
            return false;
        }

        const uint32 card_argument =
            static_cast<uint32>(g_relative_card_address) << 16;
        if (!command_ok(k_cmd_send_csd, card_argument, ResponseType::r2,
                        &response))
        {
            return false;
        }
        g_capacity_sectors = capacity_from_csd(response);
        if (g_capacity_sectors == 0)
        {
            log_controller_error("unsupported or invalid CSD", k_cmd_send_csd,
                                 card_argument);
            return false;
        }

        // OCR 的 CCS 与 CSD 结构必须相互吻合，否则 LBA/byte 地址选择会错写卡。
        uint8 csd[16]{};
        response_r2_bytes(response, csd);
        const uint32 csd_version = (csd[0] >> 6) & 0x3U;
        if ((g_high_capacity && csd_version != 1) ||
            (!g_high_capacity && csd_version != 0))
        {
            log_controller_error("OCR/CSD capacity mode mismatch", k_cmd_send_csd,
                                 card_argument);
            return false;
        }

        if (!command_ok(k_cmd_select_card, card_argument,
                        ResponseType::r1_busy))
        {
            return false;
        }
        if (!card_ready_for_transfer())
        {
            return false;
        }
        // SDHC/SDXC 固定使用 512 字节扇区，CMD16 对它们可能返回非法命令。
        if (!g_high_capacity &&
            !command_ok(k_cmd_set_block_length, k_sector_size, ResponseType::r1))
        {
            return false;
        }
        write32(k_block_size, k_sector_size);
        return true;
    }

    bool initialize_controller()
    {
        write32(k_bus_mode, k_bus_mode_software_reset);
        if (!wait_until(
                [] { return (read32(k_bus_mode) & k_bus_mode_software_reset) == 0; },
                k_reset_timeout_us))
        {
            log_controller_error("bus-mode reset timeout", 0, 0);
            return false;
        }

        write32(k_ctrl, k_ctrl_reset | k_ctrl_fifo_reset | k_ctrl_dma_reset);
        if (!wait_until(
                [] {
                    return (read32(k_ctrl) &
                            (k_ctrl_reset | k_ctrl_fifo_reset | k_ctrl_dma_reset)) == 0;
                },
                k_reset_timeout_us))
        {
            log_controller_error("controller reset timeout", 0, 0);
            return false;
        }

        write32(k_power_enable, 1);
        io_fence();
        if ((read32(k_power_enable) & 1U) == 0)
        {
            platformDiagnosticError("[dwmmc] slot power-enable did not latch\n");
            return false;
        }
        write32(k_interrupt_mask, 0); // 本实现明确采用轮询，不注册不存在的 IRQ 路径。
        clear_interrupts();
        write32(k_timeout, 0xffffffffU);
        write32(k_card_type, 0); // 先保持最保守且已验证的 1-bit 总线。
        write32(k_block_size, k_sector_size);
        write32(k_fifo_threshold, k_fifo_threshold_value);
        write32(k_clock_enable, 0);
        write32(k_clock_source, 0);

        // VisionFive 2 的卡检测连接在独立 GPIO41（active-low），不是
        // DWMMC 的 CDETECT 输入。首版不实现 GPIO 热插拔，是否插卡由后续
        // CMD0/CMD8/ACMD41 的有界协议探测决定；这里不能因 CDETECT 浮空而
        // 把一张正常 SD 卡误判为不存在。
        if (!set_clock(k_identification_clock_hz))
        {
            log_controller_error("identification clock setup failed", 0, 0);
            return false;
        }
        delay_us(k_power_up_poll_delay_us);
        return true;
    }

    void recover_data_path()
    {
        // 请求已经失败，绝不自动重放写命令。这里只清除 FIFO/状态，避免下次
        // 请求复用控制器遗留数据；恢复失败仍由本次 I/O 返回错误。
        (void)wait_data_idle(k_card_busy_timeout_us);
        if (!reset_fifo())
        {
            platformDiagnosticError("[dwmmc] FIFO recovery reset timed out\n");
        }
        clear_interrupts();
    }

    bool wait_data_completion(uint32 command_index, uint32 argument)
    {
        uint32 raw = 0;
        const bool completed = wait_until(
            [&raw] {
                raw = read32(k_raw_interrupt_status);
                return (raw & (k_interrupt_data_over | k_data_error_mask)) != 0;
            },
            k_data_timeout_us);
        if (!completed || (raw & k_data_error_mask) != 0)
        {
            log_controller_error(completed ? "data transfer error" :
                                             "data transfer timeout",
                                 command_index, argument);
            return false;
        }
        write32(k_raw_interrupt_status,
                raw & (k_interrupt_data_over | k_interrupt_rx_ready |
                       k_interrupt_tx_ready | k_data_error_mask));
        return true;
    }

    bool read_sector(uint8 *destination, uint32 argument)
    {
        if (!reset_fifo())
        {
            log_controller_error("FIFO reset before read failed",
                                 k_cmd_read_single_block, argument);
            return false;
        }
        write32(k_block_size, k_sector_size);
        write32(k_byte_count, k_sector_size);
        const CommandResult command_result =
            send_command(k_cmd_read_single_block, argument, ResponseType::r1,
                         true, false);
        if (command_result != CommandResult::success)
        {
            log_controller_error("CMD17 failed", k_cmd_read_single_block,
                                 argument);
            recover_data_path();
            return false;
        }

        uint32 words_read = 0;
        const uint64 start = current_ticks();
        const uint64 timeout_cycles = tmm::microseconds_to_cycles(k_data_timeout_us);
        while (words_read < k_words_per_sector &&
               !timeout_reached(start, timeout_cycles))
        {
            const uint32 raw = read32(k_raw_interrupt_status);
            if ((raw & k_data_error_mask) != 0)
            {
                log_controller_error("read FIFO error", k_cmd_read_single_block,
                                     argument);
                recover_data_path();
                return false;
            }

            uint32 available = fifo_word_count();
            if (available > k_words_per_sector - words_read)
            {
                available = k_words_per_sector - words_read;
            }
            for (uint32 word_index = 0; word_index < available; ++word_index)
            {
                const uint32 value = read32(k_fifo);
                const uint32 byte_offset = words_read * sizeof(uint32);
                destination[byte_offset + 0] = static_cast<uint8>(value);
                destination[byte_offset + 1] = static_cast<uint8>(value >> 8);
                destination[byte_offset + 2] = static_cast<uint8>(value >> 16);
                destination[byte_offset + 3] = static_cast<uint8>(value >> 24);
                ++words_read;
            }
            if ((raw & k_interrupt_rx_ready) != 0)
            {
                write32(k_raw_interrupt_status, k_interrupt_rx_ready);
            }
            if (available == 0 && (raw & k_interrupt_data_over) != 0)
            {
                break;
            }
        }
        io_fence();

        // 每个 CMD17 必须恰好消费 128 个 32 位 FIFO word。少读也不能把
        // 下一扇区的数据当成本扇区尾部继续拼接。
        if (words_read != k_words_per_sector ||
            !wait_data_completion(k_cmd_read_single_block, argument) ||
            !wait_data_idle(k_card_busy_timeout_us))
        {
            log_controller_error("short/incomplete CMD17",
                                 k_cmd_read_single_block, argument);
            recover_data_path();
            return false;
        }
        return true;
    }

    bool write_sector(const uint8 *source, uint32 argument)
    {
        if (!reset_fifo())
        {
            log_controller_error("FIFO reset before write failed",
                                 k_cmd_write_single_block, argument);
            return false;
        }
        write32(k_block_size, k_sector_size);
        write32(k_byte_count, k_sector_size);
        const CommandResult command_result =
            send_command(k_cmd_write_single_block, argument, ResponseType::r1,
                         true, true);
        if (command_result != CommandResult::success)
        {
            log_controller_error("CMD24 failed", k_cmd_write_single_block,
                                 argument);
            recover_data_path();
            return false;
        }

        uint32 raw = 0;
        if (!wait_until(
                [&raw] {
                    raw = read32(k_raw_interrupt_status);
                    return (raw & (k_interrupt_tx_ready | k_interrupt_data_over |
                                   k_data_error_mask)) != 0;
                },
                k_data_timeout_us) ||
            (raw & (k_interrupt_data_over | k_data_error_mask)) != 0)
        {
            log_controller_error("TX FIFO did not become ready",
                                 k_cmd_write_single_block, argument);
            recover_data_path();
            return false;
        }

        uint32 words_written = 0;
        const uint64 start = current_ticks();
        const uint64 timeout_cycles = tmm::microseconds_to_cycles(k_data_timeout_us);
        while (words_written < k_words_per_sector &&
               !timeout_reached(start, timeout_cycles))
        {
            raw = read32(k_raw_interrupt_status);
            if ((raw & (k_data_error_mask | k_interrupt_data_over)) != 0)
            {
                log_controller_error("premature write completion/error",
                                     k_cmd_write_single_block, argument);
                recover_data_path();
                return false;
            }
            if ((read32(k_status) & k_status_fifo_full) != 0)
            {
                asm volatile("nop");
                continue;
            }

            const uint32 byte_offset = words_written * sizeof(uint32);
            const uint32 value =
                static_cast<uint32>(source[byte_offset + 0]) |
                (static_cast<uint32>(source[byte_offset + 1]) << 8) |
                (static_cast<uint32>(source[byte_offset + 2]) << 16) |
                (static_cast<uint32>(source[byte_offset + 3]) << 24);
            write32(k_fifo, value);
            ++words_written;
            if ((raw & k_interrupt_tx_ready) != 0)
            {
                write32(k_raw_interrupt_status, k_interrupt_tx_ready);
            }
        }
        // 先确保最后一个 FIFO word 已对控制器可见，再观察 DATA_OVER。
        io_fence();

        // 每个 CMD24 必须恰好发布 128 个 word；不足时立即失败，不能补读
        // 相邻内存，也不能用下一扇区数据填满当前 FIFO。
        if (words_written != k_words_per_sector ||
            !wait_data_completion(k_cmd_write_single_block, argument) ||
            !wait_data_idle(k_card_busy_timeout_us) ||
            !card_ready_for_transfer())
        {
            log_controller_error("short/incomplete CMD24",
                                 k_cmd_write_single_block, argument);
            recover_data_path();
            return false;
        }
        return true;
    }

    bool sector_argument(uint64 sector, uint32 &argument)
    {
        if (g_high_capacity)
        {
            if (sector > 0xffffffffULL)
            {
                return false;
            }
            argument = static_cast<uint32>(sector);
            return true;
        }

        // 先检查再乘，不能依赖 unsigned 溢出后的值做地址边界判断。
        if (sector > 0xffffffffULL / k_sector_size)
        {
            return false;
        }
        argument = static_cast<uint32>(sector * k_sector_size);
        return true;
    }

    int transfer_locked(void *buffer, uint64 start_sector, uint32 sector_count,
                        bool write)
    {
        auto *cursor = reinterpret_cast<uint8 *>(buffer);
        for (uint32 offset = 0; offset < sector_count; ++offset)
        {
            uint32 argument = 0;
            if (!sector_argument(start_sector + offset, argument))
            {
                return -1;
            }
            const bool ok = write ? write_sector(cursor, argument)
                                  : read_sector(cursor, argument);
            if (!ok)
            {
                return -1;
            }
            // 旧分支多扇区路径曾一直复用 buffer[0]。每完成一个严格的
            // CMD17/CMD24 后推进 512 字节，保证第 N 个扇区对应第 N 段缓冲区。
            cursor += k_sector_size;
        }
        return 0;
    }
} // namespace

bool initialize()
{
    g_lock.init("jh7110 dwmmc");
    g_initialized = false;
    g_high_capacity = false;
    g_relative_card_address = 0;
    g_capacity_sectors = 0;

    // DTS 的 assigned-clock-rates=50MHz 是操作系统必须落实的配置目标，
    // 不能假设 U-Boot 恰好把 SDIO1 留在该频率。先由板级层打开 AHB/CIU
    // 时钟并释放 reset，随后本驱动才能用 50MHz 作为 DWMMC 分频基准。
    if (!board::prepare_dw_mmc_hardware())
    {
        platformDiagnosticError("[dwmmc] JH7110 SDIO1 clock/reset setup failed\n");
        return false;
    }

    if (board::k_dw_mmc_mmio.size < k_fifo + sizeof(uint32))
    {
        platformDiagnosticError("[dwmmc] MMIO region is too small: size=0x%lx\n",
                                board::k_dw_mmc_mmio.size);
        return false;
    }
    g_registers = reinterpret_cast<volatile uint8 *>(
        platform::memory::kernel_access_address(
            board::k_dw_mmc_mmio.physical_base));

    platformDiagnosticInfo(
        "[dwmmc] input: physical=0x%lx mmio=0x%lx ref_clock=%luHz "
        "verid=0x%x hcon=0x%x\n",
        board::k_dw_mmc_mmio.physical_base,
        reinterpret_cast<uint64>(g_registers),
        board::k_dw_mmc_reference_clock_hz, read32(k_version_id),
        read32(k_hardware_config));

    if (!initialize_controller() || !initialize_card())
    {
        return false;
    }
    if (!set_clock(k_transfer_clock_hz))
    {
        log_controller_error("transfer clock setup failed", 0, 0);
        return false;
    }

    g_initialized = true;
    platformDiagnosticInfo(
        "[dwmmc] output: card ready rca=0x%x high_capacity=%d "
        "sectors=%lu capacity=%lu bytes clock=%uHz\n",
        static_cast<uint32>(g_relative_card_address),
        static_cast<int>(g_high_capacity), g_capacity_sectors,
        g_capacity_sectors * static_cast<uint64>(k_sector_size),
        k_transfer_clock_hz);
    return true;
}

int transfer(void *buffer, uint64 start_sector, uint32 sector_count, bool write)
{
    if (sector_count == 0)
    {
        return 0;
    }
    if (!g_initialized || buffer == nullptr || start_sector >= g_capacity_sectors ||
        static_cast<uint64>(sector_count) > g_capacity_sectors - start_sector)
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
    return g_initialized
               ? g_capacity_sectors * static_cast<uint64>(k_sector_size)
               : 0;
}
} // namespace riscv::jh7110::dwmmc
