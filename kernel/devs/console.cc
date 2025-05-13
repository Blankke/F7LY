#include "console.hh"
#include "../mem/memlayout.hh"
#include "../hal/riscv/sbi.hh"
#include "printer.hh"

Console kConsole; // 全局控制台对象

Console::Console()
{
}

void Console::init()
{
  _lock.init("console");
  uart.init(UART0);
}

void Console::console_putc(int c)
{
  if (c == BACKSPACE)
  {
    uart.putc_sync('\b');
    uart.putc_sync(' ');
    uart.putc_sync('\b');
  }
  else if (c == '\n' || c == '\r')
  {
    uart.putc('\n');
  }
  else
  {
    uart.putc_sync(c);
  }
}

int Console::console_write(int user_src, uint64 src, int n)
{
  for (int i = 0; i < n; i++)
  {
    char c;
    // either_copyin
    TODO(if (either_copyin(&c, user_src, src + i, 1) == -1)
      break;)
    sbi_console_putchar(c);
  }
  return 0;
}

int Console::console_read(int user_dst, uint64 dst, int n)
{
  // TODO
  return 0;
}
int Console::console_intr(int c)
{
  // TODO
  _lock.acquire();

  switch (c)
  {
  case CTRL_('P'): // Print process list.
    // TODO:procdump();
    break;
  case CTRL_('U'): // Kill line.
    while (e_idx != w_idx &&
           input_buf[(e_idx - 1) % INPUT_BUF_SIZE] != '\n')
    {
      e_idx--;
      uart.putc_sync(BACKSPACE);
    }
    break;
  case CTRL_('H'): // Backspace
  case '\x7f':     // Delete key
    if (e_idx != w_idx)
    {
      e_idx--;
      uart.putc_sync(BACKSPACE);
    }
    break;
  default:
    if (c != 0 && e_idx - r_idx < INPUT_BUF_SIZE)
    {
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      uart.putc_sync(c);

      // store for consumption by consoleread().
      input_buf[e_idx++ % INPUT_BUF_SIZE] = c;

      if (c == '\n' || c == CTRL_('D') || e_idx - r_idx == INPUT_BUF_SIZE)
      {
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        w_idx = e_idx;
        // wakeup(&r_idx);
      }
    }
    break;
  }

  _lock.release();
  return 0;
}
