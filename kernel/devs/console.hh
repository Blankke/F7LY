#ifndef CONSOLE_HH
#define CONSOLE_HH

#include "spinlock.hh"
#include "uart.hh"
#define INPUT_BUF_SIZE 128
#define BACKSPACE 0x100
#define CTRL_(x) ((x) - '@')
namespace dev
{
class Console
{
    private:
        SpinLock _lock;
        char input_buf[INPUT_BUF_SIZE];
        int r_idx;
        int w_idx;
        int e_idx; // 输入缓冲区中最后一个字符的索引
        UartManager uart;
    public:
        Console();
        void init();
        void console_putc(int c);
        int console_write(uint64 src, int n);
        int console_read(int user_dst, uint64 dst, int n);
        int console_intr(int c);
};

extern Console kConsole; // 全局控制台对象
};

#endif // CONSOLE_HH