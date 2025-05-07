// 每个字母的 ASCII 横向块（高度一致，5行）
#define F_L1 "█████  "  // 第1行
#define F_L2 "█      "
#define F_L3 "████   "
#define F_L4 "█      "
#define F_L5 "█      "

#define U_L1 "█   █ "
#define U_L2 "█   █ "
#define U_L3 "█   █ "
#define U_L4 "█   █ "
#define U_L5 "█████ "

#define C_L1 " ████ "
#define C_L2 "█     "
#define C_L3 "█     "
#define C_L4 "█     "
#define C_L5 " ████ "

#define K_L1 "█   █ "
#define K_L2 "█  █  "
#define K_L3 "███   "
#define K_L4 "█  █  "
#define K_L5 "█   █ "

#define Y_L1 "█   █ "
#define Y_L2 " █ █  "
#define Y_L3 "  █   "
#define Y_L4 "  █   "
#define Y_L5 "  █   "

#define O_L1 " ███  "
#define O_L2 "█   █ "
#define O_L3 "█   █ "
#define O_L4 "█   █ "
#define O_L5 " ███  "

// 打印一行（带颜色控制）
#define PRINT_LINE(color, line) \
    color(line)
#include "printer.hh"
// 横向打印所有字母的同一行
void print_fuckyou_horizontal() ;