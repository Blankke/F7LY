#include "platform/profile.hh"

namespace platform
{
const Profile &current_profile()
{
    static constexpr Profile profile{
        .name = "StarFive VisionFive 2",
        .architecture = Architecture::Riscv64,
        .machine = Machine::VisionFive2,
        .verbose_boot_diagnostics = true,
    };
    return profile;
}
} // namespace platform
