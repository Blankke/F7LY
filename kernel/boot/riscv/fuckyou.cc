#include "printer.hh"
#include "fuckyou.hh"
void print_fuckyou_horizontal() {
    // 第1行（所有字母的第1行）
    printfRed(F_L1);      // F
    printfRed(U_L1);  // U
    printfRed(C_L1);     // C
    printfRed(K_L1);   // K
    printfRed(Y_L1);    // Y
    printfRed(O_L1);     // O
    printfRed(U_L1);    // U
    printfRed("\n");    // 换行

    // 第2行
    printfMagenta(F_L2);
    printfMagenta(U_L2);
    printfMagenta(C_L2);
    printfMagenta(K_L2);
    printfMagenta(Y_L2);
    printfMagenta(O_L2);
    printfMagenta(U_L2);
    printfMagenta("\n");

    // 第3行
    printfCyan(F_L3);
    printfCyan(U_L3);
    printfCyan(C_L3);
    printfCyan(K_L3);
    printfCyan(Y_L3);
    printfCyan(O_L3);
    printfCyan(U_L3);
    printfCyan("\n");

    // 第4行
    printfYellow(F_L4);
    printfYellow(U_L4);
    printfYellow(C_L4);
    printfYellow(K_L4);
    printfYellow(Y_L4);
    printfYellow(O_L4);
    printfYellow(U_L4);
    printfYellow("\n");

    // 第5行
    printfGreen(F_L5);
    printfGreen(U_L5);
    printfGreen(C_L5);
    printfGreen(K_L5);
    printfGreen(Y_L5);
    printfGreen(O_L5);
    printfGreen(U_L5);
    printfGreen("\n");
}

