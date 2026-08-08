OUTPUT_ARCH("loongarch")
ENTRY(_entry)

/*
 * LS2K1000 的板载内存从物理地址 0x00200000 开始可用。内核通过缓存 DMW
 * 别名执行，因此 U-Boot 应把 raw binary 加载到物理地址 0x00200000，随后
 * 跳转到其缓存 DMW 别名 0x9000000000200000。
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
  }

  . = ALIGN(8);
  PROVIDE(end = .);

  /DISCARD/ : {
    *(.note.gnu.build-id)
    *(.comment)
  }
}
