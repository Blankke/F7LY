
OUTPUT_ARCH( "loongarch" )
ENTRY( _entry )

PHDRS
{
  text   PT_LOAD FLAGS(5); /* R + X */
  rodata PT_LOAD FLAGS(4); /* R */
  data   PT_LOAD FLAGS(6); /* R + W */
}

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
  } :text

    . = ALIGN(4K);

    .rodata : {
        srodata = .;
        *(.rodata .rodata.*)
        *(.rela .rela.*)
        erodata = .;
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

    . = ALIGN(4K);

    .data : {
        sdata = .;
        *(.data .data.*)
        *(.got .got.* .got.plt)
        edata = .;
    } :data

    . = ALIGN(4K);
    bss_start = .;
    .bss : {
        *(.bss.stack)
        sbss_clear = .;
        *(.sbss .bss .bss.*)
        /* entry.S 使用 st.d 清零，禁止最后一次写越过普通 BSS。 */
        . = ALIGN(8);
        ebss_clear = .;
    } :data
    bss_end = .;

    . = ALIGN(8);
    PROVIDE(end = .);

    /*
     * LoongArch 的内核镜像同样需要在生成 raw binary 前丢弃 build-id，
     * 否则 objcopy 会尝试保留这个 NOTE 段并在高地址布局下报 file truncated。
     * 这里和 RISC-V 保持一致，确保 loongarch-qemu 画像的产物链稳定。
     */
    /DISCARD/ : {
        *(.note.gnu.build-id)
        *(.comment)
    }
}
