
// F
#define F_L1 "███████╗ "
#define F_L2 "██╔════╝ "
#define F_L3 "█████╗   "
#define F_L4 "██╔══╝   "
#define F_L5 "██║      "
#define F_L6 "╚═╝      "


// U
#define U_L1 "██╗    ██╗ "
#define U_L2 "██║    ██║ "
#define U_L3 "██║    ██║ "
#define U_L4 "██║    ██║ "
#define U_L5 "╚███████╔╝ "
#define U_L6 " ╚══════╝  "

// C
#define C_L1 "  ██████╗"
#define C_L2 " ██╔════╝"
#define C_L3 " ██║     "
#define C_L4 " ██║     "
#define C_L5 " ╚██████╗"
#define C_L6 "  ╚═════╝"

// K
#define K_L1 "██╗  ██╗ "
#define K_L2 "██║ ██╔╝ "
#define K_L3 "█████╔╝  "
#define K_L4 "██╔═██╗  "
#define K_L5 "██║  ██╗ "
#define K_L6 "╚═╝  ╚═╝ "

// Y
#define Y_L1 "██╗   ██╗"
#define Y_L2 "╚██╗ ██╔╝"
#define Y_L3 " ╚████╔╝ "
#define Y_L4 "  ╚██╔╝  "
#define Y_L5 "   ██║   "
#define Y_L6 "   ╚═╝   "

// O
#define O_L1 " ██████╗ "
#define O_L2 "██╔═══██╗"
#define O_L3 "██║   ██║"
#define O_L4 "██║   ██║"
#define O_L5 "╚██████╔╝"
#define O_L6 " ╚═════╝ "

// L
#define L_L1 "██╗      "
#define L_L2 "██║      "
#define L_L3 "██║      "
#define L_L4 "██║      "
#define L_L5 "███████╗ "
#define L_L6 "╚══════╝ "

// 7
#define S7_L1 "████████╗"
#define S7_L2 "╚════██╔╝"
#define S7_L3 "    ██║  "
#define S7_L4 "   ██║   "
#define S7_L5 "   ██║   "
#define S7_L6 "   ╚═╝   "


#define __PRINTF_WARN_COLOR(color, format, ...) \
    do {                                        \
            printf(color format "\33[0m", ##__VA_ARGS__); \
    } while (0)

#define __PRINTF_INFO_COLOR(color, format, ...) \
    do {                                        \
            printf(color format "\33[0m", ##__VA_ARGS__); \
    } while (0)

#define printfRed(format, ...) __PRINTF_WARN_COLOR("\33[1;31m", format, ##__VA_ARGS__)
#define printfGreen(format, ...) __PRINTF_INFO_COLOR("\33[1;32m", format, ##__VA_ARGS__)
#define printfBlue(format, ...) __PRINTF_INFO_COLOR("\33[1;34m", format, ##__VA_ARGS__)
#define printfCyan(format, ...) __PRINTF_INFO_COLOR("\33[1;36m", format, ##__VA_ARGS__)
#define printfYellow(format, ...) __PRINTF_WARN_COLOR("\33[1;33m", format, ##__VA_ARGS__)
#define printfWhite(format, ...) __PRINTF_INFO_COLOR("\33[1;37m", format, ##__VA_ARGS__)
#define printfMagenta(format, ...) __PRINTF_INFO_COLOR("\33[1;35m", format, ##__VA_ARGS__)

// 颜色太少了，我给你加几个
#define printfBlack(format, ...) __PRINTF_INFO_COLOR("\33[1;30m", format, ##__VA_ARGS__)
#define printfOrange(format, ...) __PRINTF_WARN_COLOR("\33[1;38;5;208m", format, ##__VA_ARGS__)
#define printfPurple(format, ...) __PRINTF_INFO_COLOR("\33[1;38;5;129m", format, ##__VA_ARGS__)
#define printfPink(format, ...) __PRINTF_WARN_COLOR("\33[1;38;5;205m", format, ##__VA_ARGS__)
#define printfBrown(format, ...) __PRINTF_WARN_COLOR("\33[1;38;5;94m", format, ##__VA_ARGS__)
#define printfGray(format, ...) __PRINTF_INFO_COLOR("\33[1;90m", format, ##__VA_ARGS__)
#define printfLightRed(format, ...) __PRINTF_WARN_COLOR("\33[0;91m", format, ##__VA_ARGS__)
#define printfLightGreen(format, ...) __PRINTF_INFO_COLOR("\33[0;92m", format, ##__VA_ARGS__)
#define printfLightBlue(format, ...) __PRINTF_INFO_COLOR("\33[0;94m", format, ##__VA_ARGS__)
#define printfLightCyan(format, ...) __PRINTF_INFO_COLOR("\33[0;96m", format, ##__VA_ARGS__)
#define printfLightYellow(format, ...) __PRINTF_WARN_COLOR("\33[0;93m", format, ##__VA_ARGS__)
#define printfLightMagenta(format, ...) __PRINTF_INFO_COLOR("\33[0;95m", format, ##__VA_ARGS__)

// Background colors
#define printfBgRed(format, ...) __PRINTF_WARN_COLOR("\33[1;41m", format, ##__VA_ARGS__)
#define printfBgGreen(format, ...) __PRINTF_INFO_COLOR("\33[1;42m", format, ##__VA_ARGS__)
#define printfBgBlue(format, ...) __PRINTF_INFO_COLOR("\33[1;44m", format, ##__VA_ARGS__)
#define printfBgYellow(format, ...) __PRINTF_WARN_COLOR("\33[1;43m", format, ##__VA_ARGS__)
#define printfBgCyan(format, ...) __PRINTF_INFO_COLOR("\33[1;46m", format, ##__VA_ARGS__)
#define printfBgMagenta(format, ...) __PRINTF_INFO_COLOR("\33[1;45m", format, ##__VA_ARGS__)
// Info print macros
#define Info(fmt, ...) printf("[INFO] => " fmt "", ##__VA_ARGS__)
#define Info_R(fmt, ...) printfRed("[INFO] => " fmt "", ##__VA_ARGS__)

void print_fuckyou();
void print_f7ly();