#pragma once

#include "types.hh"

namespace net
{
    bool vf2_adapter_init();
    void vf2_adapter_cleanup();
    void vf2_get_mac_address(unsigned char mac[6]);
    void vf2_adapter_debug_status();

    int vf2_emac_send(short buf_list_head, unsigned char *error);
    void vf2_recv_thread(void *param);
}
