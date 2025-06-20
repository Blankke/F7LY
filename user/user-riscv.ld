OUTPUT_ARCH( "riscv" )
ENTRY( main )

SECTIONS
{
 . = 0x0;
 
  .text : {
    *(.text.startup .text.startup.*)
    *(.text .text.*)
  }
  
  /DISCARD/ : {
    *(.note.gnu.build-id)
    *(.note.GNU-stack)
    *(.comment)
    *(.riscv.attributes)
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
