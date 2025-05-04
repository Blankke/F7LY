#include "uart.hh"
#include "printer.hh"
#include "../hal/riscv/sbi.hh"
#include "param.h"


extern "C" void main() {
    k_printer.init();
    k_printer.printf("Hello, World!\n");
}
