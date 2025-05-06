#include "pte.hh"

namespace mem
{
    
    
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
        return ((*_data_addr & riscv::PteEnum::pte_valid_m) != 0); 
    }
    
    bool Pte::is_writable() 
    { 
        return ((*_data_addr & riscv::PteEnum::pte_writable_m) != 0); 
    }
    
    bool Pte::is_readable() 
    { 
        return ((*_data_addr & riscv::PteEnum::pte_readable_m) != 0); 
    }
    
    bool Pte::is_executable() 
    { 
        return ((*_data_addr & riscv::PteEnum::pte_executable_m) != 0); 
    }
    
    bool Pte::is_user() 
    { 
        return ((*_data_addr & riscv::PteEnum::pte_user_m) != 0); 
    }

    bool Pte::is_leaf() 
    {
        return get_flags() == riscv::PteEnum::pte_valid_m;
    }


    
    void Pte::set_valid() 
    { 
        *_data_addr |= riscv::PteEnum::pte_valid_m; 
    }
    
    void Pte::set_writable() 
    { 
        *_data_addr |= riscv::PteEnum::pte_writable_m; 
    }
    
    void Pte::set_readable() 
    { 
        *_data_addr |= riscv::PteEnum::pte_readable_m; 
    }
    
    void Pte::set_executable() 
    { 
        *_data_addr |= riscv::PteEnum::pte_executable_m; 
    }
    
    void Pte::set_user() 
    { 
        *_data_addr |= riscv::PteEnum::pte_user_m; 
    }
    
    void Pte::set_data(uint64 data) 
    { 
        *_data_addr |= data; 
    }
    
    uint64 Pte::get_flags() 
    { 
        return *_data_addr & 0x3FF; 
    }
    
    void *Pte::pa() 
    { 
        return (void*)PTE2PA(get_data()); 
    }

    void Pte::clear_data() 
    { 
        *_data_addr = 0; 
    }
    
    uint64 Pte::get_data() 
    { 
        return *_data_addr; 
    }
}
