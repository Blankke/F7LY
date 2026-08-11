OUTPUT_ARCH("loongarch")
ENTRY(_entry)

PHDRS
{
  text   PT_LOAD FLAGS(5); /* R + X */
  rodata PT_LOAD FLAGS(4); /* R */
  data   PT_LOAD FLAGS(6); /* R + W */
}

/*
 * 内核的链接物理坐标从 0x00200000 开始，并通过缓存 DMW 别名执行。
 * 2K1000LA U-Boot 的 DRAM bank/LMB 使用高缓存地址，因此 tftpboot 与 go
 * 都应传 0x9000000000200000；它与物理 0x00200000 指向同一块 RAM。
 * 板级脚本独立存在，即使当前 LMA 与 QEMU 一致，后续调整上板装载契约也不会
 * 意外改变 QEMU 产物。
 */
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

  /*
   * LS2K1000 的 AHCI/GMAC DMA 不与 CPU cache 保持硬件一致。该段故意不在
   * entry.S 的缓存别名 BSS 清零范围内，驱动只通过 0x8 DMW 非缓存别名访问，
   * 避免早期脏 cache line 在设备运行后回写并覆盖描述符或数据。
   */
  .dma_uncached (NOLOAD) : {
    . = ALIGN(64);
    dma_uncached_start = .;
    *(.dma_uncached .dma_uncached.*)
    . = ALIGN(64);
    dma_uncached_end = .;
  } :data

  . = ALIGN(8);
  PROVIDE(end = .);

  /DISCARD/ : {
    *(.note.gnu.build-id)
    *(.comment)
  }
}
