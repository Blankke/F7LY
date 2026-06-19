#include "shm_manager.hh"
#include "platform.hh"
#include "proc_manager.hh"
#include "process_memory_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include <EASTL/algorithm.h>
#include "virtual_memory_manager.hh"
#include "memlayout.hh" // 为了获取PGSIZE等定义
#include "mem.hh"
#include "fs/lwext4/ext4_errno.hh"  // 为了获取错误码定义
#include "tm/timer_manager.hh"
#include "proc/vm_object.hh"
namespace shm
{
    ShmManager k_smm; // 全局共享内存管理器实例

    namespace
    {
        class SpinLockGuard
        {
        public:
            explicit SpinLockGuard(SpinLock &lock) : lock_(lock)
            {
                lock_.acquire();
            }

            ~SpinLockGuard()
            {
                lock_.release();
            }

            SpinLockGuard(const SpinLockGuard &) = delete;
            SpinLockGuard &operator=(const SpinLockGuard &) = delete;

        private:
            SpinLock &lock_;
        };

        class MemoryLockGuard
        {
        public:
            explicit MemoryLockGuard(proc::ProcessMemoryManager *mm) : mm_(mm)
            {
                if (mm_ != nullptr)
                {
                    mm_->lock_memory();
                }
            }

            ~MemoryLockGuard()
            {
                if (mm_ != nullptr)
                {
                    mm_->unlock_memory();
                }
            }

            MemoryLockGuard(const MemoryLockGuard &) = delete;
            MemoryLockGuard &operator=(const MemoryLockGuard &) = delete;

        private:
            proc::ProcessMemoryManager *mm_;
        };

        inline bool is_vma_backed_shared_attachment(proc::Pcb *proc, uint64 addr)
        {
            if (proc == nullptr || proc->get_memory_manager() == nullptr)
            {
                return false;
            }

            bool matched = false;
            proc->get_memory_manager()->for_each_vma([&](const proc::vma &vm) -> bool
            {
                if (vm.backing_kind == proc::VMA_BACKING_SHM && vm.backing_base == addr)
                {
                    matched = true;
                    return false;
                }
                return true;
            });
            return matched;
        }

        inline uint64 current_ipc_namespace_id()
        {
            proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
            return pcb != nullptr ? pcb->_ipc_ns_id : proc::k_initial_ipc_namespace_id;
        }

        inline bool segment_visible_in_current_namespace(const shm_segment &seg)
        {
            return seg.ipc_ns_id == current_ipc_namespace_id();
        }
    } // namespace

    void ShmManager::init(uint64 base, uint64 size)
    {
        shm_lock_.init("shm_manager");
        (void)base;
        shm_size = size;
        next_shmid = 1; // shmid从1开始
        shmmax_limit = 32 * 1024 * 1024; // Linux sysctl shmmax，默认与现有 IPC_INFO 保持一致
        shmmni_limit = 4096;             // Linux sysctl shmmni，LTP 会读取该值做 ENOSPC 压测

        segments = new eastl::unordered_map<int, shm_segment>();
        shared_file_objects = new eastl::unordered_map<eastl::string, proc::FileVmObject *>();
        registered_objects = new eastl::unordered_map<uint64, proc::VmObject *>();
    }

    eastl::unordered_map<int, shm_segment>::iterator ShmManager::find_segment_by_key_locked(key_t key, uint64 ipc_ns_id)
    {
        for (auto it = segments->begin(); it != segments->end(); ++it) {
            // IPC_RMID 后的段仍可能因 nattch>0 保留到最后一次 shmdt，
            // 但 Linux 会立即把 key 从查找空间移除，允许同 key 新建段。
            if (it->second.key == key &&
                it->second.ipc_ns_id == ipc_ns_id &&
                (it->second.mode & SHM_DEST) == 0) {
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
        if (proc == nullptr || proc->get_memory_manager() == nullptr)
        {
            return 0;
        }

        // 共享段也统一从 mmap 区域分配地址，避免再和 brk 堆边界互相踩踏。
        return proc->get_memory_manager()->reserve_mmap_region(size, SHMLBA);
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

        // 指定地址 shmat 只能看统一 VMA 视图；ELF、堆、栈、mmap/shm 都已经在这里建模。
        if (proc->get_vma() != nullptr && proc->get_memory_manager() != nullptr) {
            if (proc->get_memory_manager()->has_vma_conflict(addr, end_addr)) {
                const proc::vma *conflict = proc->get_memory_manager()->find_vma_covering(addr);
                if (conflict == nullptr) {
                    conflict = proc->get_memory_manager()->find_first_vma_at_or_after(addr);
                }

                if (conflict != nullptr && conflict->addr < end_addr) {
                    printfRed("[ShmManager] Address range [0x%x, 0x%x] conflicts with VMA [%p, %p]\n",
                             addr, end_addr, (void *)conflict->addr, (void *)conflict->end_addr());
                }
                return true;
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
        return create_seg_in_namespace(key, size, shmflg, current_ipc_namespace_id());
    }

    int ShmManager::create_seg_in_namespace(key_t key, size_t size, int shmflg, uint64 ipc_ns_id)
    {
        SpinLockGuard guard(shm_lock_);

        // 处理 IPC_PRIVATE 情况 - 总是创建新段
        if (key == IPC_PRIVATE) {
            return create_new_segment_locked(key, size, shmflg, ipc_ns_id);
        }

        // 查找是否已存在相同key的段 - 先检查容器是否为空
        if (segments->empty()) {
            // 容器为空，直接跳到创建新段的逻辑
            if (!(shmflg & IPC_CREAT)) {
                printfRed("[ShmManager] No segment exists for key=0x%x and IPC_CREAT not specified\n", key);
                return -ENOENT;  // 段不存在且未指定 IPC_CREAT
            }
            // 创建新段
            return create_new_segment_locked(key, size, shmflg, ipc_ns_id);
        }
        
        // 容器不为空，安全地查找
        auto existing_seg = find_segment_by_key_locked(key, ipc_ns_id);

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
            
            return seg.shmid;  // 返回现有段的ID
        } 
        else {
            // 段不存在的情况
            if (!(shmflg & IPC_CREAT)) {
                printfRed("[ShmManager] No segment exists for key=0x%x and IPC_CREAT not specified\n", key);
                return -ENOENT;  // 段不存在且未指定 IPC_CREAT
            }
            
            // 创建新段
            return create_new_segment_locked(key, size, shmflg, ipc_ns_id);
        }
    }
    
    int ShmManager::create_new_segment_locked(key_t key, size_t size, int shmflg, uint64 ipc_ns_id)
    {
        // SysV SHM 的 ABI 下，新建段必须满足 SHMMIN<=size<=shmmax。
        // shmget02 会通过 /proc/sys/kernel/shmmax 临时下调上限后验证这里。
        if (size == 0) {
            printfRed("[ShmManager] Size 0 is less than SHMMIN\n");
            return -EINVAL;
        }

        if (shmflg & SHM_HUGETLB) {
            // 当前教学内核没有 hugetlbfs/huge page 池，按 Linux 未启用 CONFIG_HUGETLBFS 的语义拒绝。
            return -EINVAL;
        }

        if (size > shmmax_limit) {
            printfRed("[ShmManager] Size 0x%x exceeds SHMMAX (0x%x)\n", size, shmmax_limit);
            return -EINVAL;
        }
        
        int namespace_segment_count = 0;
        for (const auto &pair : *segments)
        {
            if (pair.second.ipc_ns_id == ipc_ns_id)
            {
                ++namespace_segment_count;
            }
        }

        if (namespace_segment_count >= shmmni_limit) {
            printfRed("[ShmManager] Maximum number of namespace segments reached (%d)\n", shmmni_limit);
            return -ENOSPC;
        }
        
        // 创建新的共享内存段
        shm_segment new_seg = {};
        new_seg.shmid = next_shmid++;
        new_seg.ipc_ns_id = ipc_ns_id;
        new_seg.key = key;
        new_seg.size = size;                      // 保存用户请求的原始大小
        new_seg.real_size = PGROUNDUP(size);      // 页对齐的实际分配大小
        new_seg.shmflg = shmflg;
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
        new_seg.auto_destroy_on_last_detach = false; // 默认遵循 SysV SHM 生命周期
        new_seg.seq = 0;                              // 初始序列号

        proc::SysvShmMetadata meta = {};
        meta.shmid = new_seg.shmid;
        meta.ipc_ns_id = new_seg.ipc_ns_id;
        meta.key = new_seg.key;
        meta.size = new_seg.size;
        meta.real_size = new_seg.real_size;
        meta.shmflg = new_seg.shmflg;
        meta.atime = new_seg.atime;
        meta.dtime = new_seg.dtime;
        meta.ctime = new_seg.ctime;
        meta.creator_pid = new_seg.creator_pid;
        meta.last_pid = new_seg.last_pid;
        meta.owner_uid = new_seg.owner_uid;
        meta.owner_gid = new_seg.owner_gid;
        meta.creator_uid = new_seg.creator_uid;
        meta.creator_gid = new_seg.creator_gid;
        meta.mode = new_seg.mode;
        meta.nattch = new_seg.nattch;
        meta.auto_destroy_on_last_detach = new_seg.auto_destroy_on_last_detach;
        meta.seq = new_seg.seq;
        new_seg.object = new proc::SysvShmVmObject(meta);
        if (new_seg.object == nullptr)
        {
            return -ENOMEM;
        }

        segments->insert({new_seg.shmid, new_seg});
        
        return new_seg.shmid; // 返回新创建的共享内存段ID
    }
    int ShmManager::delete_seg(int shmid)
    {
        SpinLockGuard guard(shm_lock_);
        return delete_seg_locked(shmid);
    }

    int ShmManager::delete_seg_locked(int shmid)
    {
        auto it = segments->find(shmid);
        if (it == segments->end())
        {
            printfRed("[ShmManager] Segment with shmid=%d not found\n", shmid);
            return -EINVAL; // 未找到共享内存段
        }

        shm_segment &seg = it->second;

        if (seg.object != nullptr)
        {
            proc::SysvShmVmObject *obj = seg.object;
            seg.object = nullptr;
            if (obj->put())
            {
                delete obj;
            }
        }

        segments->erase(it); // 从容器中删除共享内存段
        return 0;
    }

    proc::FileVmObject *ShmManager::acquire_shared_file_object(fs::file *file_obj)
    {
        if (file_obj == nullptr)
        {
            return nullptr;
        }

        const eastl::string &cache_key = file_obj->backing_path();
        if (cache_key.empty())
        {
            // 没有稳定 backing key 的文件对象（例如部分匿名后端/临时句柄）
            // 不能硬塞进同一个空 key 缓存，否则不同映射会被错误共享。
            return new proc::FileVmObject(file_obj, true, false, {});
        }

        {
            SpinLockGuard guard(shm_lock_);
            auto existing = shared_file_objects->find(cache_key);
            if (existing != shared_file_objects->end() && existing->second != nullptr)
            {
                existing->second->get();
                return existing->second;
            }
        }

        // FileVmObject 构造会登记到全局对象表，登记过程也需要 shm_lock_。
        // 因此对象构造必须放在缓存锁外，避免 MAP_SHARED 文件映射触发锁重入 panic。
        proc::FileVmObject *object = new proc::FileVmObject(file_obj, true, false, cache_key);
        if (object == nullptr)
        {
            return nullptr;
        }

        proc::FileVmObject *duplicate_to_drop = nullptr;
        {
            SpinLockGuard guard(shm_lock_);
            auto existing = shared_file_objects->find(cache_key);
            if (existing != shared_file_objects->end() && existing->second != nullptr)
            {
                existing->second->get();
                duplicate_to_drop = object;
                object = existing->second;
            }
            else
            {
                (*shared_file_objects)[cache_key] = object;
                object->get(); // 额外给调用方一份引用；缓存自己保留一份
            }
        }

        if (duplicate_to_drop != nullptr && duplicate_to_drop->put())
        {
            delete duplicate_to_drop;
        }
        return object;
    }

    proc::SysvShmVmObject *ShmManager::acquire_sysv_object(int shmid)
    {
        SpinLockGuard guard(shm_lock_);
        auto it = segments->find(shmid);
        if (it == segments->end() || it->second.object == nullptr)
        {
            return nullptr;
        }

        it->second.object->get();
        return it->second.object;
    }

    void ShmManager::note_object_created(proc::VmObject *object)
    {
        if (object == nullptr || registered_objects == nullptr)
        {
            return;
        }

        if (shm_lock_.is_held())
        {
            (*registered_objects)[object->object_id()] = object;
            return;
        }

        SpinLockGuard guard(shm_lock_);
        (*registered_objects)[object->object_id()] = object;
    }

    void ShmManager::note_object_destroying(const proc::VmObject *object)
    {
        if (object == nullptr || registered_objects == nullptr)
        {
            return;
        }

        if (shm_lock_.is_held())
        {
            registered_objects->erase(object->object_id());

            const eastl::string *cache_key = object->shared_cache_key();
            if (cache_key == nullptr || shared_file_objects == nullptr)
            {
                return;
            }

            auto it = shared_file_objects->find(*cache_key);
            if (it != shared_file_objects->end() && it->second == object)
            {
                shared_file_objects->erase(it);
            }
            return;
        }

        SpinLockGuard guard(shm_lock_);
        registered_objects->erase(object->object_id());

        const eastl::string *cache_key = object->shared_cache_key();
        if (cache_key == nullptr || shared_file_objects == nullptr)
        {
            return;
        }

        auto it = shared_file_objects->find(*cache_key);
        if (it != shared_file_objects->end() && it->second == object)
        {
            shared_file_objects->erase(it);
        }
    }

    void ShmManager::release_shared_file_object_if_unused(proc::VmObject *object)
    {
        if (object == nullptr || shared_file_objects == nullptr)
        {
            return;
        }

        const eastl::string *cache_key = object->shared_cache_key();
        if (cache_key == nullptr)
        {
            return;
        }

        SpinLockGuard guard(shm_lock_);
        auto it = shared_file_objects->find(*cache_key);
        if (it == shared_file_objects->end() || it->second != object)
        {
            return;
        }

        // 当前只有“缓存引用 + 本次 area 引用”时，才把缓存主动撤掉，
        // 避免临时文件/短生命周期共享映射把对象一直留在全局索引里。
        if (object->ref_count_for_debug() != 2)
        {
            return;
        }

        shared_file_objects->erase(it);
        if (object->put())
        {
            delete object;
        }
    }
    void ShmManager::remove_attachment_record_locked(shm_segment &seg, uint tid, pid_t pid, void *addr)
    {
        auto addr_it = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry &entry)
        {
            return entry.addr == addr && (entry.tid == tid || entry.pid == pid);
        });
        if (addr_it == seg.attached_addrs.end())
        {
            return;
        }

        seg.attached_addrs.erase(addr_it);
        seg.dtime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME);
        if (seg.nattch > 0)
        {
            seg.nattch--;
        }
        if (proc::Pcb *current_proc = proc::k_pm.get_cur_pcb(); current_proc != nullptr)
        {
            seg.last_pid = current_proc->_pid;
        }
    }

    bool ShmManager::lookup_attachment_locked(uint tid, pid_t pid, void *addr, int *shmid, size_t *real_size)
    {
        for (auto &pair : *segments)
        {
            shm_segment &seg = pair.second;
            auto addr_it = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry &entry)
            {
                return entry.addr == addr && (entry.tid == tid || entry.pid == pid);
            });
            if (addr_it == seg.attached_addrs.end())
            {
                continue;
            }

            if (shmid != nullptr)
            {
                *shmid = seg.shmid;
            }
            if (real_size != nullptr)
            {
                *real_size = seg.real_size;
            }
            return true;
        }
        return false;
    }

    void *ShmManager::attach_seg(int shmid, void *shmaddr, int shmflg)
    {
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr)
        {
            return (void *)-ESRCH;
        }

        proc::ProcessMemoryManager *current_mm = current_proc->get_memory_manager();
        if (current_mm == nullptr)
        {
            return (void *)-ENOMEM;
        }
        MemoryLockGuard mm_guard(current_mm);

        int prot = PROT_NONE;
        if (shmflg & SHM_RDONLY)
        {
            prot = PROT_READ;
        }
        else if (!(shmflg & SHM_NONE))
        {
            prot = PROT_READ | PROT_WRITE;
        }

        uint64 attach_addr = 0;
        size_t real_size = 0;
        proc::SysvShmVmObject *object = nullptr;
        {
            SpinLockGuard guard(shm_lock_);
            auto it = segments->find(shmid);
            if (it == segments->end())
            {
                printfRed("[ShmManager] Segment with shmid=%d not found\n", shmid);
                return (void *)-EINVAL;
            }

            shm_segment &seg = it->second;
            if (!segment_visible_in_current_namespace(seg))
            {
                printfRed("[ShmManager] shmid=%d belongs to another IPC namespace\n", shmid);
                return (void *)-EINVAL;
            }

            bool need_write = !(shmflg & SHM_RDONLY);
            if (!check_segment_attach_permission(seg, current_proc->_uid, current_proc->_gid, need_write))
            {
                printfRed("[ShmManager] Permission denied for shmid=%d (uid=%d, need_write=%d)\n",
                          shmid, current_proc->_uid, need_write);
                return (void *)-EACCES;
            }

            const int SHMSEG_MAX = 500;
            int current_attachments = 0;
            for (const auto &pair : *segments)
            {
                current_attachments += pair.second.attached_addrs.size();
            }
            if (current_attachments >= SHMSEG_MAX)
            {
                printfRed("[ShmManager] Process attachment limit exceeded (%d/%d)\n",
                          current_attachments, SHMSEG_MAX);
                return (void *)-EMFILE;
            }

            real_size = seg.real_size;
            if (shmaddr == nullptr)
            {
                attach_addr = find_available_address(current_proc, real_size);
                if (attach_addr == 0)
                {
                    printfRed("[ShmManager] No available address space for segment size=0x%x\n", seg.size);
                    return (void *)-ENOMEM;
                }
            }
            else
            {
                uint64 requested_addr = (uint64)shmaddr;
                attach_addr = (shmflg & SHM_RND) ? (requested_addr - (requested_addr % SHMLBA))
                                                 : requested_addr;
                if (!is_valid_attach_address(attach_addr, real_size, (shmflg & SHM_RND) != 0))
                {
                    printfRed("[ShmManager] Illegal address 0x%x for attaching shared memory\n", attach_addr);
                    return (void *)-EINVAL;
                }
                if (has_address_conflict(current_proc, attach_addr, real_size))
                {
                    printfRed("[ShmManager] Address 0x%x conflicts with existing mappings\n", attach_addr);
                    return (void *)-EINVAL;
                }
            }

            if (seg.object == nullptr)
            {
                return (void *)-EINVAL;
            }

            object = seg.object;
            object->get();
            seg.attached_addrs.push_back(attached_entry{current_proc->get_tid(), current_proc->_pid, (void *)attach_addr});
            seg.atime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME);
            seg.last_pid = current_proc->_pid;
            seg.nattch++;
        }

        if (!current_mm->ensure_user_pagetable_hierarchy(attach_addr, real_size))
        {
            (void)detach_vma_attachment(shmid, (void *)attach_addr, current_proc->get_tid());
            if (object != nullptr && object->put())
            {
                delete object;
            }
            return (void *)-ENOMEM;
        }

        proc::vma *vm = current_mm->get_vm_space().create_area(attach_addr,
                                                               real_size,
                                                               prot,
                                                               MAP_SHARED,
                                                               object,
                                                               0,
                                                               proc::VmAreaKind::SysvShm,
                                                               proc::VmGrowPolicy::None,
                                                               0,
                                                               "sysv-shm");
        if (vm == nullptr)
        {
            (void)detach_vma_attachment(shmid, (void *)attach_addr, current_proc->get_tid());
            // create_area() 接管 object 引用；失败时它已经完成引用归还。
            return (void *)-ENOMEM;
        }

        vm->vfd = -1;
        vm->vfile = nullptr;
        vm->offset = 0;
        vm->max_len = real_size;
        vm->is_expandable = false;
        vm->backing_kind = proc::VMA_BACKING_SHM;
        vm->backing_shmid = shmid;
        vm->backing_base = attach_addr;
        vm->owner_mm = current_mm;
        vm->page_offset = 0;
        vm->area_kind = proc::VmAreaKind::SysvShm;
        vm->grow_policy = proc::VmGrowPolicy::None;
        vm->guard_pages = 0;
        vm->debug_name = "sysv-shm";
        vm->file_backed_bytes = 0;
        vm->zero_fill_past_file = false;
        return (void *)attach_addr;
    }

    int ShmManager::detach_vma_attachment(int shmid, void *addr, uint tid)
    {
        SpinLockGuard guard(shm_lock_);
        auto it = segments->find(shmid);
        if (it == segments->end())
        {
            return -EINVAL;
        }

        shm_segment &seg = it->second;
        size_t before = seg.attached_addrs.size();
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        pid_t pid = current_proc != nullptr ? current_proc->_pid : -1;
        remove_attachment_record_locked(seg, tid, pid, addr);
        if (seg.attached_addrs.size() == before)
        {
            return -EINVAL;
        }

        if (((seg.mode & SHM_DEST) || seg.auto_destroy_on_last_detach) && seg.nattch == 0)
        {
            return delete_seg_locked(shmid);
        }
        return 0;
    }

    int ShmManager::detach_seg(void *addr)
    {
        if (addr == nullptr)
        {
            return -EINVAL;
        }

        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr)
        {
            return -ESRCH;
        }

        proc::ProcessMemoryManager *current_mm = current_proc->get_memory_manager();
        if (current_mm == nullptr)
        {
            return -EINVAL;
        }
        MemoryLockGuard mm_guard(current_mm);

        size_t real_size = 0;
        {
            SpinLockGuard guard(shm_lock_);
            if (!lookup_attachment_locked(current_proc->get_tid(), current_proc->_pid, addr, nullptr, &real_size))
            {
                printfRed("[ShmManager] Segment with address %p not found\n", addr);
                return -EINVAL;
            }
        }

        return current_mm->unmap_memory_range(addr, real_size);
    }

    bool ShmManager::is_shared_memory_address(void *addr)
    {
        SpinLockGuard guard(shm_lock_);

        if (!addr) {
            return false;
        }

        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        uint cur_tid = current_proc != nullptr ? current_proc->get_tid() : 0;
        pid_t cur_pid = current_proc != nullptr ? current_proc->_pid : -1;
        // 遍历所有共享内存段，仅匹配当前线程的记录
        for (auto it = segments->begin(); it != segments->end(); ++it)
        {
            shm_segment &seg = it->second;
            auto addr_it = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry& e){
                return e.addr == addr && (e.tid == cur_tid || e.pid == cur_pid);
            });
            if (addr_it != seg.attached_addrs.end()) {
                return true;
            }
        }
        return false;
    }

    int ShmManager::find_shared_memory_segment(void *addr, void **start_addr, size_t *size)
    {
        SpinLockGuard guard(shm_lock_);

        if (!addr) {
            return -1;
        }

        uint64 target_addr = (uint64)addr;

        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        uint cur_tid = current_proc != nullptr ? current_proc->get_tid() : 0;
        pid_t cur_pid = current_proc != nullptr ? current_proc->_pid : -1;
        // printf("[ShmManager] Finding shared memory segment for address: %p (tid=%d)\n", addr, cur_tid);
        // 遍历所有共享内存段（只看当前线程）
        for (auto it = segments->begin(); it != segments->end(); ++it)
        {
            shm_segment &seg = it->second;
            
            // 检查每个附加地址及其范围
            for (const auto& e : seg.attached_addrs) {
                if (e.tid != cur_tid && e.pid != cur_pid) continue;
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
                    return seg.shmid;  // 返回真实的共享内存段ID
                }
            }
        }
        
        return -1;
    }

    bool ShmManager::add_reference_for_fork(void *addr)
    {
        SpinLockGuard guard(shm_lock_);

        if (!addr) {
            return false;
        }

        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        uint cur_tid = current_proc != nullptr ? current_proc->get_tid() : 0;
        pid_t cur_pid = current_proc != nullptr ? current_proc->_pid : -1;
        // 遍历所有共享内存段，找到包含该地址的段（限定当前线程）
        for (auto it = segments->begin(); it != segments->end(); ++it)
        {
            shm_segment &seg = it->second;
            
            // 在附加地址列表中查找
            auto addr_it = eastl::find_if(seg.attached_addrs.begin(), seg.attached_addrs.end(), [&](const attached_entry& e){
                return e.addr == addr && (e.tid == cur_tid || e.pid == cur_pid);
            });
            if (addr_it != seg.attached_addrs.end()) {
                // 找到了包含该地址的共享内存段，增加引用计数
                seg.nattch++;
                return true;
            }
        }
        
        return false;
    }

    int ShmManager::shmctl(int shmid, int cmd, struct shmid_ds *buf,uint64 buf_addr)
    {
        SpinLockGuard guard(shm_lock_);

        proc::Pcb* current_proc = proc::k_pm.get_cur_pcb();
        uint64 current_ns_id = current_ipc_namespace_id();

        switch (cmd) {
            case IPC_STAT:
            case SHM_STAT:
            case SHM_STAT_ANY:
            {
                // 查找共享内存段
                auto it = segments->end();
                if (cmd == SHM_STAT || cmd == SHM_STAT_ANY) {
                    // SHM_STAT 系列主要按“内核索引”查询；SHM_STAT_ANY 额外兼容
                    // LTP 在 setup 中直接传 shmid 探测能力的写法。
                    if (shmid >= 0) {
                        int index = 0;
                        for (auto iter = segments->begin(); iter != segments->end(); ++iter) {
                            if (iter->second.ipc_ns_id != current_ns_id)
                            {
                                continue;
                            }
                            if (index == shmid) {
                                it = iter;
                                break;
                            }
                            ++index;
                        }
                        if (it == segments->end() && cmd == SHM_STAT_ANY)
                        {
                            auto by_id = segments->find(shmid);
                            if (by_id != segments->end() && by_id->second.ipc_ns_id == current_ns_id)
                            {
                                it = by_id;
                            }
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
                if (seg.ipc_ns_id != current_ns_id)
                {
                    printfRed("[ShmManager] shmctl cmd=%d shmid=%d is outside current IPC namespace\n",
                              cmd, shmid);
                    return -EINVAL;
                }

                // 权限检查 (SHM_STAT_ANY 不需要权限检查)
                if (cmd != SHM_STAT_ANY) {
                    if (!check_segment_read_permission(seg, current_proc->_euid, current_proc->_egid)) {
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
                if (mem::k_vmm.copy_out(*current_proc->get_pagetable(),
                                        buf_addr,
                                        &kernel_buf,
                                        sizeof(kernel_buf),
                                        current_proc->get_memory_manager()) < 0) {
                    printfRed("[ShmManager] Failed to copy shmid_ds to user space\n");
                    return -EFAULT;
                }

                // SHM_STAT/SHM_STAT_ANY 返回实际的段标识符，IPC_STAT 返回 0。
                return (cmd == SHM_STAT || cmd == SHM_STAT_ANY) ? seg.shmid : 0;
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
                if (seg.ipc_ns_id != current_ns_id)
                {
                    return -EINVAL;
                }

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
                if (mem::k_vmm.copy_in(*current_proc->get_pagetable(),
                                       &user_buf,
                                       buf_addr,
                                       sizeof(user_buf),
                                       current_proc->get_memory_manager()) < 0) {
                    printfRed("[ShmManager] Failed to copy shmid_ds from user space\n");
                    return -EFAULT;
                }

                // 按标准更新可修改的字段
                seg.owner_uid = user_buf.shm_perm.uid;
                seg.owner_gid = user_buf.shm_perm.gid;
                seg.mode = (seg.mode & ~0777) | (user_buf.shm_perm.mode & 0777);  // 只更新低9位权限
                seg.ctime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME);  // 更新修改时间
                seg.last_pid = current_proc->_pid;
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
                if (seg.ipc_ns_id != current_ns_id)
                {
                    return -EINVAL;
                }

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
                int result = delete_seg_locked(shmid);
                if (result != 0) {
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
                sys_info.shmmax = shmmax_limit;
                sys_info.shmmin = 1;             // 最小段大小
                sys_info.shmmni = shmmni_limit;
                sys_info.shmseg = 128;                // 每进程最大段数(未使用)
                sys_info.shmall = (shm_size / PGSIZE); // 系统总页数

                // 复制到用户空间
                if (mem::k_vmm.copy_out(*current_proc->get_pagetable(),
                                        buf_addr,
                                        &sys_info,
                                        sizeof(sys_info),
                                        current_proc->get_memory_manager()) < 0) {
                    printfRed("[ShmManager] Failed to copy shminfo to user space\n");
                    return -EFAULT;
                }

                int visible_count = 0;
                for (const auto &pair : *segments)
                {
                    if (pair.second.ipc_ns_id == current_ns_id)
                    {
                        ++visible_count;
                    }
                }
                return visible_count == 0 ? 0 : visible_count - 1;
            }

            case SHM_INFO:
            {
                if (buf == nullptr) {
                    printfRed("[ShmManager] buf is null for SHM_INFO\n");
                    return -EFAULT;
                }

                // 创建系统资源使用信息
                struct shm_info usage_info = {};
                int visible_count = 0;
                
                size_t total_pages = 0;
                for (const auto& pair : *segments) {
                    if (pair.second.ipc_ns_id != current_ns_id)
                    {
                        continue;
                    }
                    ++visible_count;
                    total_pages += pair.second.real_size / PGSIZE;  // 使用实际分配大小
                }
                usage_info.used_ids = visible_count;
                
                usage_info.shm_tot = total_pages;
                usage_info.shm_rss = total_pages;  // 简化：假设都在内存中
                usage_info.shm_swp = 0;            // 简化：没有交换
                usage_info.swap_attempts = 0;      // 未使用
                usage_info.swap_successes = 0;     // 未使用

                // 复制到用户空间
                if (mem::k_vmm.copy_out(*current_proc->get_pagetable(),
                                        buf_addr,
                                        &usage_info,
                                        sizeof(usage_info),
                                        current_proc->get_memory_manager()) < 0) {
                    printfRed("[ShmManager] Failed to copy shm_info to user space\n");
                    return -EFAULT;
                }

                return visible_count == 0 ? 0 : visible_count - 1;
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
                if (seg.ipc_ns_id != current_ns_id)
                {
                    return -EINVAL;
                }

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
                } else {
                    seg.mode &= ~SHM_LOCKED;
                }
                
                return 0;
            }

            default:
                printfRed("[ShmManager] Unknown shmctl command: %d\n", cmd);
                return -EINVAL;
        }

        return 0;
    }

    size_t ShmManager::get_shmmax_limit() const
    {
        SpinLockGuard guard(shm_lock_);
        return shmmax_limit;
    }

    int ShmManager::set_shmmax_limit(size_t value)
    {
        SpinLockGuard guard(shm_lock_);
        if (value == 0 || value > shm_size)
        {
            return -EINVAL;
        }
        shmmax_limit = value;
        return 0;
    }

    int ShmManager::get_shmmni_limit() const
    {
        SpinLockGuard guard(shm_lock_);
        return shmmni_limit;
    }

    uint64 ShmManager::get_shmall_pages() const
    {
        SpinLockGuard guard(shm_lock_);
        return shm_size / PGSIZE;
    }

    eastl::string ShmManager::format_proc_sysvipc_shm() const
    {
        SpinLockGuard guard(shm_lock_);
        eastl::string result;
        result += "       key      shmid perms                  size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime        rss       swap\n";
        uint64 current_ns_id = current_ipc_namespace_id();

        for (const auto &pair : *segments)
        {
            const shm_segment &seg = pair.second;
            if (seg.ipc_ns_id != current_ns_id)
            {
                continue;
            }
            char line[256];
            // LTP 使用 fscanf("%i") 读取 /proc/sysvipc/shm；这里不能带前导 0，
            // 否则会被按八进制解析，4096 这类值会读歪。
            snprintf(line, sizeof(line),
                     "%d %d %o %lu %d %d %d %u %u %u %u %lu %lu %lu %lu %d\n",
                     static_cast<int>(seg.key),
                     seg.shmid,
                     static_cast<unsigned int>(seg.mode & 0777),
                     static_cast<unsigned long>(seg.size),
                     static_cast<int>(seg.creator_pid),
                     static_cast<int>(seg.last_pid),
                     seg.nattch,
                     static_cast<unsigned int>(seg.owner_uid),
                     static_cast<unsigned int>(seg.owner_gid),
                     static_cast<unsigned int>(seg.creator_uid),
                     static_cast<unsigned int>(seg.creator_gid),
                     static_cast<unsigned long>(seg.atime),
                     static_cast<unsigned long>(seg.dtime),
                     static_cast<unsigned long>(seg.ctime),
                     static_cast<unsigned long>(seg.real_size),
                     0);
            result += line;
        }
        return result;
    }

    shm_segment ShmManager::get_seg_info(int shmid)
    {
        SpinLockGuard guard(shm_lock_);

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
        SpinLockGuard guard(shm_lock_);

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
        SpinLockGuard guard(shm_lock_);

        printfYellow("[ShmManager] Memory Status:\n");
        printfYellow("  Total memory: 0x%x bytes\n", shm_size);
        printfYellow("  Active segments: %u\n", segments->size());
        size_t total_used = 0;
        for (const auto &pair : *segments)
        {
            total_used += pair.second.real_size;
        }
        size_t total_free = shm_size > total_used ? (shm_size - total_used) : 0;
        printfYellow("  Total free memory: 0x%x bytes\n", total_free);
        printfYellow("  Memory utilization: %.1f%%\n",
                     shm_size == 0 ? 0.0 : (double)total_used * 100.0 / shm_size);
    }

    size_t ShmManager::get_total_free_memory() const
    {
        SpinLockGuard guard(shm_lock_);

        size_t total_used = 0;
        for (const auto &pair : *segments)
        {
            total_used += pair.second.real_size;
        }
        return shm_size > total_used ? (shm_size - total_used) : 0;
    }

    size_t ShmManager::get_largest_free_block() const
    {
        return get_total_free_memory();
    }
}

namespace shm {
    bool ShmManager::duplicate_attachments_for_fork(uint parent_tid, uint child_tid, pid_t child_pid)
    {
        SpinLockGuard guard(shm_lock_);

        proc::Pcb *parent = proc::k_pm.get_cur_pcb();
        pid_t parent_pid = parent != nullptr ? parent->_pid : -1;
        bool duplicated = false;
        for (auto &pair : *segments) {
            shm_segment &seg = pair.second;
            eastl::vector<void *> copied_addrs;
            for (const auto &e : seg.attached_addrs) {
                if (e.tid == parent_tid || e.pid == parent_pid) {
                    copied_addrs.push_back(e.addr);
                }
            }
            for (void *addr : copied_addrs)
            {
                seg.attached_addrs.push_back(attached_entry{child_tid, child_pid, addr});
                seg.nattch++;
                duplicated = true;
            }
        }
        return duplicated;
    }

    int ShmManager::detach_all_for_process(proc::Pcb *proc, bool unmap_pages, bool match_tid_only)
    {
        SpinLockGuard guard(shm_lock_);

        if (proc == nullptr)
        {
            return 0;
        }

        const uint target_tid = proc->get_tid();
        const pid_t target_pid = proc->_pid;
        int detached_count = 0;
        eastl::vector<int> pending_delete;

        for (auto &pair : *segments)
        {
            shm_segment &seg = pair.second;
            for (auto it = seg.attached_addrs.begin(); it != seg.attached_addrs.end();)
            {
                bool entry_belongs_to_proc = it->tid == target_tid || it->pid == target_pid;
                if (!entry_belongs_to_proc)
                {
                    ++it;
                    continue;
                }

                bool vma_backed_attachment = is_vma_backed_shared_attachment(proc, (uint64)it->addr);

                // VMA 托管的附件会在 free_all_vma()/munmap()/shmdt() 里统一收口。
                // 这里兜底清理的是“元数据还留着，但地址空间里已经没有对应 VMA”的残留记录。
                if (vma_backed_attachment)
                {
                    ++it;
                    continue;
                }

                if (it->tid != target_tid && match_tid_only)
                {
                    ++it;
                    continue;
                }

                if (unmap_pages && proc->get_pagetable() != nullptr)
                {
                    mem::Pte pte = proc->get_pagetable()->walk((uint64)it->addr, 0);
                    if (!pte.is_null() && pte.is_valid())
                    {
                        mem::k_vmm.vmunmap(*proc->get_pagetable(), (uint64)it->addr, seg.real_size / PGSIZE, 0);
                    }
                }

                it = seg.attached_addrs.erase(it);
                seg.dtime = tmm::k_tm.clock_gettime_sec(tmm::CLOCK_REALTIME);
                seg.last_pid = proc->_pid;
                if (seg.nattch > 0)
                {
                    seg.nattch--;
                }
                detached_count++;
            }

            if (((seg.mode & SHM_DEST) || seg.auto_destroy_on_last_detach) && seg.nattch == 0)
            {
                pending_delete.push_back(seg.shmid);
            }
        }

        for (int shmid : pending_delete)
        {
            delete_seg_locked(shmid);
        }

        return detached_count;
    }
}
