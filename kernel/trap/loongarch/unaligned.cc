#ifdef LOONGARCH

#include "trap/loongarch/unaligned.hh"

#include "mem/virtual_memory_manager.hh"
#include "proc/proc.hh"

namespace loongarch::unaligned
{
namespace
{
enum class AccessType
{
    Read,
    Write,
};

enum class RegisterFile
{
    General,
    FloatingPoint,
};

struct DecodedAccess
{
    AccessType type;
    RegisterFile register_file;
    uint8 size;
    uint8 reg;
    bool signed_load;
};

// LoongArch 指令编码，名称和体系结构手册/Linux inst.h 保持一致。
constexpr uint32 kLdh = 0xa1;
constexpr uint32 kLdhu = 0xa9;
constexpr uint32 kLdw = 0xa2;
constexpr uint32 kLdwu = 0xaa;
constexpr uint32 kLdd = 0xa3;
constexpr uint32 kSth = 0xa5;
constexpr uint32 kStw = 0xa6;
constexpr uint32 kStd = 0xa7;
constexpr uint32 kFlds = 0xac;
constexpr uint32 kFsts = 0xad;
constexpr uint32 kFldd = 0xae;
constexpr uint32 kFstd = 0xaf;

constexpr uint32 kLdptrw = 0x24;
constexpr uint32 kStptrw = 0x25;
constexpr uint32 kLdptrd = 0x26;
constexpr uint32 kStptrd = 0x27;

constexpr uint32 kLdxh = 0x7008;
constexpr uint32 kLdxw = 0x7010;
constexpr uint32 kLdxd = 0x7018;
constexpr uint32 kStxh = 0x7028;
constexpr uint32 kStxw = 0x7030;
constexpr uint32 kStxd = 0x7038;
constexpr uint32 kLdxhu = 0x7048;
constexpr uint32 kLdxwu = 0x7050;
constexpr uint32 kFldxs = 0x7060;
constexpr uint32 kFldxd = 0x7068;
constexpr uint32 kFstxs = 0x7070;
constexpr uint32 kFstxd = 0x7078;

bool decode(uint32 instruction, DecodedAccess &access)
{
    const uint32 op22 = instruction >> 22;
    const uint32 op24 = instruction >> 24;
    const uint32 op15 = instruction >> 15;
    access.reg = static_cast<uint8>(instruction & 0x1f);

    if (op22 == kLdd || op24 == kLdptrd || op15 == kLdxd)
        access = {AccessType::Read, RegisterFile::General, 8, access.reg, true};
    else if (op22 == kLdw || op24 == kLdptrw || op15 == kLdxw)
        access = {AccessType::Read, RegisterFile::General, 4, access.reg, true};
    else if (op22 == kLdwu || op15 == kLdxwu)
        access = {AccessType::Read, RegisterFile::General, 4, access.reg, false};
    else if (op22 == kLdh || op15 == kLdxh)
        access = {AccessType::Read, RegisterFile::General, 2, access.reg, true};
    else if (op22 == kLdhu || op15 == kLdxhu)
        access = {AccessType::Read, RegisterFile::General, 2, access.reg, false};
    else if (op22 == kStd || op24 == kStptrd || op15 == kStxd)
        access = {AccessType::Write, RegisterFile::General, 8, access.reg, false};
    else if (op22 == kStw || op24 == kStptrw || op15 == kStxw)
        access = {AccessType::Write, RegisterFile::General, 4, access.reg, false};
    else if (op22 == kSth || op15 == kStxh)
        access = {AccessType::Write, RegisterFile::General, 2, access.reg, false};
    else if (op22 == kFldd || op15 == kFldxd)
        access = {AccessType::Read, RegisterFile::FloatingPoint, 8, access.reg, false};
    else if (op22 == kFlds || op15 == kFldxs)
        // LoongArch/Linux 的 FLD.S 非对齐模拟会把 32 位结果符号扩展到
        // 完整 64 位 FPR，不能把高 32 位简单清零。
        access = {AccessType::Read, RegisterFile::FloatingPoint, 4, access.reg, true};
    else if (op22 == kFstd || op15 == kFstxd)
        access = {AccessType::Write, RegisterFile::FloatingPoint, 8, access.reg, false};
    else if (op22 == kFsts || op15 == kFstxs)
        access = {AccessType::Write, RegisterFile::FloatingPoint, 4, access.reg, false};
    else
        return false;

    return true;
}

uint64 read_general_register(const TrapFrame &frame, uint8 reg)
{
    switch (reg)
    {
    case 0: return 0;
    case 1: return frame.ra;
    case 2: return frame.tp;
    case 3: return frame.sp;
    case 4: return frame.a0;
    case 5: return frame.a1;
    case 6: return frame.a2;
    case 7: return frame.a3;
    case 8: return frame.a4;
    case 9: return frame.a5;
    case 10: return frame.a6;
    case 11: return frame.a7;
    case 12: return frame.t0;
    case 13: return frame.t1;
    case 14: return frame.t2;
    case 15: return frame.t3;
    case 16: return frame.t4;
    case 17: return frame.t5;
    case 18: return frame.t6;
    case 19: return frame.t7;
    case 20: return frame.t8;
    case 21: return frame.r21;
    case 22: return frame.fp;
    case 23: return frame.s0;
    case 24: return frame.s1;
    case 25: return frame.s2;
    case 26: return frame.s3;
    case 27: return frame.s4;
    case 28: return frame.s5;
    case 29: return frame.s6;
    case 30: return frame.s7;
    case 31: return frame.s8;
    default: return 0;
    }
}

void write_general_register(TrapFrame &frame, uint8 reg, uint64 value)
{
    // 显式映射体系结构寄存器，避免把结构体成员伪装成数组产生别名 UB。
    switch (reg)
    {
    case 0: return; // 对 r0 的写入必须丢弃。
    case 1: frame.ra = value; return;
    case 2: frame.tp = value; return;
    case 3: frame.sp = value; return;
    case 4: frame.a0 = value; return;
    case 5: frame.a1 = value; return;
    case 6: frame.a2 = value; return;
    case 7: frame.a3 = value; return;
    case 8: frame.a4 = value; return;
    case 9: frame.a5 = value; return;
    case 10: frame.a6 = value; return;
    case 11: frame.a7 = value; return;
    case 12: frame.t0 = value; return;
    case 13: frame.t1 = value; return;
    case 14: frame.t2 = value; return;
    case 15: frame.t3 = value; return;
    case 16: frame.t4 = value; return;
    case 17: frame.t5 = value; return;
    case 18: frame.t6 = value; return;
    case 19: frame.t7 = value; return;
    case 20: frame.t8 = value; return;
    case 21: frame.r21 = value; return;
    case 22: frame.fp = value; return;
    case 23: frame.s0 = value; return;
    case 24: frame.s1 = value; return;
    case 25: frame.s2 = value; return;
    case 26: frame.s3 = value; return;
    case 27: frame.s4 = value; return;
    case 28: frame.s5 = value; return;
    case 29: frame.s6 = value; return;
    case 30: frame.s7 = value; return;
    case 31: frame.s8 = value; return;
    default: return;
    }
}

uint64 sign_extend(uint64 value, uint8 size)
{
    if (size == 2)
        return static_cast<uint64>(static_cast<int64>(static_cast<int16>(value)));
    if (size == 4)
        return static_cast<uint64>(static_cast<int64>(static_cast<int32>(value)));
    return value;
}
}

Result emulate_user_access(proc::Pcb &process, uint64 address, uint32 instruction)
{
    DecodedAccess access{};
    if (!decode(instruction, access))
        return Result::UnsupportedInstruction;

    TrapFrame &frame = *process.get_trapframe();
    mem::PageTable &page_table = *process.get_pagetable();
    uint64 value = 0;

    if (access.type == AccessType::Read)
    {
        // copy_in 会处理跨页和惰性缺页；只有全部字节可读才提交寄存器结果。
        if (mem::k_vmm.copy_in(page_table, &value, address, access.size) < 0)
            return Result::MemoryFault;

        if (access.signed_load)
            value = sign_extend(value, access.size);

        if (access.register_file == RegisterFile::General)
        {
            write_general_register(frame, access.reg, value);
        }
        else
        {
            frame.f[access.reg] = value;
            if (process._used_lsx)
                frame.lsx[access.reg][0] = value;
        }
    }
    else
    {
        value = access.register_file == RegisterFile::General
                    ? read_general_register(frame, access.reg)
                    : frame.f[access.reg];

        // 先补齐并验证完整范围，避免跨页 store 写了一半才发现后一页不可写。
        if (mem::k_vmm.ensure_user_write_range(page_table, address, access.size) < 0 ||
            mem::k_vmm.copy_out(page_table, address, &value, access.size) < 0)
            return Result::MemoryFault;
    }

    frame.era += 4;
    return Result::Complete;
}
}

#endif
