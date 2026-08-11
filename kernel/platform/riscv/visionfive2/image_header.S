.section .text.image_header, "ax"
.global _riscv_image_header
.type _riscv_image_header, @function

/*
 * U-Boot booti 识别的标准 RISC-V Linux Image 头（version 0.2）。
 * VisionFive 2 的 DRAM 从 0x40000000 开始，S-mode 镜像放在 +2 MiB，
 * 因此 text_offset 必须为 0x200000。booti 随后按标准 ABI 进入这里：
 * a0=启动 hartid，a1=DTB 物理地址。
 *
 * 次核仍由 SBI HSM 直接进入 _entry，避免再次经过镜像头。
 */
_riscv_image_header:
        j _entry
        .word 0
        .balign 8
        .dword 0x200000
        .dword end - _riscv_image_header
        .dword 0
        .word 2
        .word 0
        .dword 0
        .ascii "RISCV\0\0\0"
        .balign 4
        .ascii "RSC\005"
        .word 0

.size _riscv_image_header, . - _riscv_image_header
