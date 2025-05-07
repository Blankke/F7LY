#include "uart.hh"
#include "printer.hh"
#include "param.h"




extern "C" void main() {
    k_printer.init();
    printfRed("Hello, World!\n");
    Info("Hello, World!\n");
    printfCyan("Hello, World!\n");
    printfYellow("Hello, World!\n");
    

}
