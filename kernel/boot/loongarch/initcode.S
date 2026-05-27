.globl initcode_start, initcode_end

#ifndef INITCODE_BIN_PATH
#define INITCODE_BIN_PATH "../../user/initcode-la"
#endif

initcode_start:
.incbin INITCODE_BIN_PATH
initcode_end:
