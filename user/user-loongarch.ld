OUTPUT_ARCH( "loongarch64" )
ENTRY( main )


SECTIONS
{
 . = 0x0;

  .text : {
    *(.text.startup .text.startup.*)
    *(.text .text.*)
  }

  /*
   * initcode-la 会被直接 objcopy 成原始二进制再由内核从用户地址 0 开始执行。
   * 如果保留 build-id/comment 这类非指令段，入口地址会被推后，内核仍从 0 起跳，
   * 最终把 NOTE 数据当成代码执行。这里和 RISC-V 保持一致，明确丢弃这些段。
   */
  /DISCARD/ : {
    *(.note.gnu.build-id)
    *(.note.GNU-stack)
    *(.comment)
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
    . = ALIGN(0x1000);
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  }

  PROVIDE(end = .);
}
