#include "shm_manager.hh"
#include "platform.hh"
#include "proc_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include <EASTL/sort.h>
#include <EASTL/algorithm.h>
#include "virtual_memory_manager.hh"
#include "memlayout.hh" // 为了获取PGSIZE等定义
#include "fs/lwext4/ext4_errno.hh"  // 为了获取错误码定义
#include "tm/timer_manager.hh"
namespace shm
{
    ShmManager k_smm; // 全局共享内存管理器实例
    void ShmManager::init(uint64 base, uint64 size)
    {
        shm_base = base;
        shm_size = size;
        next_shmid = 1; // shmid从1开始

        // 初始化时整个内存区域都是空闲的
        free_blocks = new eastl::vector<free_block>();
        free_blocks->clear();
        free_blocks->push_back({base, size});
        segments =new eastl::unordered_map<int, shm_segment>();
        // 显式初始化 segments 容器

        // 注意：不进行预分配，避免在内核环境中的内存分配问题
         
        printfGreen("[ShmManager] Initialized with base=0x%x, size=0x%x\n", base, size);
    }

    uint64 ShmManager::allocate_memory(size_t size)
    {
        // 按页对齐
        size_t aligned_size = PGROUNDUP(size);

        // 遍历空闲块，找到第一个足够大的块（First Fit策略）
        for (auto it = free_blocks->begin(); it != free_blocks->end(); ++it)
        {
            if (it->size >= aligned_size)
            {
                uint64 allocated_addr = it->addr;

                // 如果空闲块正好等于需要的大小，直接移除
                if (it->size == aligned_size)
                {
                    free_blocks->erase(it);
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
        free_blocks->push_back({addr, aligned_size});

        // 按地址排序
        eastl::sort(free_blocks->begin(), free_blocks->end());

        // 合并相邻的空闲块
        merge_adjacent_blocks();
    }

    void ShmManager::merge_adjacent_blocks()
    {
        if (free_blocks->size() <= 1)
            return;

        auto write_it = free_blocks->begin();
        auto read_it = free_blocks->begin();

        while (read_it != free_blocks->end())
        {
            *write_it = *read_it;
            ++read_it;

            // 尝试与后续相邻块合并
            while (read_it != free_blocks->end() &&
                   write_it->addr + write_it->size == read_it->addr)
            {
                write_it->size += read_it->size;
                ++read_it;
            }

            ++write_it;
        }

        // 调整vector大小
        free_blocks->resize(write_it - free_blocks->begin());
    }

    eastl::unordered_map<int, shm_segment>::iterator ShmManager::find_segment_by_key(key_t key)
    {

        for (auto it = segments->begin(); it != segments->end(); ++it) {
            if (it->second.key == key) {
                return it;
            }
        }
        return segments->end();
    }

    bool ShmManager::check_segment_read_permission(const shm_segment& seg, uid_t uid, gid_t gid)
    {
        // 简化的读权限检查：root用户总是有权限
        if (uid == 0) {
            return true;
        }
        
        // 所有者权限检查
        if (uid == seg.owner_uid) {
            return (seg.mode & 0400) != 0;  // 检查所有者读权限位
        }
        
        // 组权限检查
        if (gid == seg.owner_gid) {
            return (seg.mode & 0040) != 0;  // 检查组读权限位
        }
        
        // 其他用户权限检查
        return (seg.mode & 0004) != 0;  // 检查其他用户读权限位
    }

    bool ShmManager::check_segment_attach_permission(const shm_segment& seg, uid_t uid, gid_t gid, bool need_write)
    {
        // 简化的附加权限检查：root用户总是有权限
        if (uid == 0) {
            return true;
        }
        
        // 所有者权限检查
        if (uid == seg.owner_uid) {
            if (need_write) {
                return (seg.mode & 0600) == 0600;  // 需要读写权限
            } else {
                return (seg.mode & 0400) != 0;     // 只需要读权限
            }
        }
        
        // 组权限检查
        if (gid == seg.owner_gid) {
            if (need_write) {
                return (seg.mode & 0060) == 0060;  // 需要读写权限
            } else {
                return (seg.mode & 0040) != 0;     // 只需要读权限
            }
        }
        
        // 其他用户权限检查
        if (need_write) {
            return (seg.mode & 0006) == 0006;  // 需要读写权限
        } else {
            return (seg.mode & 0004) != 0;     // 只需要读权限
        }
    }

    uint64 ShmManager::find_available_address(proc::Pcb* proc, size_t size)
    {
        // 从堆结束位置之后开始查找，避免与程序段和堆冲突
        uint64 start_addr = PGROUNDUP(proc->get_heap_end());
        
        // 确保地址对齐到SHMLBA
        start_addr = PGROUNDUP(start_addr);
        if (start_addr % SHMLBA != 0) {
            start_addr = ((start_addr / SHMLBA) + 1) * SHMLBA;
        }
        proc->set_heap_end(start_addr + size); // 更新堆结束位置
        // // 检查地址范围合法性（简化：假设用户空间上限为0x40000000）
        // const uint64 USER_SPACE_LIMIT = 0x40000000ULL;
        // if (start_addr + size > USER_SPACE_LIMIT) {
        //     printfRed("[ShmManager] Address 0x%x + size 0x%x exceeds user space limit\n", 
        //              start_addr, size);
        //     return 0;
        // }
        
        // TODO: 应该检查与现有VMA的冲突，这里简化处理
        return start_addr;
    }

    bool ShmManager::is_valid_attach_address(uint64 addr, size_t size, bool rounded)
    {
        // 检查地址是否为空指针
        if (addr == 0) {
            return false;
        }
        
        // 检查地址是否页对齐（如果没有进行舍入）
        if (!rounded && (addr % PGSIZE != 0)) {
            printfRed("[ShmManager] Address 0x%x is not page-aligned\n", addr);
            return false;
        }
        
        // 检查地址是否SHMLBA对齐（舍入后必须对齐）
        if (rounded && (addr % SHMLBA != 0)) {
            printfRed("[ShmManager] Rounded address 0x%x is not SHMLBA-aligned\n", addr);
            return false;
        }
        
        // 检查地址范围是否在用户空间内
        // const uint64 USER_SPACE_START = 0x1000ULL;     // 用户空间起始地址
        // const uint64 USER_SPACE_LIMIT = 0x40000000ULL; // 用户空间限制
        
        // if (addr < USER_SPACE_START) {
        //     printfRed("[ShmManager] Address 0x%x is below user space start\n", addr);
        //     return false;
        // }
        
        // if (addr + size > USER_SPACE_LIMIT) {
        //     printfRed("[ShmManager] Address range [0x%x, 0x%x] exceeds user space limit\n", 
        //              addr, addr + size);
        //     return false;
        // }
        
        // 检查地址范围是否会溢出
        if (addr + size < addr) {
            printfRed("[ShmManager] Address range wraps around\n");
            return false;
        }
        
        return true;
    }

    bool ShmManager::has_address_conflict(proc::Pcb* proc, uint64 addr, size_t size)
    {
        uint64 end_addr = addr + size;
        
        // 检查是否与程序段冲突
        for (int i = 0; i < proc->get_prog_section_count(); i++) {
            const auto* sections = proc->get_prog_sections();
            uint64 section_start = (uint64)sections[i]._sec_start;
            uint64 section_end = section_start + sections[i]._sec_size;
            
            if (addr < section_end && end_addr > section_start) {
                printfRed("[ShmManager] Address range [0x%x, 0x%x] conflicts with program section %d [0x%x, 0x%x]\n",
                         addr, end_addr, i, section_start, section_end);
                return true;
            }
        }
        
        // 检查是否与堆冲突
        // uint64 heap_start = proc->get_heap_start();
        // uint64 heap_end = proc->get_heap_end();
        // if (heap_start < heap_end && addr < heap_end && end_addr > heap_start) {
        //     printfRed("[ShmManager] Address range [0x%x, 0x%x] conflicts with heap [0x%x, 0x%x]\n",
        //              addr, end_addr, heap_start, heap_end);
        //     return true;
        // }
        
        // 检查是否与现有VMA冲突
        if (proc->get_vma() != nullptr) {
            for (int i = 0; i < proc::NVMA; i++) {
                if (proc->get_vma()->_vm[i].used) {
                    uint64 vma_start = proc->get_vma()->_vm[i].addr;
                    uint64 vma_end = vma_start + proc->get_vma()->_vm[i].len;
                    
                    if (addr < vma_end && end_addr > vma_start) {
                        printfRed("[ShmManager] Address range [0x%x, 0x%x] conflicts with VMA %d [0x%x, 0x%x]\n",
                                 addr, end_addr, i, vma_start, vma_end);
                        return true;
                    }
                }
            }
        }
        
        // // 检查是否与其他共享内存段冲突（仅与当前线程的映射比较，避免跨进程/线程误判）
        uint cur_tid = proc->get_tid();
        for (const auto& pair : *segments) {
            const shm_segment& seg = pair.second;
            for (const auto& ent : seg.attached_addrs) {
                if (ent.tid != cur_tid) continue;
                uint64 shm_start = (uint64)ent.addr;
                uint64 shm_end = shm_start + seg.real_size;
                
                if (addr < shm_end && end_addr > shm_start) {
                    printfRed("[ShmManager] Address range [0x%x, 0x%x] conflicts with existing shared memory [0x%x, 0x%x] (tid=%d)\n",
                             addr, end_addr, shm_start, shm_end, cur_tid);
                    return true;
                }
            }
        }
        
        return false;  // 没有冲突
    }

    bool ShmManager::check_segment_permission(const shm_segment& seg, uid_t uid, gid_t gid, mode_t requested_mode)
    {
        // 简化的权限检查：root用户总是有权限
        if (uid == 0) {
            return true;
        }
        
        // 所有者权限检查
        if (uid == seg.owner_uid) {
            return (seg.mode & 0700) != 0;  // 检查所有者权限位
        }
        
        // 组权限检查
        if (gid == seg.owner_gid) {
            return (seg.mode & 0070) != 0;  // 检查组权限位
        }
        
        // 其他用户权限检查
        return (seg.mode & 0007) != 0;  // 检查其他用户权限位
    }

    int ShmManager::create_seg(key_t key, size_t size, int shmflg)
    {
        // 处理 IPC_PRIVATE 情况 - 总是创建新段
        if (key == IPC_PRIVATE) {
            return create_new_segment(key, size, shmflg);
        }

        // 查找是否已存在相同key的段 - 先检查容器是否为空
        if (segments->empty()) {
            // 容器为空，直接跳到创建新段的逻辑
            if (!(shmflg & IPC_CREAT)) {
                printfRed("[ShmManager] No segment exists for key=0x%x and IPC_CREAT not specified\n", key);
                return -ENOENT;  // 段不存在且未指定 IPC_CREAT
            }
            // 创建新段
            return create_new_segment(key, size, shmflg);
        }
        
        // 容器不为空，安全地查找
        auto existing_seg = find_segment_by_key(key);

        if (existing_seg != segments->end()) {
            // 段已存在的情况
            shm_segment& seg = existing_seg->second;
            
            // 检查 IPC_CREAT | IPC_EXCL 组合
            if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) {
                printfRed("[ShmManager] Segment with key=0x%x already exists (IPC_EXCL specified)\n", key);
                return -EEXIST;  // 段已存在且指定了 IPC_EXCL
            }
            
            // 验证大小是否匹配
            if (size > seg.size) {
                printfRed("[ShmManager] Requested size 0x%x exceeds existing segment size 0x%x\n", 
                         size, seg.size);
                return -EINVAL;  // 请求的大小超过现有段大小
            }
            
            // TODO: 检查访问权限
            proc::Pcb* current_proc = proc::k_pm.get_cur_pcb();
            if (!check_segment_permission(seg, current_proc->_uid, current_proc->_gid, shmflg & 0777)) {
                printfRed("[ShmManager] Permission denied for existing segment key=0x%x\n", key);
                return -EACCES;  // 权限不足
            }
            
            printfCyan("[ShmManager] Found existing segment shmid=%d for key=0x%x\n", seg.shmid, key);
            return seg.shmid;  // 返回现有段的ID
        } 
        else {
            // 段不存在的情况
            if (!(shmflg & IPC_CREAT)) {
                printfRed("[ShmManager] No segment exists for key=0x%x and IPC_CREAT not specified\n", key);
                return -ENOENT;  // 段不存在且未指定 IPC_CREAT
            }
            
            // 创建新段
            return create_new_segment(key, size, shmflg);
        }
    }
    
    int ShmManager::create_new_segment(key_t key, size_t size, int shmflg)
    {
        // 验证大小限制
        // const size_t SHMMIN = PGSIZE;        // 最小段大小为一页
        const size_t SHMMAX = 32 * 1024 * 1024;  // 最大段大小为32MB (可配置)
        
        // if (size < SHMMIN) {
        //     printfRed("[ShmManager] Size 0x%x is less than SHMMIN (0x%x)\n", size, SHMMIN);
        //     size = SHMMIN; // 如果小于最小值，则调整为最小值
        // }
        
        if (size > SHMMAX) {
            printfRed("[ShmManager] Size 0x%x exceeds SHMMAX (0x%x)\n", size, SHMMAX);
            return -EINVAL;
        }
        
        // 检查系统限制 - 最大段数量
        const int SHMMNI = 4096;  // 最大共享内存标识符数量
        if (segments->size() >= SHMMNI) {
            printfRed("[ShmManager] Maximum number of segments reached (%d)\n", SHMMNI);
            return -ENOSPC;
        }
        
        // 使用新的内存分配方法
        uint64 allocated_addr = allocate_memory(size);
        if (allocated_addr == 0) {
            printfRed("[ShmManager] Failed to allocate memory for size=0x%x\n", size);
            return -ENOMEM; // 内存不足
        }

        // 创建新的共享内存段
        shm_segment new_seg = {};
        new_seg.shmid = next_shmid++;
        new_seg.key = key;
        new_seg.size = size;                      // 保存用户请求的原始大小
        new_seg.real_size = PGROUNDUP(size);      // 页对齐的实际分配大小
        new_seg.shmflg = shmflg;
        new_seg.phy_addrs = allocated_addr;       // 设置分配得到的物理地址
        new_seg.attached_addrs.clear();          // 初始化附加地址列表为空
        
        // 初始化时间信息 (按照标准)
        new_seg.atime = 0;                    // shm_atime 设为 0
        new_seg.dtime = 0;                    // shm_dtime 设为 0  
        new_seg.ctime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME);             // shm_ctime 设为当前时间
        
        // 初始化进程信息
        proc::Pcb* current_proc = proc::k_pm.get_cur_pcb();
        new_seg.creator_pid = current_proc->_pid;     // shm_cpid
        new_seg.last_pid = 0;                         // shm_lpid 设为 0
        
        // 初始化权限信息 (按照标准)
        new_seg.owner_uid = current_proc->_euid;      // shm_perm.uid = effective user ID
        new_seg.owner_gid = current_proc->_egid;      // shm_perm.gid = effective group ID  
        new_seg.creator_uid = current_proc->_euid;    // shm_perm.cuid = effective user ID
        new_seg.creator_gid = current_proc->_egid;    // shm_perm.cgid = effective group ID
        new_seg.mode = shmflg & 0777;                 // 权限位为 shmflg 的低9位
        
        // 初始化状态信息 (按照标准)
        new_seg.nattch = 0;                           // shm_nattch 设为 0
        new_seg.seq = 0;                              // 初始序列号
        
        // 清零段内容 (按照标准要求) - 使用实际分配的大小
        memset((void *)allocated_addr, 0, new_seg.real_size); // 清零物理内存

        segments->insert({new_seg.shmid, new_seg});
        
        printfGreen("[ShmManager] Created new segment shmid=%d, key=0x%x, size=0x%x at phy_addr=0x%x,pid=%d\n",
                    new_seg.shmid, key, new_seg.size, allocated_addr,current_proc->_pid);

        return new_seg.shmid; // 返回新创建的共享内存段ID
    }
    int ShmManager::delete_seg(int shmid)
    {
        auto it = segments->find(shmid);
        if (it == segments->end())
        {
            printfRed("[ShmManager] Segment with shmid=%d not found\n", shmid);
            return -1; // 未找到共享内存段
        }

        shm_segment &seg = it->second;

        // 回收内存到空闲块 - 使用实际分配的大小
        deallocate_memory(seg.phy_addrs, seg.real_size);

        // printfYellow("[ShmManager] Deleted segment shmid=%d, freed addr=0x%x, size=0x%x\n",
        //             shmid, seg.phy_addrs, seg.size);

        segments->erase(it); // 从容器中删除共享内存段
        return 0;
    }
    void *ShmManager::attach_seg(int shmid, void *shmaddr, int shmflg)
    {
        // 查找共享内存段
        auto it = segments->find(shmid);
        if (it == segments->end())
        {
            printfRed("[ShmManager] Segment with shmid=%d not found\n", shmid);
            return (void *)-EINVAL; // 无效的共享内存标识符
        }

        shm_segment &seg = it->second;
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();

        // 权限检查
        bool need_write = !(shmflg & SHM_RDONLY);
        if (!check_segment_attach_permission(seg, current_proc->_uid, current_proc->_gid, need_write)) {
            printfRed("[ShmManager] Permission denied for shmid=%d (uid=%d, need_write=%d)\n", 
                     shmid, current_proc->_uid, need_write);
            return (void *)-EACCES;
        }

        // 检查进程附加段数量限制
        const int SHMSEG_MAX = 500;  // 每个进程最大附加段数量
        int current_attachments = 0;
        for (const auto& pair : *segments) {
            current_attachments += pair.second.attached_addrs.size();  // 统计所有附加地址
        }
        if (current_attachments >= SHMSEG_MAX) {
            printfRed("[ShmManager] Process attachment limit exceeded (%d/%d)\n", 
                     current_attachments, SHMSEG_MAX);
            return (void *)-EMFILE;
        }

        // 确定映射地址
        uint64 attach_addr = 0;

        if (shmaddr == nullptr)
        {
            // 情况1：shmaddr为NULL，系统选择第一个可用地址
            // 在进程的堆区域后面分配，确保不与现有内存重叠
            attach_addr = find_available_address(current_proc, seg.real_size);
            if (attach_addr == 0) {
                printfRed("[ShmManager] No available address space for segment size=0x%x\n", seg.size);
                return (void *)-ENOMEM;
            }
            printfCyan("[ShmManager] System selected address: 0x%x\n", attach_addr);
        }
        else
        {
            // 情况2和3：shmaddr不为NULL
            uint64 requested_addr = (uint64)shmaddr;

            if (shmflg & SHM_RND)
            {
                // 情况2：设置了SHM_RND标记，向下调整到SHMLBA的整数倍
                attach_addr = requested_addr - (requested_addr % SHMLBA);
                printfCyan("[ShmManager] Address rounded down from 0x%x to 0x%x (SHMLBA=%d)\n",
                           requested_addr, attach_addr, SHMLBA);
            }
            else
            {
                // 情况3：使用指定的地址，必须精确匹配
                attach_addr = requested_addr;
                printfCyan("[ShmManager] Using exact specified address: 0x%x\n", attach_addr);
            }

            // 验证地址的合法性 - 使用实际映射大小
            if (!is_valid_attach_address(attach_addr, seg.real_size, shmflg & SHM_RND)) {
                printfRed("[ShmManager] Illegal address 0x%x for attaching shared memory\n", attach_addr);
                return (void *)-EINVAL;
            }

            // 检查地址是否与现有映射冲突 - 使用实际映射大小
            if (has_address_conflict(current_proc, attach_addr, seg.real_size)) {
                printfRed("[ShmManager] Address 0x%x conflicts with existing mappings\n", attach_addr);
                return (void *)-EINVAL;
            }
        }

        // 设置页表权限标志
        int flags = 0;
#ifdef RISCV
        flags |= PTE_U; // 用户可访问
#elif defined(LOONGARCH)
        flags |= PTE_MAT | PTE_PLV | PTE_D | PTE_P; // 用户可访问
#endif

        // 根据shmflg和权限设置读写权限
        if (shmflg & SHM_RDONLY)
        {
            flags |= PTE_R; // 只读权限
            printfCyan("[ShmManager] Attaching with READ-ONLY permissions\n");
        }
        else if(shmflg & SHM_NONE)
        {
            flags =0;
        }
        else
        {
            flags |= PTE_R | PTE_W; // 读写权限
            printfCyan("[ShmManager] Attaching with READ-WRITE permissions\n");
        }

        // 建立物理内存和虚拟内存的映射 - 使用实际分配的页对齐大小
        bool map_result = mem::k_vmm.map_pages(
            *current_proc->get_pagetable(),     // 当前进程页表
            attach_addr,           // 虚拟地址
            seg.real_size,         // 映射大小（页对齐）
            seg.phy_addrs,         // 物理地址
            flags                  // 权限标志
        );

        if (!map_result)
        {
            printfRed("[ShmManager] Failed to map pages for shmid=%d at addr=0x%x\n", shmid, attach_addr);
            return (void *)-ENOMEM;  // 数据空间不足
        }

    // 按标准更新段信息
    seg.attached_addrs.push_back(attached_entry{current_proc->get_tid(), (void *)attach_addr});        // 记录映射的虚拟地址（带tid）
        seg.atime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME); // 设置shm_atime为当前时间
        seg.last_pid = current_proc->_pid;     // 更新最后操作进程ID (shm_lpid)
        seg.nattch++;                          // 增加附加计数 (shm_nattch)

        // 注意：在新的内存管理体系中，共享内存不直接计入进程的_sz
        // _sz现在由程序段和堆的总和自动计算，共享内存有独立的生命周期管理
        // 如果需要更新总内存大小，应该调用进程的update_total_memory_size()方法
            // printfCyan("[ShmManager::detach_seg] Attached addresses: ");
            // for (void* attached_addr : seg.attached_addrs) {
            //     printfCyan("%p ", attached_addr);
            // }
        printfGreen("[ShmManager] Successfully attached segment shmid=%d at address 0x%x, user_size=0x%x, real_size=0x%x\n",
                    shmid, attach_addr, seg.size, seg.real_size);

        return (void *)attach_addr; // 返回段的起始地址
    }

    int ShmManager::detach_seg(void *addr)
    {
        printfCyan("[ShmManager::detach_seg] Looking for address: %p\n", addr);
        
        auto it = segments->begin();
    // 查找包含该地址的共享内存段（限定当前线程）
        for (; it != segments->end(); ++it)
        {
            shm_segment &seg = it->second;
            // printfCyan("[ShmManager::detach_seg] Checking segment shmid=%d\n", seg.shmid);
            // //打印地址列表
            // printfCyan("[ShmManager::detach_seg] Attached addresses: ");
            // for (void* attached_addr : seg.attached_addrs) {
            //     printfCyan("%p ", attached_addr);
            // }
            // printfCyan("\n");
            // 在附加地址列表中查找（tid + 地址匹配）
            uint cur_tid = proc::k_pm.get_cur_pcb()->get_tid();
            auto addr_it = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry& e){
                return e.tid == cur_tid && e.addr == addr;
            });
            if (addr_it != seg.attached_addrs.end()) {
                // printfCyan("[ShmManager::detach_seg] Found address in segment shmid=%d\n", seg.shmid);
                break;
            }
        }
        
        if (it == segments->end())
        {
            printfRed("[ShmManager] Segment with address %p not found\n", addr);
            printfYellow("[ShmManager] Available segments:\n");
            for (const auto& pair : *segments) {
                printfYellow("  shmid=%d, attachments: ", pair.second.shmid);
                for (const auto &ent : pair.second.attached_addrs) {
                    printfYellow("(tid=%d, addr=%p) ", ent.tid, ent.addr);
                }
                printfYellow("\n");
            }
            return -EINVAL; // 地址无效
        }
        
        shm_segment &seg = it->second;
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        
        // 从附加地址列表中移除这个地址（仅当前线程）
        {
            uint cur_tid = current_proc->get_tid();
            auto it2 = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry& e){
                return e.tid == cur_tid && e.addr == addr;
            });
            printfRed("[ShmManager] Detaching segment shmid=%d from address %p (tid=%d)\n",
                     seg.shmid, addr, cur_tid);
            if (it2 != seg.attached_addrs.end()) {
                seg.attached_addrs.erase(it2);
            }
        }
        
        // 解除映射 - 使用实际分配的页对齐大小
        mem::k_vmm.vmunmap(
            *current_proc->get_pagetable(),               // 当前进程页表
            (uint64)addr,                    // 虚拟地址
            seg.real_size / PGSIZE,          // 页数（使用实际分配大小）
            0                                // 不释放物理页
        );
        
        // 按标准更新段信息
        seg.dtime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME);                    // 设置shm_dtime为当前时间
        seg.last_pid = current_proc->_pid;       // 更新最后操作进程ID (shm_lpid)
        seg.nattch--;                            // 减少附加计数 (shm_nattch)

        printfCyan("[ShmManager] Detached segment shmid=%d at addr=%p (nattch now %d)\n", 
                  seg.shmid, addr, seg.nattch);

        // 检查段是否被标记为删除且无进程附加
        if ((seg.mode & SHM_DEST) && seg.nattch == 0) {
            int shmid = seg.shmid;  // 保存shmid用于日志
            int result = delete_seg(shmid);
            if (result == 0) {
                printfGreen("[ShmManager] Auto-destroyed marked segment shmid=%d after last detach\n", shmid);
            } else {
                printfRed("[ShmManager] Failed to auto-destroy marked segment shmid=%d\n", shmid);
            }
        }

        return 0;
    }

    bool ShmManager::is_shared_memory_address(void *addr)
    {
        if (!addr) {
            return false;
        }

        uint cur_tid = proc::k_pm.get_cur_pcb()->get_tid();
        // 遍历所有共享内存段，仅匹配当前线程的记录
        for (auto it = segments->begin(); it != segments->end(); ++it)
        {
            shm_segment &seg = it->second;
            auto addr_it = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry& e){
                return e.tid == cur_tid && e.addr == addr;
            });
            if (addr_it != seg.attached_addrs.end()) {
                return true;
            }
        }
        return false;
    }

    int ShmManager::find_shared_memory_segment(void *addr, void **start_addr, size_t *size)
    {
        if (!addr) {
            return -1;
        }

        uint64 target_addr = (uint64)addr;

        uint cur_tid = proc::k_pm.get_cur_pcb()->get_tid();
        // printf("[ShmManager] Finding shared memory segment for address: %p (tid=%d)\n", addr, cur_tid);
        // 遍历所有共享内存段（只看当前线程）
        for (auto it = segments->begin(); it != segments->end(); ++it)
        {
            shm_segment &seg = it->second;
            
            // 检查每个附加地址及其范围
            for (const auto& e : seg.attached_addrs) {
                if (e.tid != cur_tid) continue;
                uint64 seg_start = (uint64)e.addr;
                uint64 seg_end = seg_start + seg.real_size;
                
                // 检查目标地址是否在这个段的范围内
                if (target_addr >= seg_start && target_addr < seg_end) {
                    if (start_addr) {
                        *start_addr = e.addr;
                    }
                    if (size) {
                        *size = seg.real_size;
                    }
                    return e.tid;  // 返回找到的共享内存段ID
                }
            }
        }
        
        return -1;
    }

    bool ShmManager::add_reference_for_fork(void *addr)
    {
        if (!addr) {
            return false;
        }

        uint cur_tid = proc::k_pm.get_cur_pcb()->get_tid();
        // 遍历所有共享内存段，找到包含该地址的段（限定当前线程）
        for (auto it = segments->begin(); it != segments->end(); ++it)
        {
            shm_segment &seg = it->second;
            
            // 在附加地址列表中查找
            auto addr_it = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry& e){
                return e.tid == cur_tid && e.addr == addr;
            });
            if (addr_it != seg.attached_addrs.end()) {
                // 找到了包含该地址的共享内存段，增加引用计数
                seg.nattch++;
                printfCyan("[ShmManager] Fork: increased reference count for shared memory at %p, shmid=%d, nattch=%d\n", 
                          addr, seg.shmid, seg.nattch);
                return true;
            }
        }
        
        return false;
    }

    int ShmManager::shmctl(int shmid, int cmd, struct shmid_ds *buf,uint64 buf_addr)
    {
        proc::Pcb* current_proc = proc::k_pm.get_cur_pcb();

        switch (cmd) {
            case IPC_STAT:
            case SHM_STAT:
            case SHM_STAT_ANY:
            {
                // 查找共享内存段
                auto it = segments->end();
                if (cmd == SHM_STAT || cmd == SHM_STAT_ANY) {
                    // SHM_STAT 系列: shmid 是索引而不是标识符
                    if (shmid < 0 || (size_t)shmid >= segments->size()) {
                        printfRed("[ShmManager] Invalid index %d for SHM_STAT\n", shmid);
                        return -EINVAL;
                    }
                    // 找到第shmid个段
                    int index = 0;
                    for (auto iter = segments->begin(); iter != segments->end(); ++iter, ++index) {
                        if (index == shmid) {
                            it = iter;
                            break;
                        }
                    }
                } else {
                    // IPC_STAT: shmid 是段标识符
                    it = segments->find(shmid);
                }

                if (it == segments->end()) {
                    printfRed("[ShmManager] Segment not found for cmd=%d, shmid=%d\n", cmd, shmid);
                    return -EINVAL;
                }

                shm_segment& seg = it->second;

                // 权限检查 (SHM_STAT_ANY 不需要权限检查)
                if (cmd != SHM_STAT_ANY) {
                    if (!check_segment_read_permission(seg, current_proc->_uid, current_proc->_gid)) {
                        printfRed("[ShmManager] Read permission denied for shmid=%d\n", 
                                 cmd == SHM_STAT ? it->second.shmid : shmid);
                        return -EACCES;
                    }
                }

                if (buf == nullptr) {
                    printfRed("[ShmManager] buf is null for IPC_STAT\n");
                    return -EFAULT;
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
                printfCyan("[ShmManager] copy_out pt:%p,va:0x%x, kernel_buf:%p, size:%u\n",
                          *current_proc->get_pagetable(), buf_addr, &kernel_buf, sizeof(kernel_buf));
                if (mem::k_vmm.copy_out(*current_proc->get_pagetable(), buf_addr, &kernel_buf, sizeof(kernel_buf)) < 0) {
                    printfRed("[ShmManager] Failed to copy shmid_ds to user space\n");
                    return -EFAULT;
                }

                printfCyan("[ShmManager] IPC_STAT: shmid=%d, size=0x%x, nattch=%d\n", 
                          seg.shmid, seg.size, seg.nattch);

                // SHM_STAT 返回实际的段标识符
                return (cmd == SHM_STAT) ? seg.shmid : 0;
            }

            case IPC_SET:
            {
                // 查找共享内存段
                auto it = segments->find(shmid);
                if (it == segments->end()) {
                    printfRed("[ShmManager] Segment with shmid=%d not found for IPC_SET\n", shmid);
                    return -EINVAL;
                }

                shm_segment& seg = it->second;

                if (buf == nullptr) {
                    printfRed("[ShmManager] buf is null for IPC_SET\n");
                    return -EFAULT;
                }

                // 检查权限：只有所有者或创建者可以修改
                if (current_proc->_euid != seg.owner_uid && 
                    current_proc->_euid != seg.creator_uid &&
                    current_proc->_euid != 0) {  // root用户
                    printfRed("[ShmManager] Permission denied for IPC_SET (uid=%d, owner=%d, creator=%d)\n",
                             current_proc->_euid, seg.owner_uid, seg.creator_uid);
                    return -EPERM;
                }

                // 从用户空间复制数据
                struct shmid_ds user_buf;
                if (mem::k_vmm.copy_in(*current_proc->get_pagetable(), &user_buf, buf_addr, sizeof(user_buf)) < 0) {
                    printfRed("[ShmManager] Failed to copy shmid_ds from user space\n");
                    return -EFAULT;
                }

                // 按标准更新可修改的字段
                seg.owner_uid = user_buf.shm_perm.uid;
                seg.owner_gid = user_buf.shm_perm.gid;
                seg.mode = (seg.mode & ~0777) | (user_buf.shm_perm.mode & 0777);  // 只更新低9位权限
                seg.ctime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME);  // 更新修改时间
                seg.last_pid = current_proc->_pid;

                printfCyan("[ShmManager] IPC_SET: shmid=%d, new mode=0%x, new uid=%d\n", 
                          shmid, seg.mode, seg.owner_uid);
                break;
            }

            case IPC_RMID:
            {
                // 查找共享内存段
                auto it = segments->find(shmid);
                if (it == segments->end()) {
                    printfRed("[ShmManager] Segment with shmid=%d not found for IPC_RMID\n", shmid);
                    return -EINVAL;
                }

                shm_segment& seg = it->second;

                // 检查权限：只有所有者或创建者可以删除
                if (current_proc->_euid != seg.owner_uid && 
                    current_proc->_euid != seg.creator_uid &&
                    current_proc->_euid != 0) {  // root用户
                    printfRed("[ShmManager] Permission denied for IPC_RMID\n");
                    return -EPERM;
                }

                // 标记段为待删除 - 设置 SHM_DEST 标志
                seg.mode |= SHM_DEST;
                
                // 如果还有进程附加到这个段，暂时不删除
                if (seg.nattch > 0) {
                    printfYellow("[ShmManager] IPC_RMID: shmid=%d marked for destruction, %d attachments remain\n", 
                                shmid, seg.nattch);
                    return 0;  // 成功标记，但暂不删除
                }

                // 没有进程附加，立即删除
                int result = delete_seg(shmid);
                if (result == 0) {
                    printfGreen("[ShmManager] IPC_RMID: shmid=%d immediately destroyed\n", shmid);
                } else {
                    printfRed("[ShmManager] IPC_RMID: failed to destroy shmid=%d\n", shmid);
                }
                return result;
            }

            case IPC_INFO:
            {
                if (buf == nullptr) {
                    printfRed("[ShmManager] buf is null for IPC_INFO\n");
                    return -EFAULT;
                }

                // 创建系统限制信息
                struct shminfo sys_info = {};
                sys_info.shmmax = 32 * 1024 * 1024;  // 最大段大小 32MB
                sys_info.shmmin = 1;             // 最小段大小
                sys_info.shmmni = 4096;               // 最大段数量
                sys_info.shmseg = 128;                // 每进程最大段数(未使用)
                sys_info.shmall = (shm_size / PGSIZE); // 系统总页数

                // 复制到用户空间
                if (mem::k_vmm.copy_out(*current_proc->get_pagetable(), buf_addr, &sys_info, sizeof(sys_info)) < 0) {
                    printfRed("[ShmManager] Failed to copy shminfo to user space\n");
                    return -EFAULT;
                }

                printfCyan("[ShmManager] IPC_INFO: returned system limits\n");
                //TODO：这你妈不对，明天再改了
                // 计算最高使用的索引
                int max_index = -1;
                for (const auto& pair : *segments) {
                    if (pair.first > max_index) {
                        max_index = pair.first;
                    }
                }
                return 0;
            }

            case SHM_INFO:
            {
                if (buf == nullptr) {
                    printfRed("[ShmManager] buf is null for SHM_INFO\n");
                    return -EFAULT;
                }

                // 创建系统资源使用信息
                struct shm_info usage_info = {};
                usage_info.used_ids = segments->size();
                
                size_t total_pages = 0;
                for (const auto& pair : *segments) {
                    total_pages += pair.second.real_size / PGSIZE;  // 使用实际分配大小
                }
                
                usage_info.shm_tot = total_pages;
                usage_info.shm_rss = total_pages;  // 简化：假设都在内存中
                usage_info.shm_swp = 0;            // 简化：没有交换
                usage_info.swap_attempts = 0;      // 未使用
                usage_info.swap_successes = 0;     // 未使用

                // 复制到用户空间
                if (mem::k_vmm.copy_out(*current_proc->get_pagetable(), buf_addr, &usage_info, sizeof(usage_info)) < 0) {
                    printfRed("[ShmManager] Failed to copy shm_info to user space\n");
                    return -EFAULT;
                }

                printfCyan("[ShmManager] SHM_INFO: used_ids=%d, total_pages=%u\n", 
                          usage_info.used_ids, usage_info.shm_tot);
                
                // 计算最高使用的索引
                int max_index = -1;
                for (const auto& pair : *segments) {
                    if (pair.first > max_index) {
                        max_index = pair.first;
                    }
                }
                return max_index;
            }

            case SHM_LOCK:
            case SHM_UNLOCK:
            {
                // 查找共享内存段
                auto it = segments->find(shmid);
                if (it == segments->end()) {
                    printfRed("[ShmManager] Segment with shmid=%d not found for SHM_LOCK/UNLOCK\n", shmid);
                    return -EINVAL;
                }

                shm_segment& seg = it->second;

                // 检查权限：所有者、创建者或root
                if (current_proc->_euid != seg.owner_uid && 
                    current_proc->_euid != seg.creator_uid &&
                    current_proc->_euid != 0) {
                    printfRed("[ShmManager] Permission denied for SHM_LOCK/UNLOCK\n");
                    return -EPERM;
                }

                // 简化实现：只设置/清除标志
                if (cmd == SHM_LOCK) {
                    seg.mode |= SHM_LOCKED;
                    printfCyan("[ShmManager] SHM_LOCK: shmid=%d locked\n", shmid);
                } else {
                    seg.mode &= ~SHM_LOCKED;
                    printfCyan("[ShmManager] SHM_UNLOCK: shmid=%d unlocked\n", shmid);
                }
                
                return 0;
            }

            default:
                printfRed("[ShmManager] Unknown shmctl command: %d\n", cmd);
                return -EINVAL;
        }

        return 0;
    }

    shm_segment ShmManager::get_seg_info(int shmid)
    {
        auto it = segments->find(shmid);
        if (it != segments->end())
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
        auto it = segments->find(shmid);
        if (it == segments->end())
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
        printfYellow("  Total memory: 0x%x bytes\n", shm_size);
        printfYellow("  Active segments: %u\n", segments->size());
        printfYellow("  Free blocks: %u\n", free_blocks->size());

        size_t total_free = 0;
        for (const auto &block : *free_blocks)
        {
            printfYellow("    Free block: addr=0x%x, size=0x%x\n", block.addr, block.size);
            total_free += block.size;
        }
        printfYellow("  Total free memory: 0x%x bytes\n", total_free);
        printfYellow("  Memory utilization: %.1f%%\n",
                     (double)(shm_size - total_free) * 100.0 / shm_size);
    }

    size_t ShmManager::get_total_free_memory() const
    {
        size_t total_free = 0;
        for (const auto &block : *free_blocks)
        {
            total_free += block.size;
        }
        return total_free;
    }

    size_t ShmManager::get_largest_free_block() const
    {
        size_t largest = 0;
        for (const auto &block : *free_blocks)
        {
            if (block.size > largest)
            {
                largest = block.size;
            }
        }
        return largest;
    }
}

namespace shm {
    bool ShmManager::duplicate_attachments_for_fork(uint parent_tid, uint child_tid)
    {
        bool duplicated = false;
        for (auto &pair : *segments) {
            shm_segment &seg = pair.second;
            for (const auto &e : seg.attached_addrs) {
                if (e.tid == parent_tid) {
                    seg.attached_addrs.push_back(attached_entry{child_tid, e.addr});
                    seg.nattch++;
                    duplicated = true;
                }
            }
        }
        if (duplicated) {
            proc::k_pm.get_cur_pcb()->get_memory_manager()->print_memory_usage();
            printfCyan("[ShmManager] Fork: duplicated attachments from tid=%d to tid=%d\n", parent_tid, child_tid);
        }
        return duplicated;
    }
}