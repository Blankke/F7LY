#include "printer.hh"
#include "fuckyou.hh"


void print_fuckyou() {
    const char* L1[] = {F_L1, U_L1, C_L1, K_L1, Y_L1, O_L1, U_L1};
    const char* L2[] = {F_L2, U_L2, C_L2, K_L2, Y_L2, O_L2, U_L2};
    const char* L3[] = {F_L3, U_L3, C_L3, K_L3, Y_L3, O_L3, U_L3};
    const char* L4[] = {F_L4, U_L4, C_L4, K_L4, Y_L4, O_L4, U_L4};
    const char* L5[] = {F_L5, U_L5, C_L5, K_L5, Y_L5, O_L5, U_L5};
    const char* L6[] = {F_L6, U_L6, C_L6, K_L6, Y_L6, O_L6, U_L6};
    // 按照彩虹色输出每一行
    for (int row = 0; row < 6; ++row) {
        switch (row) {
            case 0:
                printfRed("%s",L1[0]); printfRed("%s",L1[1]); printfRed("%s",L1[2]); printfRed("%s",L1[3]); printfRed("%s",L1[4]); printfRed("%s",L1[5]); printfRed("%s",L1[6]); break;
            case 1:
                printfMagenta("%s",L2[0]); printfMagenta("%s",L2[1]); printfMagenta("%s",L2[2]); printfMagenta("%s",L2[3]); printfMagenta("%s",L2[4]); printfMagenta("%s",L2[5]); printfMagenta("%s",L2[6]); break;
            case 2:
                printfYellow("%s",L3[0]); printfYellow("%s",L3[1]); printfYellow("%s",L3[2]); printfYellow("%s",L3[3]); printfYellow("%s",L3[4]); printfYellow("%s",L3[5]); printfYellow("%s",L3[6]); break;
            case 3:
                printfGreen("%s",L4[0]); printfGreen("%s",L4[1]); printfGreen("%s",L4[2]); printfGreen("%s",L4[3]); printfGreen("%s",L4[4]); printfGreen("%s",L4[5]); printfGreen("%s",L4[6]); break;
            case 4:
                printfCyan("%s",L5[0]); printfCyan("%s",L5[1]); printfCyan("%s",L5[2]); printfCyan("%s",L5[3]); printfCyan("%s",L5[4]); printfCyan("%s",L5[5]); printfCyan("%s",L5[6]); break;
            case 5:
                printfBlue("%s",L6[0]); printfBlue("%s",L6[1]); printfBlue("%s",L6[2]); printfBlue("%s",L6[3]); printfBlue("%s",L6[4]); printfBlue("%s",L6[5]); printfBlue("%s",L6[6]); break;
            }
        printf("\n");
    }
}
void print_f7ly() {
    // 第1行
    printfRed("%s",F_L1);
    printfRed("%s",S7_L1);
    printfRed("%s",L_L1);
    printfRed("%s",Y_L1);
    printfRed("\n");

    // 第2行
    printfMagenta("%s",F_L2);
    printfMagenta("%s",S7_L2);
    printfMagenta("%s",L_L2);
    printfMagenta("%s",Y_L2);
    printfMagenta("\n");

    // 第3行
    printfCyan("%s",F_L3);
    printfCyan("%s",S7_L3);
    printfCyan("%s",L_L3);
    printfCyan("%s",Y_L3);
    printfCyan("\n");

    // 第4行
    printfYellow("%s",F_L4);
    printfYellow("%s",S7_L4);
    printfYellow("%s",L_L4);
    printfYellow("%s",Y_L4);
    printfYellow("\n");

    // 第5行
    printfGreen("%s",F_L5);
    printfGreen("%s",S7_L5);
    printfGreen("%s",L_L5);
    printfGreen("%s",Y_L5);
    printfGreen("\n");

    // 第6行
    printfWhite("%s",F_L6);
    printfWhite("%s",S7_L6);
    printfWhite("%s",L_L6);
    printfWhite("%s",Y_L6);
    printfWhite("\n");
}