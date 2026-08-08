#include "boot_args.hh"

#include "hal/loongarch/platform_board.hh"

namespace loongarch::boot
{
namespace
{
    constexpr uint32 k_fdt_magic = 0xd00dfeedU;
    constexpr uint64 k_uhi_fdt_arg0 = ~1ULL;
    constexpr uint64 k_max_uboot_args = 16;
    constexpr uint64 k_max_uboot_arg_length = 32;

    constexpr uint32 byte_swap32(uint32 value)
    {
        return ((value & 0x000000ffU) << 24) |
               ((value & 0x0000ff00U) << 8) |
               ((value & 0x00ff0000U) >> 8) |
               ((value & 0xff000000U) >> 24);
    }

    const volatile uint8 *firmware_pointer(uint64 address)
    {
        if (address == 0)
        {
            return nullptr;
        }
        return reinterpret_cast<const volatile uint8 *>(board::cached_address(address));
    }

    bool is_fdt(uint64 address)
    {
        const volatile uint8 *ptr = firmware_pointer(address);
        if (ptr == nullptr)
        {
            return false;
        }

        uint32 raw_magic = static_cast<uint32>(ptr[0]) |
                           (static_cast<uint32>(ptr[1]) << 8) |
                           (static_cast<uint32>(ptr[2]) << 16) |
                           (static_cast<uint32>(ptr[3]) << 24);
        return byte_swap32(raw_magic) == k_fdt_magic;
    }

    bool parse_hex_address(uint64 string_address, uint64 &value)
    {
        const volatile uint8 *text = firmware_pointer(string_address);
        if (text == nullptr)
        {
            return false;
        }

        uint64 index = 0;
        if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            index = 2;
        }

        uint64 result = 0;
        bool has_digit = false;
        for (; index < k_max_uboot_arg_length; ++index)
        {
            uint8 ch = text[index];
            if (ch == 0)
            {
                if (has_digit)
                {
                    value = result;
                }
                return has_digit;
            }

            uint64 digit = 0;
            if (ch >= '0' && ch <= '9')
                digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                digit = ch - 'A' + 10;
            else
                return false;

            if (result > (~0ULL - digit) / 16)
            {
                return false;
            }
            result = result * 16 + digit;
            has_digit = true;
        }
        return false;
    }

    uint64 read_firmware_u64(uint64 address)
    {
        const volatile uint8 *ptr = firmware_pointer(address);
        uint64 value = 0;
        for (uint64 byte = 0; byte < sizeof(uint64); ++byte)
        {
            value |= static_cast<uint64>(ptr[byte]) << (byte * 8);
        }
        return value;
    }

    uint64 resolve_uboot_go_dtb(uint64 argc, uint64 argv_address)
    {
        if (argc < 2 || argc > k_max_uboot_args || argv_address == 0)
        {
            return 0;
        }

        // argv[0] 是 go 的入口参数。先确认它确实是十六进制地址，避免把普通
        // 固件 ABI 的小整数误当成可解引用的 argv。
        uint64 ignored_entry = 0;
        uint64 argv0 = read_firmware_u64(argv_address);
        if (!parse_hex_address(argv0, ignored_entry))
        {
            return 0;
        }

        for (uint64 index = 1; index < argc; ++index)
        {
            uint64 string_address = read_firmware_u64(argv_address + index * sizeof(uint64));
            uint64 candidate = 0;
            if (parse_hex_address(string_address, candidate) && is_fdt(candidate))
            {
                return board::physical_address(candidate);
            }
        }
        return 0;
    }
} // namespace

uint64 resolve_dtb(uint64 arg0, uint64 arg1, uint64 arg2, uint64 arg3)
{
    (void)arg2;
    (void)arg3;

    // QEMU 以及部分 UHI 固件直接把 FDT 放在 a1。先检查内容而不是仅凭 a0，
    // 这样不会把 U-Boot argc=1 与 EFI 入口混淆。
    if (is_fdt(arg1))
    {
        return board::physical_address(arg1);
    }
    if (arg0 == k_uhi_fdt_arg0)
    {
        return 0;
    }
    return resolve_uboot_go_dtb(arg0, arg1);
}
} // namespace loongarch::boot
