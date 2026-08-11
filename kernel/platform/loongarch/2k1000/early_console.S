#include "platform_board_constants.h"

.section .text
.global platform_early_boot_marker
platform_early_boot_marker:
        # Printer/Console 尚未初始化，只输出一条有界标记。这里是 2K1000
        # composition root，硬件地址不会泄漏到 LoongArch 公共入口。
        li.d        $t2, LS2K1000_UART_UNCACHED_DMW
        la.global   $t3, .Learly_boot_message
.Learly_uart_next:
        li.w        $t5, 1000000
.Learly_uart_wait:
        ld.bu       $t4, $t2, 5
        andi        $t4, $t4, 0x20
        bnez        $t4, .Learly_uart_ready
        addi.w      $t5, $t5, -1
        bnez        $t5, .Learly_uart_wait
        jr          $ra
.Learly_uart_ready:
        ld.bu       $t4, $t3, 0
        beqz        $t4, .Learly_uart_done
        st.b        $t4, $t2, 0
        addi.d      $t3, $t3, 1
        b           .Learly_uart_next
.Learly_uart_done:
        jr          $ra

.section .rodata
.Learly_boot_message:
        .asciz "[early] entry reached\n"
