/**
 * @file process_memory_manager.cc
 * @brief 进程内存管理器实现
 *
 * 实现进程内存管理器的所有功能，提供统一的内存管理接口。
 * 将原本散落在proc_manager.cc中的内存管理逻辑重构到这里。
 */
#include "proc_manager.hh"
#include "process_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include "platform.hh" // 为MAX/MIN宏
#include "fs/vfs/file/normal_file.hh"

namespace proc
{

    ProcessMemoryManager::ProcessMemoryManager(Pcb *pcb) : _pcb(pcb)
    {
        if (_pcb == nullptr)
        {
            panic("ProcessMemoryManager: null PCB provided");
        }
    }

    ProcessMemoryManager::~ProcessMemoryManager()
    {
        // 析构函数中不执行清理操作，避免双重释放
        // 清理应该通过显式调用free_all_memory()来完成
    }

    /****************************************************************************************
     * 程序段管理接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::free_all_program_sections()
    {
        if (!_pcb)
            return;

        printfBlue("ProcessMemoryManager: freeing all program sections for process %s (PID: %d)\n",
                   _pcb->get_name(), _pcb->get_pid());

        // 释放程序段占用的内存
        for (int i = 0; i < _pcb->get_prog_section_count(); i++)
        {
            const auto *sections = _pcb->get_prog_sections();
            if (sections[i]._sec_start && sections[i]._sec_size > 0)
            {
                uint64 va_start = PGROUNDDOWN((uint64)sections[i]._sec_start);
                uint64 va_end = PGROUNDUP((uint64)sections[i]._sec_start + sections[i]._sec_size);

                printfBlue("  Freeing section %d (%s): %p - %p (%u bytes)\n",
                           i,
                           sections[i]._debug_name ? sections[i]._debug_name : "unnamed",
                           (void *)va_start,
                           (void *)va_end,
                           sections[i]._sec_size);

                safe_vmunmap(va_start, va_end, true);
            }
        }

        // 清理程序段描述信息
        _pcb->clear_all_program_sections();

        printfGreen("ProcessMemoryManager: program sections freed successfully\n");
    }

    bool ProcessMemoryManager::free_program_section(int section_index)
    {
        if (!_pcb || section_index < 0 || section_index >= _pcb->get_prog_section_count())
        {
            printfRed("ProcessMemoryManager: invalid section index %d\n", section_index);
            return false;
        }

        const auto *sections = _pcb->get_prog_sections();
        if (sections[section_index]._sec_start && sections[section_index]._sec_size > 0)
        {
            uint64 va_start = PGROUNDDOWN((uint64)sections[section_index]._sec_start);
            uint64 va_end = PGROUNDUP((uint64)sections[section_index]._sec_start + sections[section_index]._sec_size);

            safe_vmunmap(va_start, va_end, true);
        }

        _pcb->remove_program_section(section_index);
        return true;
    }

    bool ProcessMemoryManager::verify_program_sections_consistency() const
    {
        if (!_pcb)
            return false;

        // 计算当前程序段的总大小
        uint64 sections_total = 0;
        for (int i = 0; i < _pcb->get_prog_section_count(); i++)
        {
            const auto *sections = _pcb->get_prog_sections();
            sections_total += sections[i]._sec_size;
        }

        // 与PCB维护的总内存大小进行比较
        // 注意：_sz包含程序段+堆，但不包含VMA
        uint64 expected_sections_total = _pcb->get_size() - _pcb->get_heap_size();

        if (sections_total != expected_sections_total)
        {
            printfRed("ProcessMemoryManager: program sections inconsistency detected\n");
            printfRed("  Sections total: %u, Expected (sz - heap): %u\n",
                      sections_total, expected_sections_total);
            printfRed("  PCB size: %u, Heap size: %u\n",
                      _pcb->get_size(), _pcb->get_heap_size());
            panic("verify_program_section_count fail");
            return false;
        }

        return true;
    }

    /****************************************************************************************
     * 堆内存管理接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::init_heap(uint64 start_addr)
    {
        if (!_pcb)
            return;

        printfBlue("ProcessMemoryManager: initializing heap at %p for process %s (PID: %d)\n",
                   (void *)start_addr, _pcb->get_name(), _pcb->get_pid());

        // 设置堆的起始和结束地址
        _pcb->set_heap_start(start_addr);
        _pcb->set_heap_end(start_addr);

        printfGreen("ProcessMemoryManager: heap initialized successfully\n");
    }

    uint64 ProcessMemoryManager::grow_heap(uint64 new_end)
    {
        if (!_pcb)
            return 0;

        uint64 current_end = _pcb->get_heap_end();
        if (new_end <= current_end)
        {
            return current_end; // 无需扩展
        }

        printfBlue("ProcessMemoryManager: growing heap from %p to %p for process %s (PID: %d)\n",
                   (void *)current_end, (void *)new_end, _pcb->get_name(), _pcb->get_pid());

        // 使用虚拟内存管理器分配新的堆内存
        mem::PageTable &pt = *_pcb->get_pagetable();
        uint64 result;

#ifdef RISCV
        result = mem::k_vmm.vmalloc(pt, current_end, new_end, PTE_W | PTE_R | PTE_U);
#elif defined(LOONGARCH)
        result = mem::k_vmm.vmalloc(pt, current_end, new_end, PTE_P | PTE_W | PTE_PLV | PTE_MAT | PTE_D);
#endif

        if (result < new_end)
        {
            // 分配失败
            printfRed("ProcessMemoryManager: heap grow failed, vmalloc returned %p\n", (void *)result);
            return current_end;
        }

        // 更新堆结束地址
        _pcb->set_heap_end(new_end);

        printfGreen("ProcessMemoryManager: heap grown successfully to %p\n", (void *)new_end);
        return new_end;
    }

    uint64 ProcessMemoryManager::shrink_heap(uint64 new_end)
    {
        if (!_pcb)
            return 0;

        uint64 current_end = _pcb->get_heap_end();
        uint64 heap_start = _pcb->get_heap_start();

        if (new_end >= current_end || new_end < heap_start)
        {
            return current_end; // 无效的收缩请求
        }

        printfBlue("ProcessMemoryManager: shrinking heap from %p to %p for process %s (PID: %d)\n",
                   (void *)current_end, (void *)new_end, _pcb->get_name(), _pcb->get_pid());

        // 释放多余的堆内存
        uint64 va_start = PGROUNDUP(new_end);
        uint64 va_end = PGROUNDUP(current_end);

        for (uint64 va = va_start; va < va_end; va += PGSIZE)
        {
            if (is_page_mapped(va))
            {
                mem::k_vmm.vmunmap(*_pcb->get_pagetable(), va, 1, 1);
            }
        }

        // 更新堆结束地址
        _pcb->set_heap_end(new_end);

        printfGreen("ProcessMemoryManager: heap shrunk successfully to %p\n", (void *)new_end);
        return new_end;
    }

    void ProcessMemoryManager::free_heap_memory()
    {
        if (!_pcb)
            return;

        uint64 heap_size = _pcb->get_heap_size();
        if (heap_size > 0)
        {
            printfBlue("ProcessMemoryManager: freeing heap memory for process %s (PID: %d)\n",
                       _pcb->get_name(), _pcb->get_pid());
            printfBlue("  Heap range: %p - %p (%u bytes)\n",
                       (void *)_pcb->get_heap_start(),
                       (void *)_pcb->get_heap_end(),
                       heap_size);

            // 将堆收缩到起始位置，实际释放所有堆内存
            shrink_heap(_pcb->get_heap_start());

            printfGreen("ProcessMemoryManager: heap memory freed successfully\n");
        }
    }

    bool ProcessMemoryManager::cleanup_heap_to_size(uint64 new_size)
    {
        if (!_pcb)
            return false;

        uint64 current_size = _pcb->get_heap_size();
        if (new_size >= current_size)
        {
            return true; // 无需收缩
        }

        uint64 new_end = _pcb->get_heap_start() + new_size;
        uint64 result = shrink_heap(new_end);

        return (result == new_end);
    }

    /****************************************************************************************
     * VMA管理接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::free_all_vma()
    {
        if (!_pcb || !_pcb->_vma)
        {
            panic("ProcessMemoryManager: PCB or VMA is null");
            return;
        }

        printfBlue("ProcessMemoryManager: freeing all VMA for process %s (PID: %d)\n",
                   _pcb->get_name(), _pcb->get_pid());

        // 遍历所有VMA条目
        for (int i = 0; i < NVMA; ++i)
        {
            if (_pcb->_vma->_vm[i].used)
            {
                printfBlue("  Processing VMA %d: addr=%p, len=%u, vfd=%d, flags=0x%x, prot=0x%x\n",
                           i, (void *)_pcb->_vma->_vm[i].addr, _pcb->_vma->_vm[i].len,
                           _pcb->_vma->_vm[i].vfd, _pcb->_vma->_vm[i].flags, _pcb->_vma->_vm[i].prot);

                if (_pcb->_vma->_vm[i].vfile)
                {
                    printfBlue("    File mapping: %s\n", _pcb->_vma->_vm[i].vfile->_path_name.c_str());
                }
                else
                {
                    printfBlue("    Anonymous mapping (vfd=%d)\n", _pcb->_vma->_vm[i].vfd);
                }

                // 对文件映射进行写回操作
                if (!writeback_vma(i))
                {
                    printfYellow("  Warning: VMA %d writeback failed\n", i);
                }

                // 释放文件引用
                if (_pcb->_vma->_vm[i].vfile != nullptr)
                {
                    _pcb->_vma->_vm[i].vfile->free_file();
                    _pcb->_vma->_vm[i].vfile = nullptr;
                }

                // 取消虚拟地址映射
                uint64 va_start = PGROUNDDOWN(_pcb->_vma->_vm[i].addr);
                uint64 va_end = PGROUNDUP(_pcb->_vma->_vm[i].addr + _pcb->_vma->_vm[i].len);
                safe_vmunmap(va_start, va_end, true);

                // 标记为未使用
                _pcb->_vma->_vm[i].used = 0;
            }
        }

        printfGreen("ProcessMemoryManager: all VMA freed successfully\n");
    }

    bool ProcessMemoryManager::free_vma(int vma_index)
    {
        if (!is_vma_valid(vma_index))
        {
            return false;
        }

        vma &vm_entry = _pcb->_vma->_vm[vma_index];

        // 写回文件映射
        if (!writeback_vma(vma_index))
        {
            printfYellow("ProcessMemoryManager: VMA %d writeback failed\n", vma_index);
        }

        // 释放文件引用
        if (vm_entry.vfile != nullptr)
        {
            vm_entry.vfile->free_file();
            vm_entry.vfile = nullptr;
        }

        // 取消虚拟地址映射
        uint64 va_start = PGROUNDDOWN(vm_entry.addr);
        uint64 va_end = PGROUNDUP(vm_entry.addr + vm_entry.len);
        safe_vmunmap(va_start, va_end, true);

        // 清理VMA条目
        memset(&vm_entry, 0, sizeof(vma));

        return true;
    }

    bool ProcessMemoryManager::writeback_vma(int vma_index)
    {
        if (!is_vma_valid(vma_index))
        {
            return false;
        }

        printf("checkpoint: 0\n");

        const vma &vm_entry = _pcb->_vma->_vm[vma_index];
        printf("checkpoint: 0.5\n");

        // 检查是否是匿名映射（没有关联文件）
        if (vm_entry.vfile == nullptr)
        {
            printf("ProcessMemoryManager: VMA %d is anonymous mapping (no file), skipping writeback\n", vma_index);
            return true;
        }

        printf("ProcessMemoryManager: writeback VMA %d to file %s\n",
               vma_index, vm_entry.vfile->_path_name.c_str());

        printf("checkpoint: 1\n");

        // 跳过temp文件
        // if (vm_entry.vfile->_path_name.substr(0, 5) == "/tmp/")
        // {
        //     printfOrange("[freeproc] skipping tmp writeback\n");
        //     return false;
        // }

        printf("checkpoint: 2\n");

        // 只对文件映射且为共享且可写的VMA进行写回
        if (vm_entry.flags == MAP_SHARED &&
            (vm_entry.prot & PROT_WRITE) != 0)
        {
            Pcb* p=k_pm.get_cur_pcb();
            printfBlue("  Writing back VMA %d to file\n", vma_index);
            uint64 vma_start = PGROUNDDOWN(vm_entry.addr);
            uint64 vma_end = PGROUNDUP(vma_start + vm_entry.len);
            for (uint64 va = vma_start; va < vma_end; va += PGSIZE)
            {
                mem::Pte pte = p->_pt.walk(va, 0);
                if (!pte.is_null() && pte.is_valid())
                {
                    // 页面已分配，需要写回到文件
                    uint64 pa = (uint64)pte.pa();
                    int file_offset = vm_entry.offset + (va - vma_start);

                    printfCyan("[SyscallHandler::sys_msync] Writing back page at va=%p, file_offset=%d\n",
                               (void *)va, file_offset);

                    // 写回数据到文件
                    int write_result = vm_entry.vfile->write(pa, PGSIZE, file_offset, false);
                    if (write_result < 0)
                    {
                        printfRed("[SyscallHandler::sys_msync] Failed to write back page at va=%p\n", (void *)va);
                        return -EIO;
                    }
                }
            }
        }
        return true;
    }
        bool ProcessMemoryManager::decrease_vma_refcount_and_free()
        {
            if (!_pcb || !_pcb->_vma)
                return false;

            // 减少引用计数
            --_pcb->_vma->_ref_cnt;

            printfBlue("ProcessMemoryManager: VMA ref count decreased to %d\n", _pcb->_vma->_ref_cnt);

            // 如果引用计数为0，则释放VMA
            if (_pcb->_vma->_ref_cnt <= 0)
            {
                free_all_vma();
                delete _pcb->_vma;
                _pcb->_vma = nullptr;
                _pcb->_shared_vm = false;

                printfGreen("ProcessMemoryManager: VMA structure freed\n");
                return true;
            }
            else
            {
                printfYellow("ProcessMemoryManager: VMA still referenced, not freeing\n");
                return false;
            }
        }

        int ProcessMemoryManager::unmap_memory_range(void *addr, size_t length)
        {
            if (!_pcb || !addr || length == 0)
            {
                return -1;
            }

            // 检查地址对齐
            if ((uint64)addr % PGSIZE != 0)
            {
                printfRed("ProcessMemoryManager: unmap address not page aligned: %p\n", addr);
                return -1;
            }

            uint64 start_addr = (uint64)addr;
            uint64 aligned_length = PGROUNDUP(length);
            uint64 end_addr = start_addr + aligned_length;

            // 检查地址范围溢出
            if (end_addr < start_addr)
            {
                printfRed("ProcessMemoryManager: address range overflow\n");
                return -1;
            }

            printfYellow("ProcessMemoryManager: unmapping range [%p, %p) length=%u\n",
                         addr, (void *)end_addr, aligned_length);

            // 查找重叠的VMA
            int overlapping_vmas[NVMA];
            int overlap_count = find_overlapping_vmas(start_addr, end_addr, overlapping_vmas, NVMA);

            if (overlap_count == 0)
            {
                printfYellow("ProcessMemoryManager: no VMA found for unmapping range\n");
                // 仍然尝试取消页表映射，以防有非VMA管理的映射
                safe_vmunmap(start_addr, end_addr, true);
                return 0;
            }

            // 处理每个重叠的VMA
            for (int i = 0; i < overlap_count; i++)
            {
                int vma_idx = overlapping_vmas[i];
                vma &vm_entry = _pcb->_vma->_vm[vma_idx];

                uint64 vma_start = vm_entry.addr;
                uint64 vma_end = vm_entry.addr + vm_entry.len;

                printfCyan("ProcessMemoryManager: processing overlapping VMA %d: [%p, %p)\n",
                           vma_idx, (void *)vma_start, (void *)vma_end);

                // 计算需要取消映射的区域
                uint64 unmap_start = start_addr > vma_start ? start_addr : vma_start;
                uint64 unmap_end = end_addr < vma_end ? end_addr : vma_end;

                // 如果需要写回文件映射
                if (vm_entry.vfile != nullptr &&
                    vm_entry.flags == MAP_SHARED &&
                    (vm_entry.prot & PROT_WRITE) != 0)
                {
                    printfCyan("ProcessMemoryManager: writing back shared file mapping\n");
                    writeback_vma(vma_idx);
                }

                // 取消页表映射
                safe_vmunmap(unmap_start, unmap_end, true);

                // 处理VMA条目的更新
                if (unmap_start == vma_start && unmap_end == vma_end)
                {
                    // 完全取消映射
                    printfCyan("ProcessMemoryManager: completely unmapping VMA %d\n", vma_idx);
                    if (vm_entry.vfile)
                    {
                        vm_entry.vfile->free_file();
                    }
                    memset(&vm_entry, 0, sizeof(vma));
                }
                else
                {
                    // 部分取消映射
                    if (!partial_unmap_vma(vma_idx, unmap_start, unmap_end))
                    {
                        printfRed("ProcessMemoryManager: partial unmap failed for VMA %d\n", vma_idx);
                        return -1;
                    }
                }
            }

            // 检查是否需要调整堆指针
            uint64 heap_start = _pcb->get_heap_start();
            uint64 heap_end = _pcb->get_heap_end();

            if (start_addr <= heap_end && end_addr > heap_start)
            {
                // 取消映射的区域与堆重叠，需要调整堆大小
                if (start_addr <= heap_start)
                {
                    // 从堆开始位置或更早开始取消映射
                    _pcb->set_heap_end(heap_start);
                    printfYellow("ProcessMemoryManager: reset heap_end to heap_start\n");
                }
                else if (start_addr < heap_end)
                {
                    // 从堆中间开始取消映射
                    _pcb->set_heap_end(start_addr);
                    printfYellow("ProcessMemoryManager: shrunk heap_end to %p\n", (void *)start_addr);
                }
            }

            return 0;
        }

        int ProcessMemoryManager::find_overlapping_vmas(uint64 start_addr, uint64 end_addr,
                                                        int overlapping_vmas[], int max_count)
        {
            if (!_pcb || !_pcb->_vma || !overlapping_vmas)
            {
                return 0;
            }

            int count = 0;
            for (int i = 0; i < NVMA && count < max_count; i++)
            {
                if (_pcb->_vma->_vm[i].used)
                {
                    uint64 vma_start = _pcb->_vma->_vm[i].addr;
                    uint64 vma_end = vma_start + _pcb->_vma->_vm[i].len;

                    // 检查是否有重叠
                    if (start_addr < vma_end && end_addr > vma_start)
                    {
                        overlapping_vmas[count++] = i;
                    }
                }
            }

            return count;
        }

        bool ProcessMemoryManager::partial_unmap_vma(int vma_index, uint64 unmap_start, uint64 unmap_end)
        {
            if (!is_vma_valid(vma_index))
            {
                return false;
            }

            vma &vm_entry = _pcb->_vma->_vm[vma_index];
            uint64 vma_start = vm_entry.addr;
            uint64 vma_end = vm_entry.addr + vm_entry.len;

            if (unmap_start == vma_start && unmap_end < vma_end)
            {
                // 从VMA开始处取消映射
                printfCyan("ProcessMemoryManager: unmapping from start of VMA %d\n", vma_index);
                vm_entry.addr = unmap_end;
                vm_entry.len = vma_end - unmap_end;
                if (vm_entry.vfile)
                {
                    vm_entry.offset += (unmap_end - vma_start);
                }
                return true;
            }
            else if (unmap_start > vma_start && unmap_end == vma_end)
            {
                // 从VMA末尾取消映射
                printfCyan("ProcessMemoryManager: unmapping from end of VMA %d\n", vma_index);
                vm_entry.len = unmap_start - vma_start;
                return true;
            }
            else if (unmap_start > vma_start && unmap_end < vma_end)
            {
                // 从VMA中间取消映射（需要分割VMA）
                printfRed("ProcessMemoryManager: middle unmapping not fully supported yet\n");
                // 这里可以实现VMA分割逻辑，但比较复杂
                return false;
            }

            return false;
        }

        /****************************************************************************************
         * 页表管理接口实现
         ****************************************************************************************/

        void ProcessMemoryManager::free_pagetable()
        {
            if (!_pcb || !_pcb->get_pagetable()->get_base())
            {
                return;
            }

            printfBlue("ProcessMemoryManager: freeing pagetable for process %s (PID: %d)\n",
                       _pcb->get_name(), _pcb->get_pid());

            mem::PageTable &pt = *_pcb->get_pagetable();

// 取消特殊页面的映射
#ifdef RISCV
            mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
#endif
            mem::k_vmm.vmunmap(pt, TRAPFRAME, 1, 0);
            mem::k_vmm.vmunmap(pt, SIG_TRAMPOLINE, 1, 0);

            // 释放页表结构
            pt.dec_ref();

            printfGreen("ProcessMemoryManager: pagetable freed successfully\n");
        }

        void ProcessMemoryManager::safe_vmunmap(uint64 va_start, uint64 va_end, bool check_validity)
        {
            if (!_pcb || !_pcb->get_pagetable()->get_base())
            {
                return;
            }

            // 确保地址对齐到页边界
            va_start = PGROUNDDOWN(va_start);
            va_end = PGROUNDUP(va_end);

            for (uint64 va = va_start; va < va_end; va += PGSIZE)
            {
                if (check_validity)
                {
                    mem::Pte pte = _pcb->get_pagetable()->walk(va, 0);
                    if (!pte.is_null() && pte.is_valid())
                    {
                        mem::k_vmm.vmunmap(*_pcb->get_pagetable(), va, 1, 1);
                    }
                }
                else
                {
                    // 不检查有效性，直接尝试取消映射
                    mem::k_vmm.vmunmap(*_pcb->get_pagetable(), va, 1, 1);
                }
            }
        }

        /****************************************************************************************
         * 统一内存释放接口实现
         ****************************************************************************************/

        void ProcessMemoryManager::free_all_memory()
        {
            if (!_pcb)
                return;

            printfCyan("ProcessMemoryManager: starting complete memory cleanup for process %s (PID: %d)\n",
                       _pcb->get_name(), _pcb->get_pid());

            // 1. 处理VMA引用计数并释放（如果计数为0）
            if (_pcb->_vma != nullptr)
            {
                decrease_vma_refcount_and_free();
            }

            // 2. 释放trapframe
            if (_pcb->_trapframe)
            {
                mem::k_pmm.free_page(_pcb->_trapframe);
                _pcb->_trapframe = nullptr;
                printfGreen("ProcessMemoryManager: trapframe freed\n");
            }

            // 3. 如果页表存在，释放程序段和堆内存
            if (_pcb->get_pagetable()->get_base())
            {
                free_all_program_sections();
                free_heap_memory();
                free_pagetable();
            }

            // 4. 重置内存相关状态
            _pcb->reset_memory_sections();

            printfCyan("ProcessMemoryManager: complete memory cleanup finished\n");
        }

        void ProcessMemoryManager::emergency_cleanup()
        {
            if (!_pcb)
                return;

            printfRed("ProcessMemoryManager: emergency cleanup for process %s (PID: %d)\n",
                      _pcb->get_name(), _pcb->get_pid());

            // 紧急清理：不进行写回操作，只释放内存

            // 1. 强制释放VMA（不写回）
            if (_pcb->_vma != nullptr)
            {
                for (int i = 0; i < NVMA; ++i)
                {
                    if (_pcb->_vma->_vm[i].used)
                    {
                        // 只释放文件引用，不写回
                        if (_pcb->_vma->_vm[i].vfile != nullptr)
                        {
                            _pcb->_vma->_vm[i].vfile->free_file();
                        }

                        // 取消映射
                        uint64 va_start = PGROUNDDOWN(_pcb->_vma->_vm[i].addr);
                        uint64 va_end = PGROUNDUP(_pcb->_vma->_vm[i].addr + _pcb->_vma->_vm[i].len);
                        safe_vmunmap(va_start, va_end, false); // 不检查有效性

                        _pcb->_vma->_vm[i].used = 0;
                    }
                }
                delete _pcb->_vma;
                _pcb->_vma = nullptr;
            }

            // 2. 释放其他内存资源
            if (_pcb->_trapframe)
            {
                mem::k_pmm.free_page(_pcb->_trapframe);
                _pcb->_trapframe = nullptr;
            }

            if (_pcb->get_pagetable()->get_base())
            {
                free_all_program_sections();
                free_heap_memory();
                free_pagetable();
            }

            _pcb->reset_memory_sections();

            printfRed("ProcessMemoryManager: emergency cleanup completed\n");
        }

        void ProcessMemoryManager::cleanup_execve_pagetable(mem::PageTable & pagetable,
                                                            const program_section_desc *section_descs,
                                                            int section_count)
        {
            if (!pagetable.get_base())
            {
                printfYellow("cleanup_execve_pagetable: invalid pagetable, skipping cleanup\n");
                return;
            }

            printfRed("cleanup_execve_pagetable: cleaning up %d allocated sections\n", section_count);

            // 遍历所有已记录的程序段，释放其占用的内存
            for (int i = 0; i < section_count; i++)
            {
                if (section_descs[i]._sec_start && section_descs[i]._sec_size > 0)
                {
                    uint64 va_start = PGROUNDDOWN((uint64)section_descs[i]._sec_start);
                    uint64 va_end = PGROUNDUP((uint64)section_descs[i]._sec_start + section_descs[i]._sec_size);

                    printfRed("  Cleaning section %d (%s): %p - %p (%u bytes)\n",
                              i,
                              section_descs[i]._debug_name ? section_descs[i]._debug_name : "unnamed",
                              (void *)va_start,
                              (void *)va_end,
                              section_descs[i]._sec_size);

                    // 直接使用vmunmap清理，不检查页面有效性以提高错误处理的鲁棒性
                    for (uint64 va = va_start; va < va_end; va += PGSIZE)
                    {
                        mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                    }
                }
            }

            // 清理页表的特殊映射（trampoline、trapframe等）
#ifdef RISCV
            mem::k_vmm.vmunmap(pagetable, TRAMPOLINE, 1, 0);
#endif
            mem::k_vmm.vmunmap(pagetable, TRAPFRAME, 1, 0);
            mem::k_vmm.vmunmap(pagetable, SIG_TRAMPOLINE, 1, 0);

            // 减少页表引用计数
            pagetable.dec_ref();

            printfGreen("cleanup_execve_pagetable: cleanup completed\n");
        }

        /****************************************************************************************
         * 内存调试和监控接口实现
         ****************************************************************************************/

        void ProcessMemoryManager::print_memory_usage() const
        {
            if (!_pcb)
                return;
            // 调用PCB的详细内存信息打印函数
            _pcb->print_detailed_memory_info();
        }

        bool ProcessMemoryManager::verify_all_memory_consistency() const
        {
            if (!_pcb)
                return false;

            bool consistent = true;

            // 检查程序段一致性
            if (!verify_program_sections_consistency())
            {
                consistent = false;
            }

            // 检查PCB内部一致性
            if (!_pcb->verify_memory_consistency())
            {
                consistent = false;
            }

            return consistent;
        }

        uint64 ProcessMemoryManager::get_total_memory_usage() const
        {
            if (!_pcb)
                return 0;
            return _pcb->get_size();
        }

        uint64 ProcessMemoryManager::get_vma_memory_usage() const
        {
            if (!_pcb || !_pcb->_vma)
                return 0;

            uint64 total = 0;
            for (int i = 0; i < NVMA; i++)
            {
                if (_pcb->_vma->_vm[i].used)
                {
                    total += _pcb->_vma->_vm[i].len;
                }
            }
            return total;
        }

        bool ProcessMemoryManager::check_memory_leaks() const
        {
            if (!_pcb)
                return false;

            bool leaks_detected = false;

            // 检查是否有未释放的程序段
            if (_pcb->get_prog_section_count() > 0)
            {
                printfYellow("ProcessMemoryManager: %d program sections still present\n",
                             _pcb->get_prog_section_count());
                leaks_detected = true;
            }

            // 检查是否有未释放的堆内存
            if (_pcb->get_heap_size() > 0)
            {
                printfYellow("ProcessMemoryManager: heap memory still present (%u bytes)\n",
                             _pcb->get_heap_size());
                leaks_detected = true;
            }

            // 检查是否有未释放的VMA
            if (_pcb->_vma)
            {
                int active_vmas = 0;
                for (int i = 0; i < NVMA; i++)
                {
                    if (_pcb->_vma->_vm[i].used)
                    {
                        active_vmas++;
                    }
                }
                if (active_vmas > 0)
                {
                    printfYellow("ProcessMemoryManager: %d VMA entries still active\n", active_vmas);
                    leaks_detected = true;
                }
            }

            return leaks_detected;
        }

        /****************************************************************************************
         * 内部辅助函数实现
         ****************************************************************************************/

        bool ProcessMemoryManager::is_page_mapped(uint64 va) const
        {
            if (!_pcb || !_pcb->get_pagetable()->get_base())
            {
                return false;
            }

            mem::Pte pte = _pcb->get_pagetable()->walk(va, 0);
            return !pte.is_null() && pte.is_valid();
        }

        bool ProcessMemoryManager::safe_unmap_page(uint64 va)
        {
            if (!_pcb || !_pcb->get_pagetable()->get_base())
            {
                return false;
            }

            if (is_page_mapped(va))
            {
                mem::k_vmm.vmunmap(*_pcb->get_pagetable(), va, 1, 1);
                return true;
            }

            return false;
        }

        bool ProcessMemoryManager::writeback_file_mapping(const vma &vma_entry)
        {
            if (vma_entry.vfile == nullptr)
            {
                return true; // 匿名映射，无需写回
            }

            if (vma_entry.flags != MAP_SHARED || (vma_entry.prot & PROT_WRITE) == 0)
            {
                return true; // 非共享或不可写，无需写回
            }

            int result = vma_entry.vfile->write(vma_entry.addr, vma_entry.len);
            return result >= 0;
        }

        bool ProcessMemoryManager::is_vma_valid(int vma_index) const
        {
            if (!_pcb || !_pcb->_vma)
            {
                return false;
            }

            if (vma_index < 0 || vma_index >= NVMA)
            {
                return false;
            }

            return _pcb->_vma->_vm[vma_index].used;
        }

        uint64 ProcessMemoryManager::calculate_page_count(uint64 start_addr, uint64 size) const
        {
            uint64 start_aligned = PGROUNDDOWN(start_addr);
            uint64 end_aligned = PGROUNDUP(start_addr + size);
            return (end_aligned - start_aligned) / PGSIZE;
        }

        uint64 ProcessMemoryManager::align_to_page(uint64 addr, bool round_up) const
        {
            if (round_up)
            {
                return PGROUNDUP(addr);
            }
            else
            {
                return PGROUNDDOWN(addr);
            }
        }

    } // namespace proc
