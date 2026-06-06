#include "console_termios.hh"

#include "console.hh"
#include "klib.hh"
#include <termios.h>

namespace dev
{
    ConsoleTermiosController k_console_termios;

    ConsoleTermiosController::ConsoleTermiosController()
        : _termios(make_default_termios())
    {
        sync_to_line_discipline(_termios);
    }

    abi::KernelTermios ConsoleTermiosController::snapshot() const
    {
        return _termios;
    }

    abi::KernelTermio ConsoleTermiosController::legacy_snapshot() const
    {
        return to_legacy(_termios);
    }

    void ConsoleTermiosController::apply(const abi::KernelTermios &termios)
    {
        _termios = termios;
        sync_to_line_discipline(_termios);
    }

    void ConsoleTermiosController::apply_legacy(const abi::KernelTermio &termio)
    {
        merge_legacy(_termios, termio);
        sync_to_line_discipline(_termios);
    }

    abi::KernelTermios ConsoleTermiosController::make_default_termios()
    {
        abi::KernelTermios ts{};
        ts.c_iflag = BRKINT | ICRNL | IXON;
#ifdef IMAXBEL
        ts.c_iflag |= IMAXBEL;
#endif
        ts.c_oflag = OPOST | ONLCR;
        ts.c_cflag = B38400 | CS8 | CREAD;
        ts.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
#ifdef ECHOCTL
        ts.c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
        ts.c_lflag |= ECHOKE;
#endif
        ts.c_cc[VINTR] = 3;    // Ctrl-C
        ts.c_cc[VQUIT] = 28;   // Ctrl-backslash
        ts.c_cc[VERASE] = 127; // DEL
        ts.c_cc[VKILL] = 21;   // Ctrl-U
        ts.c_cc[VEOF] = 4;     // Ctrl-D
        ts.c_cc[VTIME] = 0;
        ts.c_cc[VMIN] = 1;
#ifdef VSTART
        ts.c_cc[VSTART] = 17; // Ctrl-Q
#endif
#ifdef VSTOP
        ts.c_cc[VSTOP] = 19; // Ctrl-S
#endif
#ifdef VSUSP
        ts.c_cc[VSUSP] = 26; // Ctrl-Z
#endif
#ifdef VEOL
        ts.c_cc[VEOL] = 0;
#endif
#ifdef VREPRINT
        ts.c_cc[VREPRINT] = 18; // Ctrl-R
#endif
#ifdef VDISCARD
        ts.c_cc[VDISCARD] = 15; // Ctrl-O
#endif
#ifdef VWERASE
        ts.c_cc[VWERASE] = 23; // Ctrl-W
#endif
#ifdef VLNEXT
        ts.c_cc[VLNEXT] = 22; // Ctrl-V
#endif
#ifdef VEOL2
        ts.c_cc[VEOL2] = 0;
#endif
        return ts;
    }

    void ConsoleTermiosController::sync_to_line_discipline(const abi::KernelTermios &ts)
    {
        // termios 状态是用户可见 ABI；console line discipline 是内核内部行为。
        // 两者集中同步，避免 TCSETS/TCSETA 路径各自维护半套规则。
        dev::kConsole.set_line_discipline((ts.c_lflag & ICANON) != 0,
                                          (ts.c_lflag & ECHO) != 0,
                                          (ts.c_iflag & ICRNL) != 0,
                                          ts.c_cc[VERASE],
                                          ts.c_cc[VKILL],
                                          ts.c_cc[VEOF],
                                          (ts.c_lflag & ISIG) != 0,
                                          ts.c_cc[VINTR]);
    }

    abi::KernelTermio ConsoleTermiosController::to_legacy(const abi::KernelTermios &ts)
    {
        abi::KernelTermio tio{};
        tio.c_iflag = static_cast<uint16>(ts.c_iflag);
        tio.c_oflag = static_cast<uint16>(ts.c_oflag);
        tio.c_cflag = static_cast<uint16>(ts.c_cflag);
        tio.c_lflag = static_cast<uint16>(ts.c_lflag);
        tio.c_line = ts.c_line;
        size_t cc_count = sizeof(tio.c_cc) < sizeof(ts.c_cc) ? sizeof(tio.c_cc) : sizeof(ts.c_cc);
        memcpy(tio.c_cc, ts.c_cc, cc_count);
        return tio;
    }

    void ConsoleTermiosController::merge_legacy(abi::KernelTermios &ts,
                                                const abi::KernelTermio &tio)
    {
        ts.c_iflag = tio.c_iflag;
        ts.c_oflag = tio.c_oflag;
        ts.c_cflag = tio.c_cflag;
        ts.c_lflag = tio.c_lflag;
        ts.c_line = tio.c_line;
        size_t cc_count = sizeof(tio.c_cc) < sizeof(ts.c_cc) ? sizeof(tio.c_cc) : sizeof(ts.c_cc);
        memcpy(ts.c_cc, tio.c_cc, cc_count);
    }
}
