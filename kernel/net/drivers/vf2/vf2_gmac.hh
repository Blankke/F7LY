/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Based on the RocketOS VisionFive2 driver, revision
 * 1e1fc238f6ffa17b262163f029d48f660943cd96.
 * Ported to F7LY C++ and adapted to the F7LY DMA/ONPS interfaces.
 */
#pragma once

#include "types.hh"

namespace net
{
    bool vf2_gmac_init();
    bool vf2_gmac_is_initialized();
    int vf2_gmac_send(const void *data, uint32 len);
    int vf2_gmac_recv(void *data, uint32 *len);
    void vf2_gmac_poll();
    void vf2_gmac_intr();
    bool vf2_gmac_link_up();
    void vf2_gmac_get_mac(uint8 mac[6]);
    void vf2_gmac_debug_status();
}
