#pragma once
#include "proc_manager.hh"
inline void debug_fd_4()
{
//     proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
//     fs::file *f = p->get_open_file(4);
//     if (f != nullptr)
//     {
//         char k_buf[256];
//         f->read((uint64)k_buf, 256, 0, 0);
//         printf("hexdump of k_buf (256 bytes):\n");
//         for (int i = 0; i < 256; i += 16)
//         {
//             printf("%04x: ", i);
//             for (int j = 0; j < 16; ++j)
//             {
//                 printf("%02x ", (unsigned char)k_buf[i + j]);
//             }
//             printf(" | ");
//             for (int j = 0; j < 16; ++j)
//             {
//                 char c = k_buf[i + j];
//                 printf("%c", (c >= 32 && c <= 126) ? c : '.');
//             }
//             printf("\n");
//         }
    // }
}