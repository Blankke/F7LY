#pragma once


namespace riscv
{
	namespace qemu
	{
		// Physical memory layout

		// k210 peripherals
		// (0x0200_0000, 0x1000),      /* CLINT     */
		// // we only need claim/complete for target0 after initializing
		// (0x0C20_0000, 0x1000),      /* PLIC      */
		// (0x3800_0000, 0x1000),      /* UARTHS    */
		// (0x3800_1000, 0x1000),      /* GPIOHS    */
		// (0x5020_0000, 0x1000),      /* GPIO      */
		// (0x5024_0000, 0x1000),      /* SPI_SLAVE */
		// (0x502B_0000, 0x1000),      /* FPIOA     */
		// (0x502D_0000, 0x1000),      /* TIMER0    */
		// (0x502E_0000, 0x1000),      /* TIMER1    */
		// (0x502F_0000, 0x1000),      /* TIMER2    */
		// (0x5044_0000, 0x1000),      /* SYSCTL    */
		// (0x5200_0000, 0x1000),      /* SPI0      */
		// (0x5300_0000, 0x1000),      /* SPI1      */
		// (0x5400_0000, 0x1000),      /* SPI2      */
		// (0x8000_0000, 0x600000),    /* Memory    */

		// qemu -machine virt is set up like this,
		// based on qemu's hw/riscv/virt.c:
		//
		// 00001000 -- boot ROM, provided by qemu
		// 02000000 -- CLINT
		// 0C000000 -- PLIC
		// 10000000 -- uart0 
		// 10001000 -- virtio disk 
		// 80000000 -- boot ROM jumps here in machine mode
		//             -kernel loads the kernel here
		// unused RAM after 80000000.

		// #define VIRT_OFFSET             0x3F00000000L

		// // qemu puts UART registers here in physical memory.
		// #define UART                    0x10000000L

		// #define UART_V                  (UART + VIRT_OFFSET)

		// // virtio mmio interface
		// #define VIRTIO0                 0x10001000L
		// #define VIRTIO0_V               (VIRTIO0 + VIRT_OFFSET)

		// // local interrupt controller, which contains the timer.
		// #define CLINT                   0x02000000L
		// #define CLINT_V                 (CLINT + VIRT_OFFSET)

		// #define PLIC                    0x0c000000L
		// #define PLIC_V                  (PLIC + VIRT_OFFSET)

		// // #define PLIC_PRIORITY           (PLIC_V + 0x0)
		// #define PLIC_PENDING            (PLIC_V + 0x1000)
		// #define PLIC_SENABLE(hart)      (PLIC_V + 0x2080 + (hart) * 0x100)
		// #define PLIC_SPRIORITY(hart)    (PLIC_V + 0x201000 + (hart) * 0x2000)
		// #define PLIC_SCLAIM(hart)       (PLIC_V + 0x201004 + (hart) * 0x2000)

		// // the physical address of rustsbi
		// #define RUSTSBI_BASE            0x80000000L

		// // the kernel expects there to be RAM
		// // for use by the kernel and user pages
		// // from physical address 0x80200000 to PHYSTOP.
		// #define KERNBASE                0x80200000L

		// #define PHYSTOP                 0x88000000L

		// #define MAXUVA                  RUSTSBI_BASE

	} // namespace qemu

} // namespace riscv
