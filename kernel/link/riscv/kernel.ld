OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

PHDRS
{
  text   PT_LOAD FLAGS(5); /* R + X */
  rodata PT_LOAD FLAGS(4); /* R */
  data   PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
  /*
   * ensure that entry.S / _entry is at 0x80200000,
   * where qemu's -kernel jumps.
   */
  . = 0x80200000;
  PROVIDE(kernel_start = .);

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
  } :text

  . = ALIGN(0x1000);
  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  } :rodata

  .init_array : {
    . = ALIGN(8);
    PROVIDE(__preinit_array_start = .);
    KEEP(*(.preinit_array .preinit_array.*))
    PROVIDE(__preinit_array_end = .);
    PROVIDE(__init_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE(__init_array_end = .);
  } :rodata

  . = ALIGN(0x1000);
  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.stack)
    . = ALIGN(16);
    *(.data .data.*)
    *(.got .got.* .got.plt)
  } :data

  /* 多 hart 可能同时停在各自启动栈，普通 BSS 清零不得覆盖该段。 */
  .bss.stack (NOLOAD) : {
    . = ALIGN(16);
    *(.bss.stack)
  } :data

  .bss (NOLOAD) : {
    . = ALIGN(16);
    PROVIDE(bss_start = .);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
    /* runtime_init.cc 以 64 位字清零，结束符必须保持 8 字节对齐。 */
    . = ALIGN(8);
    PROVIDE(bss_end = .);
  } :data

  PROVIDE(end = .);

  /DISCARD/ : {
    *(.note.gnu.build-id)
    *(.comment)
  }
}
