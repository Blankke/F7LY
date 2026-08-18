#include "console.hh"
#include "console_termios.hh"
#include "hal/irq.hh"
#include "platform/console_backend.hh"
#include "printer.hh"
#include "proc_manager.hh"
namespace dev
{
  namespace
  {
    void echo_output_char(int c)
    {
      const u8 character = static_cast<u8>(c);
      if (c == '\n' && k_console_termios.map_output_newline())
      {
        // 输入行规程已经把 Enter 的 CR 映射成 LF；回显仍需服从
        // OPOST|ONLCR，否则下一次 shell 提示符会从当前列继续右移。
        k_uart.put_chars_sync_crlf(&character, 1);
        return;
      }
      k_uart.put_char_sync(character);
    }

    void uart_irq_handler(void *)
    {
      k_uart.handle_intr();
    }
  }

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
    // termios 全局对象只保存 ABI 状态；等 Console 的锁可用后，再明确把
    // 默认配置交给行规程，避免依赖不同翻译单元的全局构造先后顺序。
    k_console_termios.initialize_line_discipline();

    // 控制台、中断分发和 /dev/stdin 共享同一个软件 UART 实例；具体
    // MMIO/SBI 传输由当前平台后端在 initialize() 中建立。
    if (!k_uart.initialize())
    {
      panic("console backend %s initialization failed",
            platform::console_backend::name());
    }

    const platform::console_backend::InterruptSource source =
        platform::console_backend::interrupt_source();
    if (source.present &&
        !hal::irq::register_handler(source.source, uart_irq_handler, nullptr,
                                    "console"))
    {
      panic("console backend %s failed to register interrupt source %u",
            platform::console_backend::name(), source.source);
    }
  }

  void Console::console_putc(int c)
  {
    if (c == BACKSPACE)
    {
      k_uart.put_char_sync('\b');
      k_uart.put_char_sync(' ');
      k_uart.put_char_sync('\b');
    }
    else if (c == '\n')
    {
      // 串口终端的 LF 只下移光标，不会回到行首。内核日志统一输出 CRLF，
      // 否则连续打印后会逐行向右漂移，最终只剩屏幕最右侧一列。
      const u8 newline[] = {'\r', '\n'};
      k_uart.put_chars_sync(newline, sizeof(newline));
    }
    else if (c == '\r')
    {
      k_uart.put_char_sync('\r');
    }
    else
    {
      k_uart.put_char_sync(c);
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
        k_uart.put_char_sync('^');
        k_uart.put_char_sync('C');
        echo_output_char('\n');
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
              k_uart.put_char_sync((u8)BACKSPACE);
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
              k_uart.put_char_sync((u8)BACKSPACE);
            }
          }
          break;
        }
        if (c != 0 && e_idx - r_idx < INPUT_BUF_SIZE)
        {
          if (_echo_enabled)
          {
            echo_output_char(c);
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
          echo_output_char(c);
        }
        input_buf[e_idx++ % INPUT_BUF_SIZE] = c;
        w_idx = e_idx;
      }
    }

    _lock.release();
    return 0;
  }
};
