#include "smp.hh"

#include "mem/memlayout.hh"

namespace loongarch::smp
{
namespace
{
    // QEMU LoongArch virt 的 slave boot code 等待 MAIL_BUF0 非零，并由 IPI
    // 唤醒后跳转到该地址。MAIL_BUF1 保留给次核启动参数。
    constexpr uint64 k_iocsr_ipi_send = 0x1040;
    constexpr uint64 k_iocsr_mailbox_send = 0x1048;
    constexpr uint64 k_iocsr_send_blocking = 1ULL << 31;
    constexpr uint64 k_iocsr_target_cpu_shift = 16;
    constexpr uint64 k_iocsr_mailbox_shift = 2;
    constexpr uint64 k_iocsr_high_word_mask = 0xffffffff00000000ULL;

    // QEMU 的 mailbox 传输寄存器一次只写目标 mailbox 的 32 位。BOX 的
    // 编码是 mailbox*2（低 32 位）或 mailbox*2+1（高 32 位）。
    inline void write_mailbox(uint64 cpu_id, uint64 mailbox, uint64 value)
    {
        const uint64 target = k_iocsr_send_blocking |
                              (cpu_id << k_iocsr_target_cpu_shift);
        const uint64 high_word = target |
                                 (((mailbox * 2 + 1) << k_iocsr_mailbox_shift)) |
                                 (value & k_iocsr_high_word_mask);
        const uint64 low_word = target |
                                ((mailbox * 2) << k_iocsr_mailbox_shift) |
                                (value << 32);

        asm volatile("iocsrwr.d %0, %1" : : "r"(high_word), "r"(k_iocsr_mailbox_send) : "memory");
        asm volatile("iocsrwr.d %0, %1" : : "r"(low_word), "r"(k_iocsr_mailbox_send) : "memory");
    }

    inline void send_boot_ipi(uint64 cpu_id)
    {
        // action/vector 0 是 QEMU slave boot ROM 使用的唤醒位。IOCSR_IPI_SEND
        // 的控制位宽为 32 位，使用 iocsrwr.w 与设备寄存器宽度保持一致。
        const uint32 value = static_cast<uint32>(k_iocsr_send_blocking |
                                                  (cpu_id << k_iocsr_target_cpu_shift));
        asm volatile("iocsrwr.w %0, %1" : : "r"(value), "r"(k_iocsr_ipi_send) : "memory");
    }
}

void start_secondary_cpu(uint64 cpu_id, uint64 entry, uint64 argument)
{
    // QEMU 的 slave boot ROM 在 DA 模式下直接 jirl 到 MAIL_BUF0。转换到
    // DMW1 的缓存别名后，次核可直接复用内核的 _entry 初始化代码。
    const uint64 cached_entry = VIRT2PHY(entry) | DMWIN_MASK;

    // 先把所有由次核观察的内存状态发布出去，再写 mailbox 并发 IPI；这与
    // LoongArch 的 dbar 0 启动序列保持一致。
    asm volatile("dbar 0" ::: "memory");
    write_mailbox(cpu_id, 1, argument);
    write_mailbox(cpu_id, 0, cached_entry);
    asm volatile("dbar 0" ::: "memory");
    send_boot_ipi(cpu_id);
}
}
