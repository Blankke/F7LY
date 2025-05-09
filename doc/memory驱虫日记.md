# buddy修复
buddy tree节点转移不对，现已修复
# walk & map_page
在riscv中，tlb无需初始化，因此第二个sfence卡死并非由tlb导致，而是由w_satp导致的，经检查，satp设置无误。卡死原因是，执行 `w_satp()` 后，MMU 开始使用新页表，但新页表（`k_pagetable`）未正确映射内核代码所在的虚拟地址区域（如 `PC` 指向的指令地址），导致后续指令无法正常执行。

==w_satp的作用是开启虚拟化==
经查： kvmmap(pt, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);并没有达到预期的映射效果
map失效

内核代码段没有正确映射，导致虚拟化后PC丢失
```
etext: 0x0000000080206000
KERNBASE: 0xffffffff80200000
try to walk-addr( k-pt, 0x0000000080200000 ). nullptr will be return.va: 0x0000000080200000, pa: 0x0000000000000000
try to walk-addr( k-pt, 0x0000000080201000 ). nullptr will be return.va: 0x0000000080201000, pa: 0x0000000000000000
try to walk-addr( k-pt, 0x0000000080202000 ). nullptr will be return.va: 0x0000000080202000, pa: 0x0000000000000000
try to walk-addr( k-pt, 0x0000000080203000 ). nullptr will be return.va: 0x0000000080203000, pa: 0x0000000000000000
try to walk-addr( k-pt, 0x0000000080204000 ). nullptr will be return.va: 0x0000000080204000, pa: 0x0000000000000000
try to walk-addr( k-pt, 0x0000000080205000 ). nullptr will be return.va: 0x0000000080205000, pa: 0x0000000000000000
```
所以问题仍然是map问题

检查walk函数，发现pte地址未设置正确，修改如下
```cpp
        return_pte.set_addr((uint64*)(current_pt.get_base() + 8*PX(0, va)));
        return return_pte;
```
至此，检查发现前两级页表映射正确，walk函数映射正常，第三级页表全空，故将bug定位到map_page上,未正确应用PA2PTE，导致pte错位，修复如下
```cpp
pte.set_data(PA2PTE(PGROUNDDOWN(riscv::virt_to_phy_address(pa))) |
                                flags |
                                riscv::PteEnum::pte_valid_m);
```


