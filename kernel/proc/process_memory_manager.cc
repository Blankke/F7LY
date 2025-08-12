/**
 * @file process_memory_manager.cc
 * @brief 进程内存管理器实现
 *
 * 实现进程内存管理器的所有功能，提供统一的内存管理接口。
 * 将原本散落在proc_manager.cc中的内存管理逻辑重构到这里。
 *
 * 统一管理说明：
 * - 所有内存释放统一通过 free_all_memory() 进行
 * - free_heap_memory() 内部调用 cleanup_heap_to_size(0)
 * - get_total_memory_usage() 返回缓存值，calculate_total_memory_size() 实时计算
 * - verify_all_memory_consistency() 包含 verify_memory_consistency 的核心逻辑
 * - get_total_program_memory() 保留为API兼容性，功能包含在 calculate_total_memory_size() 中
 */
#include "proc_manager.hh"
#include "process_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include "platform.hh" // 为MAX/MIN宏
#include "fs/vfs/file/normal_file.hh"
#include "shm/shm_manager.hh"

// 外部符号声明
extern char trampoline[];     // trampoline.S
extern char sig_trampoline[]; // sig_trampoline.S

namespace proc
{

    ProcessMemoryManager::ProcessMemoryManager()
        : prog_section_count(0), heap_start(0), heap_end(0), shared_vm(false),
          total_memory_size(0), ref_count(1)
    {
        // 初始化内存锁
        memory_lock.init("process_memory_lock");

        // 初始化程序段数组
        for (int i = 0; i < max_program_section_num; i++)
        {
            prog_sections[i]._sec_start = nullptr;
            prog_sections[i]._sec_size = 0;
            prog_sections[i]._debug_name = nullptr;
        }

        // 初始化VMA数据
        // 阶段1：移除VMA的分散引用计数，统一使用ProcessMemoryManager的引用计数
        for (int i = 0; i < NVMA; i++)
        {
            vma_data._vm[i].used = false;
        }
    }

    ProcessMemoryManager::~ProcessMemoryManager()
    {
        // 析构函数中不执行清理操作，避免双重释放
        // 清理应该通过显式调用free_all_memory()来完成
    }

    void ProcessMemoryManager::get()
    {
        ref_count.fetch_add(1, eastl::memory_order_relaxed);
    }

    bool ProcessMemoryManager::put()
    {
        int old_count = ref_count.fetch_sub(1, eastl::memory_order_acq_rel);
        if (old_count <= 1)
        {
            // 引用计数降至0或以下，需要清理
            return true;
        }
        return false;
    }

    int ProcessMemoryManager::get_ref_count() const
    {
        return ref_count.load(eastl::memory_order_acquire);
    }

    ProcessMemoryManager *ProcessMemoryManager::share_for_thread()
    {
        // 线程共享：增加引用计数并返回当前对象
        get();
        shared_vm = true; // 标记为共享虚拟内存
        return this;
    }

    ProcessMemoryManager *ProcessMemoryManager::clone_for_fork()
    {
        // 进程复制：创建新的内存管理器并深拷贝内容
        ProcessMemoryManager *new_mgr = new ProcessMemoryManager();

        // 为新进程创建页表
        if (!new_mgr->create_pagetable())
        {
            panic("[clone for fork] create_pagetable faol");
            delete new_mgr;
            return nullptr;
        }
        // printf("[clone_for_fork] start clone prog_section\n");

        // 复制程序段信息
        new_mgr->prog_section_count = prog_section_count;
        for (int i = 0; i < max_program_section_num; i++)
        {
            new_mgr->prog_sections[i] = prog_sections[i];
        }

        // 复制堆信息
        new_mgr->heap_start = heap_start;
        new_mgr->heap_end = heap_end;

        // 复制总内存大小
        new_mgr->total_memory_size = total_memory_size;

        // fork操作不共享虚拟内存，设置为false
        new_mgr->shared_vm = false;

        // 复制进程的所有内存段
        bool copy_success = true;


        // 复制程序段
        for (int i = 0; i < prog_section_count; i++)
        {
            uint64 start = (uint64)prog_sections[i]._sec_start;
            uint64 size = prog_sections[i]._sec_size;

            if (mem::k_vmm.vm_copy(pagetable, new_mgr->pagetable, start, size) < 0)
            {
                copy_success = false;
                break;
            }
        }

        // 复制堆
        if (copy_success && (heap_end > heap_start))
        {
            uint64 heap_size = heap_end - heap_start;
            if (mem::k_vmm.vm_copy(pagetable, new_mgr->pagetable, heap_start, heap_size) < 0)
            {
                copy_success = false;
            }
        }

        if (!copy_success)
        {
            panic("[clone_from_fork] copy failed");
            delete new_mgr;
            return nullptr;
        }

        // 复制VMA数据
        new_mgr->vma_data = vma_data;

        // 处理VMA中的文件映射引用计数
        for (int i = 0; i < NVMA; ++i)
        {
            if (vma_data._vm[i].used)
            {
                // 只对文件映射增加引用计数
                if (vma_data._vm[i].vfile != nullptr)
                {
                    vma_data._vm[i].vfile->dup(); // 增加引用计数
                }
            }
        }

        return new_mgr;
    }

    /****************************************************************************************
     * 程序段管理接口实现
     ****************************************************************************************/

    int ProcessMemoryManager::add_program_section(void *start, ulong size, const char *name)
    {
        if (prog_section_count >= max_program_section_num)
        {
            panic("add_program_section: too many program sections\n");
            return -1;
        }

        int index = prog_section_count++;
        prog_sections[index]._sec_start = start;
        prog_sections[index]._sec_size = size;
        prog_sections[index]._debug_name = name;

        // 更新总内存大小
        update_total_memory_size();

        // 验证内存一致性
        verify_memory_consistency();

        return index;
    }

    void ProcessMemoryManager::remove_program_section(int index)
    {
        if (index < 0 || index >= prog_section_count)
        {
            printfRed("remove_program_section: invalid index %d\n", index);
            return;
        }

        // 移动后续段到前面
        for (int i = index; i < prog_section_count - 1; i++)
        {
            prog_sections[i] = prog_sections[i + 1];
        }

        prog_section_count--;

        // 清理最后一个位置
        prog_sections[prog_section_count]._sec_start = nullptr;
        prog_sections[prog_section_count]._sec_size = 0;
        prog_sections[prog_section_count]._debug_name = nullptr;

        // 更新总内存大小
        update_total_memory_size();

        // 验证内存一致性
        verify_memory_consistency();
    }

    void ProcessMemoryManager::clear_all_program_sections_data()
    {
        for (int i = 0; i < prog_section_count; i++)
        {
            prog_sections[i]._sec_start = nullptr;
            prog_sections[i]._sec_size = 0;
            prog_sections[i]._debug_name = nullptr;
        }
        prog_section_count = 0;

        // 重新计算总内存大小：只包含堆空间
        update_total_memory_size();

        // 验证内存一致性
        verify_memory_consistency();
    }

    void ProcessMemoryManager::reset_memory_sections()
    {
        // 清空所有程序段
        clear_all_program_sections_data();

        // 重置堆信息
        heap_start = 0;
        heap_end = 0;

        // 重置总内存大小
        total_memory_size = 0;
    }

    uint64 ProcessMemoryManager::get_total_program_memory() const
    {
        // 为API兼容性保留，实现程序段总大小计算
        uint64 total = 0;
        for (int i = 0; i < prog_section_count; i++)
        {
            total += prog_sections[i]._sec_size;
        }
        return total;
    }

    void ProcessMemoryManager::copy_program_sections(const ProcessMemoryManager &src)
    {
        prog_section_count = src.prog_section_count;
        for (int i = 0; i < prog_section_count; i++)
        {
            prog_sections[i] = src.prog_sections[i];
        }

        // 更新总内存大小
        update_total_memory_size();
    }

    void ProcessMemoryManager::free_all_program_sections()
    {
        // 释放程序段占用的内存
        for (int i = 0; i < prog_section_count; i++)
        {
            if (prog_sections[i]._sec_size > 0)
            {
                uint64 va_start = PGROUNDDOWN((uint64)prog_sections[i]._sec_start);
                uint64 va_end = PGROUNDUP((uint64)prog_sections[i]._sec_start + prog_sections[i]._sec_size);
                // printfBlue("  Freeing section %d (%s): %p - %p (%u bytes)\n",
                //            i,
                //            prog_sections[i]._debug_name ? prog_sections[i]._debug_name : "unnamed",
                //            (void *)va_start,
                //            (void *)va_end,
                //            prog_sections[i]._sec_size);

                safe_vmunmap(va_start, va_end, true);
            }
            else
            {
                printfRed("prog_sections[i]._debug_name : %s  prog_sections[i]._sec_start : %p   prog_sections[i]._sec_size : %p\n", prog_sections[i]._debug_name, prog_sections[i]._sec_start, prog_sections[i]._sec_size);
                panic("free_all_program_section counter illegal section");
            }
        }

        // 阶段1：清理ProcessMemoryManager内的程序段描述信息
        for (int i = 0; i < max_program_section_num; i++)
        {
            prog_sections[i]._sec_start = nullptr;
            prog_sections[i]._sec_size = 0;
            prog_sections[i]._debug_name = nullptr;
        }
        prog_section_count = 0;

        // printfGreen("ProcessMemoryManager: program sections freed successfully\n");
    }

    bool ProcessMemoryManager::verify_program_sections_consistency() const
    {
        // 直接计算ProcessMemoryManager中程序段的总大小
        uint64 sections_total = 0;
        for (int i = 0; i < prog_section_count; i++)
        {
            sections_total += prog_sections[i]._sec_size;
        }

        // 与ProcessMemoryManager维护的总内存大小进行比较
        // 注意：total_memory_size包含程序段+堆，但不包含VMA
        uint64 heap_size = heap_end > heap_start ? heap_end - heap_start : 0;
        uint64 expected_sections_total = total_memory_size - heap_size;

        if (sections_total != expected_sections_total)
        {
            printfRed("ProcessMemoryManager: program sections inconsistency detected\n");
            printfRed("  Sections total: %u, Expected (sz - heap): %u\n",
                      (uint32)sections_total, (uint32)expected_sections_total);
            printfRed("  Total memory size: %u, Heap size: %u\n",
                      (uint32)total_memory_size, (uint32)heap_size);
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
        // 设置ProcessMemoryManager中的堆地址
        heap_start = start_addr;
        heap_end = start_addr;

        printfGreen("ProcessMemoryManager: heap initialized successfully\n");
    }

    uint64 ProcessMemoryManager::grow_heap(uint64 new_end)
    {
        // 直接使用ProcessMemoryManager中的堆地址
        uint64 current_end = heap_end;
        if (new_end <= current_end)
        {
            return current_end; // 无需扩展
        }

        // 使用虚拟内存管理器分配新的堆内存
        mem::PageTable &pt = pagetable;
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

        // 更新ProcessMemoryManager中的堆结束地址
        heap_end = new_end;

        printfGreen("ProcessMemoryManager: heap grown successfully to %p\n", (void *)new_end);
        return new_end;
    }

    uint64 ProcessMemoryManager::shrink_heap(uint64 new_end)
    {
        // 直接使用ProcessMemoryManager中的堆地址
        uint64 current_end = heap_end;
        uint64 current_start = heap_start;

        if (new_end >= current_end || new_end < current_start)
        {
            return current_end; // 无效的收缩请求
        }

        // 释放多余的堆内存
        uint64 va_start = PGROUNDUP(new_end);
        uint64 va_end = PGROUNDUP(current_end);

        for (uint64 va = va_start; va < va_end; va += PGSIZE)
        {
            if (is_page_mapped(va))
            {
                // 检查是否为共享内存地址
                void* shm_start_addr = nullptr;
                size_t shm_size = 0;
                if (shm::k_smm.find_shared_memory_segment((void *)va, &shm_start_addr, &shm_size)>=0)
                {
                    // 对于共享内存，使用detach_seg来正确处理引用计数
                    int result = shm::k_smm.detach_seg(shm_start_addr);
                    if (result != 0)
                    {
                        panic("[shrink_heap] Failed to detach shared memory at VA=%p\n", shm_start_addr);
                    }
                    
                    // 跳过整个共享内存段，直接移动到段结束位置
                    uint64 shm_end = (uint64)shm_start_addr + shm_size;
                    va = PGROUNDUP(shm_end) - PGSIZE; // -PGSIZE因为循环会+PGSIZE
                }
                else
                {
                    // 对于普通内存，直接使用vmunmap
                    mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                }
            }
        }

        // 更新ProcessMemoryManager中的堆结束地址
        heap_end = new_end;

        printfGreen("ProcessMemoryManager: heap shrunk successfully to %p\n", (void *)new_end);
        return new_end;
    }

    bool ProcessMemoryManager::cleanup_heap_to_size(uint64 new_size)
    {
        // 直接使用ProcessMemoryManager中的堆大小
        uint64 current_size = heap_end > heap_start ? heap_end - heap_start : 0;
        if (new_size >= current_size)
        {
            return true; // 无需收缩
        }

        uint64 new_end = heap_start + new_size;
        uint64 result = shrink_heap(new_end);

        return (result == new_end);
    }

    void ProcessMemoryManager::free_heap_memory()
    {
        // 重构：使用cleanup_heap_to_size(0)来完全释放堆内存
        cleanup_heap_to_size(0);
    }

    /****************************************************************************************
     * VMA管理接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::free_single_vma(int vma_index)
    {
        if (vma_index < 0 || vma_index >= NVMA || !vma_data._vm[vma_index].used)
        {
            return;
        }

        vma &vm_entry = vma_data._vm[vma_index];

        // printfBlue("  Processing VMA %d: addr=%p, len=%u, vfd=%d, flags=0x%x, prot=0x%x\n",
        //            vma_index, (void *)vm_entry.addr, vm_entry.len,
        //            vm_entry.vfd, vm_entry.flags, vm_entry.prot);

        // 写回文件映射（对于共享且可写的映射）
        if (vm_entry.vfile != nullptr &&
            vm_entry.flags == MAP_SHARED &&
            (vm_entry.prot & PROT_WRITE) != 0)
        {
            // 检查是否为/tmp下的临时文件或memfd，如果是则跳过写回
            bool is_tmp_file = false;
            if (vm_entry.vfile->_path_name.find("/tmp/") == 0 || 
                vm_entry.vfile->_path_name.find("memfd:") == 0)
            {
                is_tmp_file = true;
                printf("    Skipping writeback for temporary file: %s\n", 
                       vm_entry.vfile->_path_name.c_str());
            }
            
            if (!is_tmp_file)
            {
                // 简化的写回逻辑，避免调用单独的writeback_vma函数
                uint64 vma_start = PGROUNDDOWN(vm_entry.addr);
                uint64 vma_end = PGROUNDUP(vma_start + vm_entry.len);
                for (uint64 va = vma_start; va < vma_end; va += PGSIZE)
                {
                    mem::Pte pte = pagetable.walk(va, 0);
                    if (!pte.is_null() && pte.is_valid())
                    {
                        uint64 pa = (uint64)pte.pa();
                        int file_offset = vm_entry.offset + (va - vma_start);
                        printf("    Writing back page at va=%p to file offset %d\n", (void *)va, file_offset);
                        int write_result = vm_entry.vfile->write(pa, PGSIZE, file_offset, false);
                        if (write_result < 0)
                        {
                            printfRed("ProcessMemoryManager: VMA %d writeback failed\n", vma_index);
                        }
                    }
                }
            }
        }

        // 释放文件引用
        if (vm_entry.vfile != nullptr)
        {
            vm_entry.vfile->free_file();
            vm_entry.vfile = nullptr;
        }

        ///@brief 这里应该不用解除映射，因为mmap的位置也在堆上，后续free_heap的时候也要unmap
        ///此处应该只用处理文件映射相关的引用
        // // 取消虚拟地址映射
        // uint64 va_start = PGROUNDDOWN(vm_entry.addr);
        // uint64 va_end = PGROUNDUP(vm_entry.addr + vm_entry.len);
        // safe_vmunmap(va_start, va_end, true);

        // // 清理VMA条目
        memset(&vm_entry, 0, sizeof(vma));
    }

    void ProcessMemoryManager::free_all_vma()
    {
        // 遍历所有VMA条目，统一释放
        for (int i = 0; i < NVMA; ++i)
        {
            if (vma_data._vm[i].used)
            {
                free_single_vma(i);
            }
        }

        // printfGreen("ProcessMemoryManager: all VMA freed successfully\n");
    }

    int ProcessMemoryManager::unmap_memory_range(void *addr, size_t length)
    {
        if (!addr || length == 0)
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

        // printfYellow("ProcessMemoryManager: unmapping range [%p, %p) length=%u\n",
        //              addr, (void *)end_addr, aligned_length);

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
            vma &vm_entry = vma_data._vm[vma_idx];

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
                // 内联writeback_vma逻辑
                uint64 vma_start = PGROUNDDOWN(vm_entry.addr);
                uint64 vma_end = PGROUNDUP(vma_start + vm_entry.len);
                for (uint64 va = vma_start; va < vma_end; va += PGSIZE)
                {
                    mem::Pte pte = pagetable.walk(va, 0);
                    if (!pte.is_null() && pte.is_valid())
                    {
                        // 页面已分配，需要写回到文件
                        uint64 pa = (uint64)pte.pa();
                        int file_offset = vm_entry.offset + (va - vma_start);

                        // 写回数据到文件
                        int write_result = vm_entry.vfile->write(pa, PGSIZE, file_offset, false);
                        if (write_result < 0)
                        {
                            printfRed("[ProcessMemoryManager] Failed to write back page at va=%p\n", (void *)va);
                        }
                    }
                }
            }
            if (vm_entry.flags & MAP_SHARED) // 取消页表映射
            {
                shm::k_smm.detach_seg(addr);
            }
            else
            { // 取消页表映射
                safe_vmunmap(unmap_start, unmap_end, true);
            }
            // 处理VMA条目的更新
            if (unmap_start == vma_start && unmap_end == vma_end)
            {
                // 完全取消映射
                // printfCyan("ProcessMemoryManager: completely unmapping VMA %d\n", vma_idx);
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
        if (start_addr <= heap_end && end_addr > heap_start)
        {
            // 取消映射的区域与堆重叠，需要调整堆大小
            if (start_addr <= heap_start)
            {
                // 从堆开始位置或更早开始取消映射
                heap_end = heap_start;
                // printfYellow("ProcessMemoryManager: reset heap_end to heap_start\n");
            }
            else if (start_addr < heap_end)
            {
                // 从堆中间开始取消映射
                heap_end = start_addr;
                // printfYellow("ProcessMemoryManager: shrunk heap_end to %p\n", (void *)start_addr);
            }
        }

        return 0;
    }

    int ProcessMemoryManager::find_overlapping_vmas(uint64 start_addr, uint64 end_addr,
                                                    int overlapping_vmas[], int max_count)
    {
        if (!overlapping_vmas)
        {
            return 0;
        }

        int count = 0;
        for (int i = 0; i < NVMA && count < max_count; i++)
        {
            if (vma_data._vm[i].used)
            {
                uint64 vma_start = vma_data._vm[i].addr;
                uint64 vma_end = vma_start + vma_data._vm[i].len;

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

        vma &vm_entry = vma_data._vm[vma_index];
        uint64 vma_start = vm_entry.addr;
        uint64 vma_end = vm_entry.addr + vm_entry.len;

        if (unmap_start == vma_start && unmap_end < vma_end)
        {
            // 从VMA开始处取消映射
            // printfCyan("ProcessMemoryManager: unmapping from start of VMA %d\n", vma_index);
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
            // printfCyan("ProcessMemoryManager: unmapping from end of VMA %d\n", vma_index);
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

    bool ProcessMemoryManager::create_pagetable()
    {
        // 创建基础页表
        mem::PageTable pt = mem::k_vmm.vm_create();
        if (pt.is_null() || pt.get_base() == 0)
        {
            printfRed("ProcessMemoryManager: vm_create failed\n");
            return false;
        }

#ifdef RISCV
        // 映射trampoline页面
        if (mem::k_vmm.map_pages(pt, TRAMPOLINE, PGSIZE, (uint64)trampoline,
                                 riscv::PteEnum::pte_readable_m | riscv::pte_executable_m) == 0)
        {
            panic("ProcessMemoryManager: map trampoline failed\n");
            pt.freewalk();
            return false;
        }

        // 注意：trapframe映射延迟到usertrapret时进行

        // 映射信号trampoline页面
        if (mem::k_vmm.map_pages(pt, SIG_TRAMPOLINE, PGSIZE, (uint64)sig_trampoline,
                                 riscv::PteEnum::pte_readable_m | riscv::pte_executable_m | riscv::PteEnum::pte_user_m) == 0)
        {
            panic("ProcessMemoryManager: map sigtrapframe failed\n");
            // 先取消已成功的映射，再释放页表
            mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
            pt.freewalk();
            return false;
        }

#elif defined(LOONGARCH)
        // 注意：trapframe映射延迟到usertrapret时进行

        // 映射信号trampoline页面
        if (mem::k_vmm.map_pages(pt, SIG_TRAMPOLINE, PGSIZE, (uint64)sig_trampoline,
                                 PTE_P | PTE_MAT | PTE_D | PTE_U) == 0)
        {
            panic("ProcessMemoryManager: Fail to map sig_trampoline\n");
            pt.freewalk();
            return false;
        }
#endif

        // 设置页表
        pagetable = pt;
        return true;
    }

    void ProcessMemoryManager::free_pagetable()
    {
        if (!pagetable.get_base())
        {
            panic("ProcessMemoryManager: pagetable is null");
            return;
        }

        mem::PageTable &pt = pagetable;

        // 阶段1：不再依赖分散的引用计数，直接释放
        // 取消特殊页面的映射
#ifdef RISCV
        mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
#endif
        mem::k_vmm.vmunmap(pt, TRAPFRAME, 1, 0); // 有可能没有映射
        mem::k_vmm.vmunmap(pt, SIG_TRAMPOLINE, 1, 0);

        pt.freewalk();

        printfGreen("ProcessMemoryManager: pagetable freed successfully\n");
    }

    void ProcessMemoryManager::safe_vmunmap(uint64 va_start, uint64 va_end, bool check_validity)
    {
        if (!pagetable.get_base())
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
                mem::Pte pte = pagetable.walk(va, 0);
                if (!pte.is_null() && pte.is_valid())
                {
                    // 检查是否为共享内存地址
                    void* shm_start_addr = nullptr;
                    size_t shm_size = 0;
                    int shmid=shm::k_smm.find_shared_memory_segment((void *)va, &shm_start_addr, &shm_size);
                    printfBlue("[safe_vmunmap] Attempting to unmap VA=%p,tid:%d\n", (void *)va,shmid);
                    if (shmid>=0)
                    {
                        printfRed("[safe_vmunmap] Attempted to unmap shared memory at VA=%p (validity check)\n", shm_start_addr);
                        printfRed("[safe_vmunmap] Shared memory segment ID: %d\n", shmid);
                        
                        printfRed("[safe_vmunmap] current pid:%d, seg pid :%d\n", k_pm.get_cur_pcb()->_tid,shmid);
                        panic("shared memory should not appear here");
                    }
                    else
                    {
                        // 对于普通内存，直接使用vmunmap
                        mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                    }
                }
            }
            else
            {
                // 不检查有效性时也要区分共享内存和普通内存
                void* shm_start_addr = nullptr;
                size_t shm_size = 0;
                if (shm::k_smm.find_shared_memory_segment((void *)va, &shm_start_addr, &shm_size))
                {
                    // // 对于共享内存，使用detach_seg来正确处理引用计数
                    // int result = shm::k_smm.detach_seg(shm_start_addr);
                    // if (result != 0)
                    // {
                    //     printfRed("[safe_vmunmap] Failed to detach shared memory at VA=%p (no validity check)\n", shm_start_addr);
                    // }
                    
                    // // 跳过整个共享内存段，直接移动到段结束位置
                    // uint64 shm_end = (uint64)shm_start_addr + shm_size;
                    // va = PGROUNDUP(shm_end) - PGSIZE; // -PGSIZE因为循环会+PGSIZE
                    panic("shared memory should not appear here");
                }
                else
                {
                    // 对于普通内存，直接尝试取消映射
                    mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                }
            }
        }
    }

    /****************************************************************************************
     * 统一内存释放接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::free_all_memory()
    {
        // 减少引用计数，只有当引用计数降为0时才释放整个内存
        int old_count = ref_count.fetch_sub(1, eastl::memory_order_acq_rel);
        
        if (old_count <= 1)
        {
            // 引用计数降为0，释放所有内存资源
            // print_memory_usage();
            // 1. 释放VMA
            free_all_vma();
            // printfGreen("ProcessMemoryManager: all VMA freed\n");
            shared_vm = false;

            // 2. 如果页表存在，释放程序段和堆内存
            if (pagetable.get_base())
            {
                free_all_program_sections();
                // printfGreen("ProcessMemoryManager: all program sections freed\n");
                free_heap_memory();
                // printfGreen("ProcessMemoryManager: heap memory freed\n");
                free_pagetable();
                // printfGreen("ProcessMemoryManager: pagetable freed\n");
            }
            else
            {
                panic("pagetable is null");
            }

            // 3. 重置内存相关状态
            reset_memory_sections();
        }
        else{

            ///@todo 减少shm的segments里面的addr的vector里面的所有的包含此tid的都detach
            /// 同时记得在进程里面也增加一个duplicate_attachments的调用
        }
        // 如果引用计数还大于0，说明还有其他进程/线程在使用这块内存，不进行释放
    }

    void ProcessMemoryManager::emergency_cleanup()
    {
        printfRed("ProcessMemoryManager: emergency cleanup\n");

        // 紧急清理：不进行写回操作，只释放内存

        // 1. 强制释放VMA（不写回）
        for (int i = 0; i < NVMA; ++i)
        {
            if (vma_data._vm[i].used)
            {
                // 只释放文件引用，不写回
                if (vma_data._vm[i].vfile != nullptr)
                {
                    vma_data._vm[i].vfile->free_file();
                }

                // 取消映射
                uint64 va_start = PGROUNDDOWN(vma_data._vm[i].addr);
                uint64 va_end = PGROUNDUP(vma_data._vm[i].addr + vma_data._vm[i].len);
                safe_vmunmap(va_start, va_end, false); // 不检查有效性

                vma_data._vm[i].used = 0;
            }
        }
        shared_vm = false;

        // 2. 释放其他内存资源
        if (pagetable.get_base())
        {
            free_all_program_sections();
            free_heap_memory();
            free_pagetable();
        }

        reset_memory_sections();

        printfRed("ProcessMemoryManager: emergency cleanup completed\n");
    }

    void ProcessMemoryManager::cleanup_execve_pagetable(mem::PageTable &pagetable,
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

        // 清理页表的特殊映射（trampoline、sig_trampoline等）
#ifdef RISCV
        mem::k_vmm.vmunmap(pagetable, TRAMPOLINE, 1, 0);
#endif
        // 注意：trapframe映射由usertrapret管理，这里不需要显式取消映射
        mem::k_vmm.vmunmap(pagetable, SIG_TRAMPOLINE, 1, 0);

        // 阶段1：不再使用分散的引用计数
        // pagetable.dec_ref(); // 注释掉分散的引用计数操作

        printfGreen("cleanup_execve_pagetable: cleanup completed\n");
    }

    /****************************************************************************************
     * 内存调试和监控接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::update_total_memory_size()
    {
        total_memory_size = calculate_total_memory_size();
    }

    uint64 ProcessMemoryManager::calculate_total_memory_size() const
    {
        uint64 total = 0;

        // 计算所有程序段的大小（与get_total_program_memory()逻辑相同）
        for (int i = 0; i < prog_section_count; i++)
        {
            total += prog_sections[i]._sec_size;
        }

        // 加上堆的大小
        if (heap_end > heap_start)
        {
            total += (heap_end - heap_start);
        }

        return total;
    }

    bool ProcessMemoryManager::verify_memory_consistency()
    {
        uint64 calculated_total = calculate_total_memory_size();
        bool consistent = (total_memory_size == calculated_total);

        if (!consistent)
        {
            printfRed("Memory inconsistency detected\n");
            printfRed("  total_memory_size: %u, calculated: %u\n", (uint32)total_memory_size, (uint32)calculated_total);
            printfRed("  Note: VMA regions are managed separately and not counted in total_memory_size\n");
            panic("ProcessMemoryManager verify_memory_consistency failed\n");
        }

        return consistent;
    }

    void ProcessMemoryManager::print_memory_usage() const
    {
        printfCyan("=== ProcessMemoryManager Memory Information ===\n");
        printfCyan("Total process size: %u bytes\n", (uint32)total_memory_size);

        // 程序段信息
        printfCyan("Program sections (%d):\n", prog_section_count);
        uint64 sections_total = 0;
        for (int i = 0; i < prog_section_count; i++)
        {
            printfCyan("  Section %d (%s): %p - %p (%u bytes)\n",
                       i,
                       prog_sections[i]._debug_name ? prog_sections[i]._debug_name : "unnamed",
                       prog_sections[i]._sec_start,
                       (void *)((uint64)prog_sections[i]._sec_start + prog_sections[i]._sec_size),
                       (uint32)prog_sections[i]._sec_size);
            sections_total += prog_sections[i]._sec_size;
        }
        printfCyan("Total program sections: %u bytes\n", (uint32)sections_total);

        // 堆信息
        uint64 heap_size = (heap_end > heap_start) ? (heap_end - heap_start) : 0;
        if (heap_size > 0)
        {
            printfCyan("Heap: %p - %p (%u bytes)\n",
                       (void *)heap_start,
                       (void *)heap_end,
                       (uint32)heap_size);
        }
        else
        {
            printfCyan("Heap: not allocated\n");
        }

        // VMA信息
        printfCyan("VMA structure: present\n");
        uint64 vma_total = 0;
        int active_vmas = 0;
        for (int i = 0; i < NVMA; i++)
        {
            if (vma_data._vm[i].used)
            {
                printfCyan("  VMA %d: %p - %p (%u bytes, prot=%d, flags=%d)\n",
                           i,
                           (void *)vma_data._vm[i].addr,
                           (void *)(vma_data._vm[i].addr + vma_data._vm[i].len),
                           (uint32)vma_data._vm[i].len,
                           vma_data._vm[i].prot,
                           vma_data._vm[i].flags);
                vma_total += vma_data._vm[i].len;
                active_vmas++;
            }
        }
        printfCyan("Total VMA usage: %u bytes (%d active VMAs)\n", (uint32)vma_total, active_vmas);

        // 页表信息
        if (pagetable.get_base())
        {
            printfCyan("Page table: present (%p)\n", pagetable.get_base());
        }
        else
        {
            printfCyan("Page table: not present\n");
        }

        printfCyan("=== End ProcessMemoryManager Memory Information ===\n");
    }

    bool ProcessMemoryManager::verify_all_memory_consistency() const
    {
        bool consistent = true;

        // 检查程序段一致性
        if (!verify_program_sections_consistency())
        {
            consistent = false;
        }

        // 检查总内存大小一致性（类似于verify_memory_consistency的逻辑）
        uint64 calculated_total = calculate_total_memory_size();
        if (total_memory_size != calculated_total)
        {
            printfRed("Memory inconsistency detected in verify_all_memory_consistency\n");
            printfRed("  total_memory_size: %u, calculated: %u\n",
                      (uint32)total_memory_size, (uint32)calculated_total);
            consistent = false;
        }

        return consistent;
    }

    uint64 ProcessMemoryManager::get_total_memory_usage() const
    {
        // 直接返回缓存的总内存大小，等价于calculate_total_memory_size()的结果
        return total_memory_size;
    }

    uint64 ProcessMemoryManager::get_vma_memory_usage() const
    {
        uint64 total = 0;
        for (int i = 0; i < NVMA; i++)
        {
            if (vma_data._vm[i].used)
            {
                total += vma_data._vm[i].len;
            }
        }
        return total;
    }

    bool ProcessMemoryManager::check_memory_leaks() const
    {
        bool leaks_detected = false;

        // 检查是否有未释放的程序段
        if (prog_section_count > 0)
        {
            printfYellow("ProcessMemoryManager: %d program sections still present\n",
                         prog_section_count);
            leaks_detected = true;
        }

        // 检查是否有未释放的堆内存
        uint64 heap_size = (heap_end > heap_start) ? (heap_end - heap_start) : 0;
        if (heap_size > 0)
        {
            printfYellow("ProcessMemoryManager: heap memory still present (%u bytes)\n",
                         (uint32)heap_size);
            leaks_detected = true;
        }

        // 检查是否有未释放的VMA
        int active_vmas = 0;
        for (int i = 0; i < NVMA; i++)
        {
            if (vma_data._vm[i].used)
            {
                active_vmas++;
            }
        }
        if (active_vmas > 0)
        {
            printfYellow("ProcessMemoryManager: %d VMA entries still active\n", active_vmas);
            leaks_detected = true;
        }

        return leaks_detected;
    }

    /****************************************************************************************
     * 内部辅助函数实现
     ****************************************************************************************/

    bool ProcessMemoryManager::is_page_mapped(uint64 va)
    {
        if (!pagetable.get_base())
        {
            return false;
        }

        mem::Pte pte = pagetable.walk(va, 0);
        return !pte.is_null() && pte.is_valid();
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
        if (vma_index < 0 || vma_index >= NVMA)
        {
            return false;
        }

        return vma_data._vm[vma_index].used;
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
