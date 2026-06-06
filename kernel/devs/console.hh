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
        bool _canonical_mode;
        bool _echo_enabled;
        bool _map_cr_to_nl;
        bool _signal_enabled;
        unsigned char _erase_char;
        unsigned char _kill_char;
        unsigned char _eof_char;
        unsigned char _intr_char;
        int _foreground_pgrp;
    public:
        Console();
        void init();
        void console_putc(int c);
        int console_read_kernel(void *dst, int n);
        int console_intr(int c);
        int buffered_input_size();
        void flush_input();
        void set_line_discipline(bool canonical_mode, bool echo_enabled,
                                 bool map_cr_to_nl, unsigned char erase_char,
                                 unsigned char kill_char, unsigned char eof_char,
                                 bool signal_enabled, unsigned char intr_char);
        void set_foreground_pgrp(int pgrp);
        int foreground_pgrp();
};

extern Console kConsole; // 全局控制台对象
};

#endif // CONSOLE_HH
