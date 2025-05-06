#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "slab.hh"
#include "pagetable.hh"
#include "fuckyou.hh"
__attribute__ ((aligned (16))) char stack0[NCPU][4096*2];



// 打印彩色大字母（每个字母不同颜色）

void main() {
    k_printer.init();
    printfWhite("\n\n");  // 留出顶部空白
    print_fuckyou_horizontal();
    printfWhite("\n\n");  // 底部空白


}
