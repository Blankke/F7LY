#include "shm_manager.hh"
#include "platform.hh"
#include "proc_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include <EASTL/sort.h>
#include "virtual_memory_manager.hh"
#include "memlayout.hh" // 为了获取PGSIZE等定义
namespace shm
{
    void ShmManager::init(uint64 base, uint64 size)
    {
        shm_base = base;
        shm_size = size;
        next_shmid = 1; // shmid从1开始

        // 初始化时整个内存区域都是空闲的
        free_blocks.clear();
        free_blocks.push_back({base, size});
        segments.clear(); // 清空共享内存段容器

        printfGreen("[ShmManager] Initialized with base=0x%lx, size=0x%lx\n", base, size);
    }

    uint64 ShmManager::allocate_memory(size_t size)
    {
        // 按页对齐
        size_t aligned_size = PGROUNDUP(size);

        // 遍历空闲块，找到第一个足够大的块（First Fit策略）
        for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it)
        {
            if (it->size >= aligned_size)
            {
                uint64 allocated_addr = it->addr;

                // 如果空闲块正好等于需要的大小，直接移除
                if (it->size == aligned_size)
                {
                    free_blocks.erase(it);
                }
                else
                {
                    // 否则缩小空闲块
                    it->addr += aligned_size;
                    it->size -= aligned_size;
                }

                return allocated_addr;
            }
        }

        // 没有找到足够大的空闲块
        return 0;
    }

    void ShmManager::deallocate_memory(uint64 addr, size_t size)
    {
        size_t aligned_size = PGROUNDUP(size);

        // 添加新的空闲块
        free_blocks.push_back({addr, aligned_size});

        // 按地址排序
        eastl::sort(free_blocks.begin(), free_blocks.end());

        // 合并相邻的空闲块
        merge_adjacent_blocks();
    }

    void ShmManager::merge_adjacent_blocks()
    {
        if (free_blocks.size() <= 1)
            return;

        auto write_it = free_blocks.begin();
        auto read_it = free_blocks.begin();

        while (read_it != free_blocks.end())
        {
            *write_it = *read_it;
            ++read_it;

            // 尝试与后续相邻块合并
            while (read_it != free_blocks.end() &&
                   write_it->addr + write_it->size == read_it->addr)
            {
                write_it->size += read_it->size;
                ++read_it;
            }

            ++write_it;
        }

        // 调整vector大小
        free_blocks.resize(write_it - free_blocks.begin());
    }

    int ShmManager::create_seg(key_t key, size_t size, int shmflg)
    {
        // 使用新的内存分配方法
        uint64 allocated_addr = allocate_memory(size);
        if (allocated_addr == 0)
        {
            printfRed("[ShmManager] Failed to allocate memory for size=0x%lx\n", size);
            return ; // 内存不足
        }

        // 创建新的共享内存段
        shm_segment new_seg;
        new_seg.shmid = next_shmid++;
        new_seg.key = key;
        new_seg.size = PGROUNDUP(size);
        new_seg.shmflg = shmflg;
        new_seg.phy_addrs = allocated_addr;                   // 设置分配得到的物理地址
        new_seg.addr = nullptr;                               // 初始时未映射到进程地址空间
        
        // 初始化时间信息
        new_seg.atime = 0;                                    // 初始化访问时间
        new_seg.dtime = 0;                                    // 初始化分离时间
        new_seg.ctime = rdtime();                             // 设置创建时间为当前时间
        
        // 初始化进程信息
        proc::Pcb* current_proc = proc::k_pm.get_cur_pcb();
        new_seg.creator_pid = current_proc->_pid;             // 设置创建者进程ID
        new_seg.last_pid = current_proc->_pid;                // 最后操作进程ID
        
        // 初始化权限信息 (从当前进程获取)
        new_seg.owner_uid = current_proc->_uid;               // 所有者用户ID
        new_seg.owner_gid = current_proc->_gid;               // 所有者组ID
        new_seg.creator_uid = current_proc->_uid;             // 创建者用户ID
        new_seg.creator_gid = current_proc->_gid;             // 创建者组ID
        new_seg.mode = shmflg & 0777;                         // 从shmflg提取权限位
        
        // 初始化状态信息
        new_seg.nattch = 0;                                   // 初始附加数为0
        new_seg.seq = 0;                                      // 初始序列号

        segments.insert({new_seg.shmid, new_seg}); // 插入到容器中

        printfGreen("[ShmManager] Created segment shmid=%d, size=0x%lx at addr=0x%lx\n",
                    new_seg.shmid, new_seg.size, allocated_addr);

        return new_seg.shmid; // 返回新创建的共享内存段ID
    }
    int ShmManager::delete_seg(int shmid)
    {
        auto it = segments.find(shmid);
        if (it == segments.end())
        {
            printfRed("[ShmManager] Segment with shmid=%d not found\n", shmid);
            return -1; // 未找到共享内存段
        }

        shm_segment &seg = it->second;

        // 回收内存到空闲块
        deallocate_memory(seg.phy_addrs, seg.size);

        // printfYellow("[ShmManager] Deleted segment shmid=%d, freed addr=0x%x, size=0x%x\n",
        //             shmid, seg.phy_addrs, seg.size);

        segments.erase(it); // 从容器中删除共享内存段
        return 0;
    }
    void *ShmManager::attach_seg(int shmid, void *shmaddr, int shmflg)
    {
        // 查找共享内存段
        auto it = segments.find(shmid);
        if (it == segments.end())
        {
            printfRed("[ShmManager] Segment with shmid=%d not found\n", shmid);
            return (void *)-1; // 段不存在
        }

        shm_segment &seg = it->second;

        // 确定映射地址
        uint64 attach_addr = 0;

        if (shmaddr == nullptr)
        {
            // 情况1：shmaddr为NULL，内核自动选择一个地址
            // 这里我们可以在进程的堆区域后面分配
            proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
            attach_addr = PGROUNDUP(current_proc->_sz); // 在进程空间末尾分配

            printfCyan("[ShmManager] Kernel auto-selected address: 0x%lx\n", attach_addr);
        }
        else
        {
            // 情况2和3：shmaddr不为NULL
            uint64 requested_addr = (uint64)shmaddr;

            if (shmflg & SHM_RND)
            {
                // 情况3：设置了SHM_RND标记，向下调整到SHMLBA的整数倍
                attach_addr = requested_addr - (requested_addr % SHMLBA);
                printfCyan("[ShmManager] Address rounded down from 0x%lx to 0x%lx (SHMLBA=%d)\n",
                           requested_addr, attach_addr, SHMLBA);
            }
            else
            {
                // 情况2：使用指定的地址
                attach_addr = requested_addr;
                printfCyan("[ShmManager] Using specified address: 0x%lx\n", attach_addr);
            }

            // 检查地址是否页对齐（如果没有SHM_RND标志）
            if (!(shmflg & SHM_RND) && (attach_addr % PGSIZE != 0))
            {
                printfRed("[ShmManager] Address 0x%lx is not page-aligned\n", attach_addr);
                return (void *)-1;
            }
        }

        // 设置页表权限标志
        int flags = 0;
#ifdef RISCV
        flags |= PTE_U; // 用户可访问
#elif defined(LOONGARCH)
        flags |= PTE_MAT | PTE_PLV | PTE_D | PTE_P; // 用户可访问
#endif

        // 根据shmflg设置读写权限
        if (shmflg & SHM_RDONLY)
        {
            flags |= PTE_R; // 只读权限
            printfCyan("[ShmManager] Attaching with READ-ONLY permissions\n");
        }
        else
        {
            flags |= PTE_R | PTE_W; // 读写权限
            printfCyan("[ShmManager] Attaching with READ-WRITE permissions\n");
        }

        // 建立物理内存和虚拟内存的映射
        bool map_result = mem::k_vmm.map_pages(
            proc::k_pm.get_cur_pcb()->_pt, // 当前进程页表
            attach_addr,                   // 虚拟地址
            seg.size,                      // 映射大小
            seg.phy_addrs,                 // 物理地址
            flags                          // 权限标志
        );

        if (!map_result)
        {
            printfRed("[ShmManager] Failed to map pages for shmid=%d\n", shmid);
            return (void *)-1;
        }

        // 更新段信息
        seg.addr = (void *)attach_addr; // 记录映射的虚拟地址
        seg.atime = rdtime();           // 更新访问时间
        seg.last_pid = proc::k_pm.get_cur_pcb()->_pid; // 更新最后操作进程ID
        seg.nattch++;                   // 增加附加计数

        // 更新进程大小（如果映射地址超出当前进程大小）
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        uint64 end_addr = attach_addr + seg.size;
        if (end_addr > current_proc->_sz)
        {
            current_proc->_sz = end_addr;
        }

        printfGreen("[ShmManager] Successfully attached segment shmid=%d at address 0x%x, size=0x%x\n",
                    shmid, attach_addr, seg.size);

        return (void *)attach_addr; // 返回映射的虚拟地址
    }

    int ShmManager::detach_seg(void *addr)
    {
        auto it = segments.begin();
        // 查找包含该地址的共享内存段
        for (; it != segments.end(); ++it)
        {
            shm_segment &seg = it->second;
            if (seg.addr == addr)
                break;
        }
        if (it == segments.end())
        {
            printfRed("[ShmManager] Segment with address %p not found\n", addr);
            return -1; // 未找到共享内存段
        }
        shm_segment &seg = it->second;
        // 解除映射
        mem::k_vmm.vmunmap(
            proc::k_pm.get_cur_pcb()->_pt, // 当前进程页表
            (uint64)addr,                  // 虚拟地址
            PGROUNDUP(seg.size) / PGSIZE,  // 页数
            0                              // 不释放物理页
        );
        // 更新分离时间
        seg.dtime = rdtime();
        seg.last_pid = proc::k_pm.get_cur_pcb()->_pid; // 更新最后操作进程ID
        seg.nattch--;                   // 减少附加计数
        // 清除映射地址
        seg.addr = nullptr;
        //注意，此处detach_seg只是解除映射，并不删除共享内存段
        // 如果需要删除共享内存段，请调用delete_seg方法
        printfCyan("[ShmManager] Detached segment at addr=%p\n", addr);

        return 0;
    }

    int ShmManager::shmctl(int shmid, int cmd, struct shmid_ds *buf)
    {
        // 查找共享内存段
        auto it = segments.find(shmid);
        if (it == segments.end()) {
            printfRed("[ShmManager] Segment with shmid=%d not found for shmctl\n", shmid);
            return -1;
        }

        shm_segment& seg = it->second;
        proc::Pcb* current_proc = proc::k_pm.get_cur_pcb();

        switch (cmd) {
            case IPC_STAT:
            {
                // 获取共享内存段信息到用户提供的缓冲区
                if (buf == nullptr) {
                    printfRed("[ShmManager] buf is null for IPC_STAT\n");
                    return -1;
                }

                // 创建内核空间的 shmid_ds 结构体
                struct shmid_ds kernel_buf = {};
                
                // 填充 shmid_ds 结构体
                kernel_buf.shm_perm.__key = seg.key;
                kernel_buf.shm_perm.uid = seg.owner_uid;
                kernel_buf.shm_perm.gid = seg.owner_gid;
                kernel_buf.shm_perm.cuid = seg.creator_uid;
                kernel_buf.shm_perm.cgid = seg.creator_gid;
                kernel_buf.shm_perm.mode = seg.mode;
                kernel_buf.shm_perm.__seq = seg.seq;

                kernel_buf.shm_segsz = seg.size;
                kernel_buf.shm_atime = seg.atime;
                kernel_buf.shm_dtime = seg.dtime;
                kernel_buf.shm_ctime = seg.ctime;
                kernel_buf.shm_cpid = seg.creator_pid;
                kernel_buf.shm_lpid = seg.last_pid;
                kernel_buf.shm_nattch = seg.nattch;

                // 复制到用户空间
                if (mem::k_vmm.copy_out(current_proc->_pt, (uint64)buf, &kernel_buf, sizeof(kernel_buf)) < 0) {
                    printfRed("[ShmManager] Failed to copy shmid_ds to user space\n");
                    return -1;
                }

                printfCyan("[ShmManager] IPC_STAT: shmid=%d, size=0x%lx, nattch=%d\n", 
                          shmid, seg.size, seg.nattch);
                break;
            }

            case IPC_SET:
            {
                // 设置共享内存段信息
                if (buf == nullptr) {
                    printfRed("[ShmManager] buf is null for IPC_SET\n");
                    return -1;
                }

                // 检查权限：只有所有者或创建者可以修改
                if (current_proc->_uid != seg.owner_uid && 
                    current_proc->_uid != seg.creator_uid &&
                    current_proc->_uid != 0) {  // root用户
                    printfRed("[ShmManager] Permission denied for IPC_SET\n");
                    return -1;
                }

                // 从用户空间复制数据
                struct shmid_ds user_buf;
                if (mem::k_vmm.copy_in(current_proc->_pt, &user_buf, (uint64)buf, sizeof(user_buf)) < 0) {
                    printfRed("[ShmManager] Failed to copy shmid_ds from user space\n");
                    return -1;
                }

                // 更新可修改的字段
                seg.owner_uid = user_buf.shm_perm.uid;
                seg.owner_gid = user_buf.shm_perm.gid;
                seg.mode = user_buf.shm_perm.mode & 0777;  // 只保留权限位
                seg.ctime = rdtime();  // 更新修改时间
                seg.last_pid = current_proc->_pid;

                printfCyan("[ShmManager] IPC_SET: shmid=%d, new mode=0%o, new uid=%d\n", 
                          shmid, seg.mode, seg.owner_uid);
                break;
            }

            case IPC_RMID:
            {
                // 标记共享内存段待删除
                // 检查权限：只有所有者或创建者可以删除
                if (current_proc->_uid != seg.owner_uid && 
                    current_proc->_uid != seg.creator_uid &&
                    current_proc->_uid != 0) {  // root用户
                    printfRed("[ShmManager] Permission denied for IPC_RMID\n");
                    return -1;
                }

                // 如果还有进程附加到这个段，暂时不删除，只标记
                if (seg.nattch > 0) {
                    printfYellow("[ShmManager] IPC_RMID: shmid=%d marked for deletion, %d attachments remain\n", 
                                shmid, seg.nattch);
                    // 在实际系统中应该设置一个删除标记，当nattch变为0时自动删除
                    // 这里简化处理，直接删除
                }

                // 立即删除共享内存段
                int result = delete_seg(shmid);
                if (result == 0) {
                    printfGreen("[ShmManager] IPC_RMID: shmid=%d successfully removed\n", shmid);
                } else {
                    printfRed("[ShmManager] IPC_RMID: failed to remove shmid=%d\n", shmid);
                }
                return result;
            }

            default:
                printfRed("[ShmManager] Unknown shmctl command: %d\n", cmd);
                return -1;
        }

        return 0;
    }

    shm_segment ShmManager::get_seg_info(int shmid)
    {
        auto it = segments.find(shmid);
        if (it != segments.end())
        {
            return it->second;
        }

        // 返回一个无效的段
        shm_segment invalid_seg = {};
        invalid_seg.shmid = -1;
        return invalid_seg;
    }

    int ShmManager::set_seg_info(int shmid, const shm_segment &seg_info)
    {
        auto it = segments.find(shmid);
        if (it == segments.end())
        {
            return -1; // 段不存在
        }

        it->second = seg_info;
        return 0;
    }

    key_t ShmManager::ftok(const char *__pathname, int __proj_id)
    {
        // 简单的ftok实现，实际应该基于文件系统信息
        // 这里使用字符串哈希 + proj_id
        uint64 hash = 0;
        while (*__pathname)
        {
            hash = hash * 31 + *__pathname++;
        }
        return (key_t)((hash & 0xFFFFFF) | ((__proj_id & 0xFF) << 24));
    }

    void ShmManager::print_memory_status() const
    {
        printfYellow("[ShmManager] Memory Status:\n");
        printfYellow("  Total memory: 0x%lx bytes\n", shm_size);
        printfYellow("  Active segments: %zu\n", segments.size());
        printfYellow("  Free blocks: %zu\n", free_blocks.size());

        size_t total_free = 0;
        for (const auto &block : free_blocks)
        {
            printfYellow("    Free block: addr=0x%lx, size=0x%lx\n", block.addr, block.size);
            total_free += block.size;
        }
        printfYellow("  Total free memory: 0x%lx bytes\n", total_free);
        printfYellow("  Memory utilization: %.1f%%\n",
                     (double)(shm_size - total_free) * 100.0 / shm_size);
    }

    size_t ShmManager::get_total_free_memory() const
    {
        size_t total_free = 0;
        for (const auto &block : free_blocks)
        {
            total_free += block.size;
        }
        return total_free;
    }

    size_t ShmManager::get_largest_free_block() const
    {
        size_t largest = 0;
        for (const auto &block : free_blocks)
        {
            if (block.size > largest)
            {
                largest = block.size;
            }
        }
        return largest;
    }
}