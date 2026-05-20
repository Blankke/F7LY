#include "pte.hh"
#include "printer.hh"

namespace mem
{
    namespace
    {
        bool is_sane_pte_ptr(const pte_t *addr)
        {
            if (addr == nullptr)
            {
                return false;
            }

            const uint64 raw = reinterpret_cast<uint64>(addr);
            if ((raw & (sizeof(pte_t) - 1)) != 0)
            {
                return false;
            }

            return raw >= KERNBASE && raw < PHYSTOP;
        }
    }

    void Pte::set_addr(pte_t *addr)
    {
        _data_addr = addr;
    }

    bool Pte::is_null()
    {
        return _data_addr == 0;
    }

    bool Pte::is_valid()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            printfRed("[Pte::is_valid] 非法 PTE 指针: %p\n", _data_addr);
            return false;
        }
        return ((*_data_addr & riscv::PteEnum::pte_valid_m) != 0);
    }

    bool Pte::is_writable()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return false;
        }
        return ((*_data_addr & riscv::PteEnum::pte_writable_m) != 0);
    }

    bool Pte::is_readable()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return false;
        }
        return ((*_data_addr & riscv::PteEnum::pte_readable_m) != 0);
    }

    bool Pte::is_executable()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return false;
        }
        return ((*_data_addr & riscv::PteEnum::pte_executable_m) != 0);
    }

    bool Pte::is_user()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return false;
        }
        return ((*_data_addr & riscv::PteEnum::pte_user_m) != 0);
    }

    bool Pte::is_leaf() // TODO: 再次确认。这tm抄反了吧，原来是get_flags() == riscv::PteEnum::pte_valid_m;我觉得错了，comment by @gkq
    {
        return get_flags() != riscv::PteEnum::pte_valid_m;
    }

    void Pte::set_valid()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            printfRed("[Pte::set_valid] 非法 PTE 指针: %p\n", _data_addr);
            return;
        }
        *_data_addr |= riscv::PteEnum::pte_valid_m;
    }

    void Pte::set_writable()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return;
        }
        *_data_addr |= riscv::PteEnum::pte_writable_m;
    }

    void Pte::set_readable()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return;
        }
        *_data_addr |= riscv::PteEnum::pte_readable_m;
    }

    void Pte::set_executable()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return;
        }
        *_data_addr |= riscv::PteEnum::pte_executable_m;
    }

    void Pte::set_user()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return;
        }
        *_data_addr |= riscv::PteEnum::pte_user_m;
    }

    void Pte::set_data(uint64 data)
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            printfRed("[Pte::set_data] 非法 PTE 指针: %p\n", _data_addr);
            return;
        }
        *_data_addr |= data;
    }

    uint64 Pte::get_flags()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return 0;
        }
        return *_data_addr & 0x3FF;
    }

    void *Pte::pa()
    {
        return (void *)PTE2PA(get_data());
    }

    void Pte::clear_data()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            return;
        }
        *_data_addr = 0;
    }

    uint64 Pte::get_data()
    {
        if (!is_sane_pte_ptr(_data_addr))
        {
            printfRed("[Pte::get_data] 非法 PTE 指针: %p\n", _data_addr);
            return 0;
        }
        return *_data_addr;
    }
}
