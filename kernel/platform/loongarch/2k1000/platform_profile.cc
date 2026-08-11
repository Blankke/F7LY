#include "platform/profile.hh"

namespace platform
{
const Profile &current_profile()
{
    static constexpr Profile profile{
        .name = "Loongson 2K1000",
        .architecture = Architecture::LoongArch64,
        .machine = Machine::Loongson2K1000,
        .verbose_boot_diagnostics = true,
    };
    return profile;
}
} // namespace platform
