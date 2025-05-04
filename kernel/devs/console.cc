#include "console.hh"
#include "../mem/memlayout.hh"
#include "../hal/riscv/sbi.hh"
Console::Console()
{

}

void Console::init()
{
    lock.init("console");
    uart.init(UART0);
}

void Console::console_putc(int c)
{
    if(c == BACKSPACE)
    {
        uart.putc_sync('\b');
        uart.putc_sync(' ');
        uart.putc_sync('\b');
        sbi_console_putchar('\b');
        sbi_console_putchar(' ');
        sbi_console_putchar('\b');
    }
    else if (c == '\n' || c == '\r') {
    uart.putc('\n');
    sbi_console_putchar('\n');
}
    else
    {
        uart.putc_sync(c);
        sbi_console_putchar(c);
    }
}

int Console::console_write(int user_src, uint64 src, int n)
{
//TODO
    return 0;
}
int Console::console_read(int user_dst, uint64 dst, int n)
{
//TODO
    return 0;
}
int Console::console_intr(int c)
{
//TODO
    return 0;
}



