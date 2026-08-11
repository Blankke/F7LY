#include "platform/profile.hh"

namespace platform
{
const Profile &current_profile()
{
    static constexpr Profile profile{
        .name = "RISC-V QEMU virt",
        .architecture = Architecture::Riscv64,
        .machine = Machine::QemuVirt,
        .verbose_boot_diagnostics = false,
    };
    return profile;
}
} // namespace platform
