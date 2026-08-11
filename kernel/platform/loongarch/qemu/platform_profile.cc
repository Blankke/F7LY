#include "platform/profile.hh"

namespace platform
{
const Profile &current_profile()
{
    static constexpr Profile profile{
        .name = "LoongArch QEMU virt",
        .architecture = Architecture::LoongArch64,
        .machine = Machine::QemuVirt,
        .verbose_boot_diagnostics = false,
    };
    return profile;
}
} // namespace platform
