#include "console.hh"
#include "proc_manager.hh"
namespace dev
{
  Console kConsole; // 全局控制台对象

  Console::Console()
  {
    r_idx = w_idx = e_idx = 0;
    _canonical_mode = true;
    _echo_enabled = true;
    _map_cr_to_nl = true;
    _signal_enabled = true;
    _erase_char = 0x7f;
    _kill_char = CTRL_('U');
    _eof_char = CTRL_('D');
    _intr_char = CTRL_('C');
    _foreground_pgrp = 0;
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
      uart.put_char_sync('\b');
      uart.put_char_sync(' ');
      uart.put_char_sync('\b');
    }
    else if (c == '\n' || c == '\r')
    {
      uart.put_char('\n');
    }
    else
    {
      uart.put_char_sync(c);
    }
  }

  int Console::console_read_kernel(void *dst, int n)
  {
    if (dst == nullptr || n <= 0)
    {
      return 0;
    }

    _lock.acquire();
    int copied = 0;
    char *out = reinterpret_cast<char *>(dst);
    while (copied < n)
    {
      if (r_idx == w_idx)
      {
        break;
      }

      char c = input_buf[r_idx % INPUT_BUF_SIZE];
      r_idx++;
      out[copied++] = c;

      if (_canonical_mode && c == '\n')
      {
        break;
      }
    }
    _lock.release();
    return copied;
  }

  int Console::buffered_input_size()
  {
    _lock.acquire();
    int available = w_idx - r_idx;
    _lock.release();
    return available;
  }

  void Console::flush_input()
  {
    _lock.acquire();
    r_idx = w_idx = e_idx = 0;
    _lock.release();
  }

  void Console::set_line_discipline(bool canonical_mode, bool echo_enabled,
                                    bool map_cr_to_nl, unsigned char erase_char,
                                    unsigned char kill_char, unsigned char eof_char,
                                    bool signal_enabled, unsigned char intr_char)
  {
    _lock.acquire();
    _canonical_mode = canonical_mode;
    _echo_enabled = echo_enabled;
    _map_cr_to_nl = map_cr_to_nl;
    _signal_enabled = signal_enabled;
    _erase_char = erase_char;
    _kill_char = kill_char;
    _eof_char = eof_char;
    _intr_char = intr_char;
    _lock.release();
  }

  void Console::set_foreground_pgrp(int pgrp)
  {
    _lock.acquire();
    _foreground_pgrp = pgrp;
    _lock.release();
  }

  int Console::foreground_pgrp()
  {
    _lock.acquire();
    int pgrp = _foreground_pgrp;
    _lock.release();
    return pgrp;
  }

  int Console::console_intr(int c)
  {
    _lock.acquire();

    if (_map_cr_to_nl && c == '\r')
    {
      c = '\n';
    }

    // 交互式 shell 需要把 Ctrl-C 送给当前前台进程组，而不是仅仅把字节塞进输入缓冲。
    if (_signal_enabled && _intr_char != 0 && c == _intr_char)
    {
      int target_pgrp = _foreground_pgrp;
      if (_echo_enabled)
      {
        uart.put_char_sync('^');
        uart.put_char_sync('C');
        uart.put_char_sync('\n');
      }
      r_idx = w_idx = e_idx = 0;
      _lock.release();

      proc::Pcb *cur = proc::k_pm.get_cur_pcb();
      if (target_pgrp <= 0 && cur != nullptr)
      {
        target_pgrp = static_cast<int>(cur->get_pgid());
      }
      if (target_pgrp > 0)
      {
        proc::k_pm.kill_signal(-target_pgrp, proc::ipc::signal::SIGINT);
      }
      return 0;
    }

    if (_canonical_mode)
    {
      switch (c)
      {
      case CTRL_('P'): // Print process list.
        // TODO:procdump();
        break;
      default:
        if (c == _kill_char)
        {
          while (e_idx != w_idx &&
                 input_buf[(e_idx - 1) % INPUT_BUF_SIZE] != '\n')
          {
            e_idx--;
            if (_echo_enabled)
            {
              uart.put_char_sync((u8)BACKSPACE);
            }
          }
          break;
        }
        if (c == CTRL_('H') || c == '\x7f' || c == _erase_char)
        {
          if (e_idx != w_idx)
          {
            e_idx--;
            if (_echo_enabled)
            {
              uart.put_char_sync((u8)BACKSPACE);
            }
          }
          break;
        }
        if (c != 0 && e_idx - r_idx < INPUT_BUF_SIZE)
        {
          if (_echo_enabled)
          {
            uart.put_char_sync(c);
          }
          input_buf[e_idx++ % INPUT_BUF_SIZE] = c;
          if (c == '\n' || c == _eof_char || e_idx - r_idx == INPUT_BUF_SIZE)
          {
            w_idx = e_idx;
          }
        }
        break;
      }
    }
    else
    {
      if (c != 0 && e_idx - r_idx < INPUT_BUF_SIZE)
      {
        if (_echo_enabled)
        {
          uart.put_char_sync(c);
        }
        input_buf[e_idx++ % INPUT_BUF_SIZE] = c;
        w_idx = e_idx;
      }
    }

    _lock.release();
    return 0;
  }
};
