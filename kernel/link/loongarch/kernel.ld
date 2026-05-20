
OUTPUT_ARCH( "loongarch" )
ENTRY( _entry )

SECTIONS
{
  . = 0x9000000000200000;

  kernel_start = .;

  . = ALIGN(4K);

  .text : AT(0x200000) {
    *(.text .text.*)
    . = ALIGN(0x1000);
    _keentry = .;
    *(k_eentry)
    . = ALIGN(0x1000);
    _ueentry = .;
    *(u_eentry)
    . = ALIGN(0x1000);
    _tlbrentry = .;
    *(tlbrentry)
    . = ALIGN(0x1000);
    _merrentry = .;
    *(merrentry)
    . = ALIGN(0x1000);
    _sig_trampoline = .;
    *(sigtrampsec)
    . = ALIGN(0x1000);
    PROVIDE(etext = .);
  }

    . = ALIGN(4K);

    .rodata : {
        srodata = .;
        *(.rodata .rodata.*)
        erodata = .;
    }

    . = ALIGN(4K);

    .data : {
        sdata = .;
        *(.data .data.*)
        edata = .;
    }

    . = ALIGN(4K);
    bss_start = .;
    .bss : {
        *(.bss.stack)
        sbss_clear = .;
        *(.sbss .bss .bss.*)
        ebss_clear = .;
    }
    bss_end = .;

    . = ALIGN(8);
    PROVIDE(end = .);

    /*
     * LoongArch 的内核镜像同样需要在生成 raw binary 前丢弃 build-id，
     * 否则 objcopy 会尝试保留这个 NOTE 段并在高地址布局下报 file truncated。
     * 这里和 RISC-V 保持一致，确保 make build/run l 的产物链稳定。
     */
    /DISCARD/ : {
        *(.note.gnu.build-id)
        *(.comment)
    }
}
