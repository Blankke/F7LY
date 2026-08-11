.globl initcode_start, initcode_end

#ifndef INITCODE_BIN_PATH
#error "INITCODE_BIN_PATH 必须由当前平台画像的构建规则提供"
#endif

initcode_start:
.incbin INITCODE_BIN_PATH
initcode_end:
