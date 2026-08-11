OUTPUT_ARCH( "riscv" )
ENTRY( _riscv_image_header )

PHDRS
{
  text   PT_LOAD FLAGS(5); /* R + X */
  rodata PT_LOAD FLAGS(4); /* R */
  data   PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
  /* U-Boot/OpenSBI：load 到 0x40200000，a0=hartid，a1=DTB 后跳入 _entry。 */
  . = 0x40200000;
  PROVIDE(kernel_start = .);

  .text : {
    /* 必须位于 raw Image 的前 64 bytes，供 U-Boot booti 识别。 */
    KEEP(*(.text.image_header))
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
    *(.srodata .srodata.*)
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
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.stack)
    . = ALIGN(16);
    *(.data .data.*)
    *(.got .got.* .got.plt)
  } :data

  /* 多 hart 可能同时停在启动栈上，普通 BSS 清零不能覆盖该段。 */
  .bss.stack (NOLOAD) : {
    . = ALIGN(16);
    *(.bss.stack)
  } :data

  .bss (NOLOAD) : {
    . = ALIGN(16);
    PROVIDE(bss_start = .);
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    . = ALIGN(8);
    PROVIDE(bss_end = .);
  } :data

  PROVIDE(end = .);

  /DISCARD/ : {
    *(.note.gnu.build-id)
    *(.comment)
  }
}
