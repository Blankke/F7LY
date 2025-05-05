OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

SECTIONS
{

  . = 0x80200000;

  .text : {
    *(.text.entry)
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    . = ALIGN(0x1000);
    PROVIDE(etext = .);
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.stack)
    . = ALIGN(16);
    *(.data .data.*)
    PROVIDE(stack0 = .);
  }

  .bss : {
    . = ALIGN(16);
    PROVIDE(bss_start = .);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
    PROVIDE(bss_end = .);
  }

  PROVIDE(end = .);
}
