OUTPUT_ARCH("riscv")
ENTRY(_entry)

/*
 * 使用示例：make vf2 run 或 make vf2 shell。
 * VF2 U-Boot 的 kernel_addr_r 默认约定为 0x40200000；这里生成的 .bin
 * 可由启动分区加载到该地址，ELF 则保留给调试和符号分析使用。
 */
F7LY_KERNEL_BASE = 0x40200000;

SECTIONS
{
  . = F7LY_KERNEL_BASE;

  .text : {
    *(.text.entry)
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    _sig_trampoline = .;
    *(sigtrampsec)
    . = ALIGN(0x1000);
    PROVIDE(etext = .);
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.stack)
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    PROVIDE(bss_start = .);
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    PROVIDE(bss_end = .);
  }

  PROVIDE(end = .);

  /DISCARD/ : {
    *(.note.gnu.build-id)
    *(.comment)
  }
}
