#ifdef LOONGARCH
#include "types.hh"

extern "C" {
    extern void wrap_kerneltrap(const uint64 *saved_frame);
    extern void wrap_usertrap();
    extern void wrap_usertrapret();
}
#endif
