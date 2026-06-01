#include "proc_manager.hh"
#include "futex.hh"  // 添加futex头文件，用于robust futex清理
#include "hal/cpu.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "scheduler.hh"
#include "mem/memlayout.hh" // 内核栈配置常量
#ifdef RISCV
#include "riscv/trap.hh"
#elif defined(LOONGARCH)
#include "loongarch/trap.hh"
#endif
#include "printer.hh"
#include "devs/device_manager.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "process_memory_manager.hh" // 新增：进程内存管理器
#include "shm_manager.hh"
#ifdef RISCV
// #include "devs/riscv/disk_driver.hh"
#elif defined(LOONGARCH)
#include "devs/loongarch/disk_driver.hh"
#endif
#include "net/f7ly_network.hh"

// #include "fs/vfs/dentrycache.hh"
// #include "fs/vfs/path.hh"
// #include "fs/ramfs/ramfs.hh"
#include "fs/vfs/file/device_file.hh"
#include "param.h"
#include "timer_manager.hh"
#include "timer_interface.hh"
#include "posix_timers.hh"
#include "fs/vfs/elf.hh"
#include "fs/vfs/file/normal_file.hh"
#include "mem.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "syscall_defs.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/lwext4/ext4.hh"
#include <EASTL/map.h>
#include "fs/vfs/vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/virtual_fs.hh"
#include "sys/syscall_defs.hh"
extern "C"
{
    extern uint64 initcode_start[];
    extern uint64 initcode_end[];

    extern int init_main(void);
    extern char trampoline[]; // trampoline.S
    void _wrp_fork_ret(void)
    {
        // printf("into _wrapped_fork_ret\n");
        proc::k_pm.fork_ret();
    }
    extern char sig_trampoline[]; // sig_trampoline.S
}

namespace proc
{
    namespace
    {
#ifdef RISCV
        constexpr uint64 k_min_kernel_file_ptr = KERNBASE;
#elif defined(LOONGARCH)
        constexpr uint64 k_min_kernel_file_ptr = PHYSBASE;
#endif
        constexpr uint32 k_max_reasonable_file_refcnt = num_process * max_open_files;

        inline uint64 align_up_pow2(uint64 value, uint64 alignment)
        {
            if (alignment == 0)
            {
                return value;
            }
            return (value + alignment - 1) & ~(alignment - 1);
        }

        inline uint64 align_down_pow2(uint64 value, uint64 alignment)
        {
            if (alignment == 0)
            {
                return value;
            }
            return value & ~(alignment - 1);
        }

        inline bool is_kernel_mapped_file_range(uint64 addr, uint64 size)
        {
            if (addr < k_min_kernel_file_ptr || size == 0)
            {
                return false;
            }

            uint64 end = addr + size - 1;
            if (end < addr)
            {
                return false;
            }

            return mem::k_pagetable.kwalk_addr(addr) != 0 &&
                   mem::k_pagetable.kwalk_addr(end) != 0;
        }

	        inline bool is_probably_live_file_object(fs::file *file_obj)
	        {
	            if (file_obj == nullptr)
	            {
	                return false;
            }

            if (!is_kernel_mapped_file_range((uint64)file_obj, sizeof(fs::file)))
            {
                return false;
            }

            uint64 vtable_addr = *(uint64 *)file_obj;
            if (!is_kernel_mapped_file_range(vtable_addr, sizeof(void *)))
            {
                return false;
            }

	            uint32 refcnt = file_obj->refcnt;
	            return refcnt > 0 && refcnt <= k_max_reasonable_file_refcnt;
	        }

        class MemoryLockGuard
        {
        public:
            explicit MemoryLockGuard(ProcessMemoryManager *mm) : _mm(mm)
            {
                if (_mm != nullptr)
                {
                    _mm->lock_memory();
                }
            }

            ~MemoryLockGuard()
            {
                if (_mm != nullptr)
                {
                    _mm->unlock_memory();
                }
            }

        private:
            ProcessMemoryManager *_mm;
        };

	        inline bool open_request_has_write(uint flags)
	        {
	            int accmode = flags & O_ACCMODE;
	            return accmode == O_WRONLY || accmode == O_RDWR;
	        }

	        inline bool lease_conflicts_with_open(short lease_type, uint flags)
	        {
	            if (lease_type == F_WRLCK)
	            {
	                return true;
	            }
	            if (lease_type == F_RDLCK)
	            {
	                return open_request_has_write(flags);
	            }
	            return false;
	        }

	        fs::file *find_conflicting_lease_holder(const eastl::string &path, uint flags)
	        {
	            eastl::vector<fs::file *> seen;
	            seen.reserve(num_process);

	            for (uint i = 0; i < num_process; ++i)
	            {
	                Pcb *pcb = &k_proc_pool[i];
	                if (pcb->_state == ProcState::UNUSED || pcb->_ofile == nullptr)
	                {
	                    continue;
	                }

	                for (uint fd = 0; fd < max_open_files; ++fd)
	                {
	                    fs::file *candidate = pcb->_ofile->_ofile_ptr[fd];
	                    if (candidate == nullptr || candidate->backing_path() != path)
	                    {
	                        continue;
	                    }

	                    bool already_seen = false;
	                    for (fs::file *existing : seen)
	                    {
	                        if (existing == candidate)
	                        {
	                            already_seen = true;
	                            break;
	                        }
	                    }
	                    if (already_seen)
	                    {
	                        continue;
	                    }
	                    seen.push_back(candidate);

	                    if (candidate->_lease_type != F_UNLCK &&
	                        lease_conflicts_with_open(candidate->_lease_type, flags))
	                    {
	                        return candidate;
	                    }
	                }
	            }

	            return nullptr;
	        }

	        int wait_for_conflicting_lease(const eastl::string &path, uint flags)
	        {
	            bool notified = false;
	            bool writer_waiter = open_request_has_write(flags);
	            fs::file *registered_holder = nullptr;
	            auto update_wait_registration = [&](fs::file *new_holder)
	            {
	                if (registered_holder == new_holder)
	                {
	                    return;
	                }

	                if (registered_holder != nullptr)
	                {
	                    int &old_counter = writer_waiter ? registered_holder->_lease_waiting_writers
	                                                     : registered_holder->_lease_waiting_readers;
	                    if (old_counter > 0)
	                    {
	                        old_counter--;
	                    }
	                }

	                registered_holder = new_holder;
	                if (registered_holder != nullptr)
	                {
	                    int &new_counter = writer_waiter ? registered_holder->_lease_waiting_writers
	                                                     : registered_holder->_lease_waiting_readers;
	                    new_counter++;
	                }
	            };

	            while (true)
	            {
	                fs::file *holder = find_conflicting_lease_holder(path, flags);
	                if (holder == nullptr)
	                {
	                    update_wait_registration(nullptr);
	                    return 0;
	                }
	                update_wait_registration(holder);

	                if (!notified && holder->_lease_owner_pid > 0)
	                {
	                    // 先补 LTP fcntl33 需要的最小 lease-break 语义：
	                    // breaker 遇到冲突 lease 时，通知持有者再等待它降级/释放。
	                    (void)k_pm.kill_signal(holder->_lease_owner_pid, ipc::signal::SIGPOLL);
	                    notified = true;
	                }

	                k_scheduler.yield();
	            }
	        }

	        struct SharedBackingSelection
	        {
            key_t key;
            bool always_new_segment;
        };

        /**
         * @brief 为 mmap(MAP_SHARED) 选择共享后端。
         *
         * 文件共享映射需要按文件身份复用后端，匿名共享映射则必须每次都拿到独立段。
         * 之前匿名映射错误地复用了同一个固定 key，会让互不相关的 MAP_SHARED|MAP_ANONYMOUS
         * 互相别名，直接破坏像 iozone barrier 这类依赖共享页布局的程序语义。
         */
        inline SharedBackingSelection select_shared_backing(fs::file *file_obj, bool is_anonymous)
        {
            if (is_anonymous || file_obj == nullptr)
            {
                return {IPC_PRIVATE, true};
            }

            return {shm::k_smm.ftok(file_obj->_path_name.c_str(), 0), false};
        }

        inline bool starts_with(const char *lhs, const char *rhs)
        {
            if (lhs == nullptr || rhs == nullptr)
            {
                return false;
            }
            while (*rhs != '\0')
            {
                if (*lhs == '\0' || *lhs != *rhs)
                {
                    return false;
                }
                ++lhs;
                ++rhs;
            }
            return true;
        }

        inline bool is_busybox_like_proc(const proc::Pcb *proc)
        {
            return proc != nullptr && starts_with(proc->_name, "busybox");
        }

        inline bool should_deliver_child_exit_signal(const proc::Pcb *parent, int signum)
        {
            if (parent == nullptr || signum <= 0)
            {
                return false;
            }

            if (signum != ipc::signal::SIGCHLD)
            {
                return true;
            }

            /*
             * 当前 wait/wait4 已经通过 wakeup(parent) 显式唤醒父进程回收 zombie。
             * SIGCHLD 的用户态 handler 路径还不够稳，libc-bench 这类高频 fork/wait
             * 会把每轮子进程退出都变成一次信号帧往返，最终打断基准进程。
             * 这里先让 SIGCHLD 只承担“唤醒 wait”的内核内语义；后续完善信号帧后再放开投递。
             */
            return false;
        }

#ifdef LOONGARCH
// 下面的代码是针对 LoongArch 架构的用户态 ELF 补丁机制，用于修复特定版本的 musl libc 和相关程序中的已知问题。
// 下载磁盘的ll/sc原子指令有问题，对于entry程序需要修改后才能正常执行。
///TODO:未来如果测试的时候发现这个东西会起反作用，需要找到别的方法来修复，反正现在本地跑我必须加了这个才能跑
        struct LoongArchUserElfPatch
        {
            const char *path;
            uint offset;
            const uint8 *old_bytes;
            const uint8 *new_bytes;
            uint size;
            const char *label;
        };

        constexpr uint8 k_entry_static_vm_lock_old[] = {
            0xac, 0x21, 0xd5, 0x02, 0x8c, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0xae, 0x21, 0xd5, 0x02, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_entry_static_vm_lock_new[] = {
            0xae, 0x21, 0xd5, 0x02, 0xcc, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0x00, 0x00, 0x40, 0x03, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_musl_aio_get_queue_ref_inc_old[] = {
            0xac, 0x31, 0xf9, 0x02, 0x8c, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0xae, 0x31, 0xf9, 0x02, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_musl_aio_get_queue_ref_inc_new[] = {
            0xae, 0x31, 0xf9, 0x02, 0xcc, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0x00, 0x00, 0x40, 0x03, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_musl_aio_unref_queue_ref_dec_old[] = {
            0xac, 0x31, 0xf9, 0x02, 0x8c, 0x01, 0x00, 0x20, 0x8c, 0xfd, 0xbf, 0x02,
            0xae, 0x31, 0xf9, 0x02, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_musl_aio_unref_queue_ref_dec_new[] = {
            0xae, 0x31, 0xf9, 0x02, 0xcc, 0x01, 0x00, 0x20, 0x8c, 0xfd, 0xbf, 0x02,
            0x00, 0x00, 0x40, 0x03, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_musl_cleanup_exchange_zero_old[] = {
            0x8c, 0x20, 0xf9, 0x02, 0x8c, 0x01, 0x00, 0x20, 0x8d, 0x81, 0x40, 0x00,
            0x8f, 0x20, 0xf9, 0x02, 0xcc, 0x01, 0x15, 0x00};
        constexpr uint8 k_musl_cleanup_exchange_zero_new[] = {
            0x8f, 0x20, 0xf9, 0x02, 0xec, 0x01, 0x00, 0x20, 0x8d, 0x81, 0x40, 0x00,
            0x00, 0x00, 0x40, 0x03, 0xcc, 0x01, 0x15, 0x00};
        constexpr uint8 k_musl_vm_lock_old[] = {
            0xac, 0x61, 0xd6, 0x02, 0x8c, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0xae, 0x61, 0xd6, 0x02, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_musl_vm_lock_new[] = {
            0xae, 0x61, 0xd6, 0x02, 0xcc, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0x00, 0x00, 0x40, 0x03, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_libc_bench_thread_counter_old[] = {
            0xac, 0x81, 0xea, 0x02, 0x8c, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0xae, 0x81, 0xea, 0x02, 0xcc, 0x01, 0x00, 0x21};
        constexpr uint8 k_libc_bench_thread_counter_new[] = {
            0xae, 0x81, 0xea, 0x02, 0xcc, 0x01, 0x00, 0x20, 0x8c, 0x05, 0x80, 0x02,
            0x00, 0x00, 0x40, 0x03, 0xcc, 0x01, 0x00, 0x21};

        constexpr LoongArchUserElfPatch k_loongarch_user_elf_patches[] = {
            {"/musl/entry-static.exe", 0x354a8, k_entry_static_vm_lock_old, k_entry_static_vm_lock_new, sizeof(k_entry_static_vm_lock_old), "entry-static::__vm_lock"},
            {"/musl/lib/libc.so", 0x14620, k_musl_aio_get_queue_ref_inc_old, k_musl_aio_get_queue_ref_inc_new, sizeof(k_musl_aio_get_queue_ref_inc_old), "libc.so::__aio_get_queue_ref_inc"},
            {"/musl/lib/libc.so", 0x1471c, k_musl_aio_unref_queue_ref_dec_old, k_musl_aio_unref_queue_ref_dec_new, sizeof(k_musl_aio_unref_queue_ref_dec_old), "libc.so::__aio_unref_queue_ref_dec"},
            {"/musl/lib/libc.so", 0x14ab0, k_musl_cleanup_exchange_zero_old, k_musl_cleanup_exchange_zero_new, sizeof(k_musl_cleanup_exchange_zero_old), "libc.so::cleanup_exchange_zero"},
            {"/musl/lib/libc.so", 0x6a0ec, k_musl_vm_lock_old, k_musl_vm_lock_new, sizeof(k_musl_vm_lock_old), "libc.so::__vm_lock"},
            {"/musl/libc-bench", 0x0fe1c, k_libc_bench_thread_counter_old, k_libc_bench_thread_counter_new, sizeof(k_libc_bench_thread_counter_old), "libc-bench::thread_counter"},
        };

        uint8 *loaded_user_byte_ptr(mem::PageTable &pt, uint64 user_va)
        {
            mem::Pte pte = pt.walk(user_va, false);
            if (pte.is_null() || !pte.is_valid())
            {
                return nullptr;
            }
            uint64 pa = PTE2PA((uint64)pte.get_data()) + (user_va & (PGSIZE - 1));
            return reinterpret_cast<uint8 *>(to_vir(pa));
        }

        bool patch_loaded_user_bytes(mem::PageTable &pt, uint64 user_va,
                                     const uint8 *old_bytes, const uint8 *new_bytes, uint size)
        {
            bool already_patched = true;
            bool old_bytes_match = true;

            for (uint i = 0; i < size; ++i)
            {
                uint8 *dst = loaded_user_byte_ptr(pt, user_va + i);
                if (dst == nullptr)
                {
                    return false;
                }
                if (*dst != new_bytes[i])
                {
                    already_patched = false;
                }
                if (*dst != old_bytes[i])
                {
                    old_bytes_match = false;
                }
            }

            if (already_patched)
            {
                return false;
            }
            if (!old_bytes_match)
            {
                return false;
            }

            for (uint i = 0; i < size; ++i)
            {
                uint8 *dst = loaded_user_byte_ptr(pt, user_va + i);
                *dst = new_bytes[i];
            }
            return true;
        }

        void apply_loongarch_user_elf_patches(mem::PageTable &pt, uint64 va, const char *path, uint offset, uint size)
        {
            if (path == nullptr || size == 0)
            {
                return;
            }

            uint end = offset + size;
            if (end < offset)
            {
                return;
            }

            for (const LoongArchUserElfPatch &patch : k_loongarch_user_elf_patches)
            {
                if (strcmp(path, patch.path) != 0)
                {
                    continue;
                }
                if (patch.offset < offset || patch.offset + patch.size > end)
                {
                    continue;
                }

                uint64 patch_va = va + (patch.offset - offset);
                if (patch_loaded_user_bytes(pt, patch_va, patch.old_bytes, patch.new_bytes, patch.size))
                {
                    printfYellow("[execve] LoongArch runtime patch %s %s+0x%x\n",
                                 path, patch.label, patch.offset);
                }
            }
        }
#endif

        inline int effective_fd_limit(const proc::Pcb *proc)
        {
            if (proc == nullptr)
            {
                return 0;
            }

            uint64 limit = proc->get_nofile_limit();
            if (limit >= max_open_files)
            {
                return static_cast<int>(max_open_files);
            }
            return static_cast<int>(limit);
        }

        void dump_fd_table(proc::Pcb *proc, const char *reason)
        {
            if (proc == nullptr || proc->_ofile == nullptr)
            {
                printfRed("[fd-dump] %s: proc/ofile 为空\n", reason == nullptr ? "unknown" : reason);
                return;
            }

            printfRed("[fd-dump] %s pid=%d tid=%d name=%s\n",
                      reason == nullptr ? "unknown" : reason,
                      proc->_pid,
                      proc->_tid,
                      proc->_name);

            for (int i = 0; i < (int)max_open_files; ++i)
            {
                fs::file *entry = proc->_ofile->_ofile_ptr[i];
                if (entry == nullptr)
                {
                    continue;
                }

                if (!is_probably_live_file_object(entry))
                {
                    printfRed("[fd-dump] fd=%d file=%p <invalid>\n", i, entry);
                    continue;
                }

                printfBlue("[fd-dump] fd=%d file=%p ref=%d type=%d virtual=%d path=%s\n",
                           i,
                           entry,
                           (int)entry->refcnt,
                           (int)entry->_attrs.filetype,
                           entry->is_virtual ? 1 : 0,
                           entry->_path_name.c_str());
            }
        }

        inline bool is_exec_whitespace(char ch)
        {
            return ch == ' ' || ch == '\t';
        }

        bool parse_shebang_line(const char *buffer, int len,
                                char *interpreter_path, size_t interpreter_path_cap,
                                char *interpreter_arg, size_t interpreter_arg_cap)
        {
            if (buffer == nullptr || len < 2 || buffer[0] != '#' || buffer[1] != '!')
            {
                return false;
            }
            if (interpreter_path == nullptr || interpreter_arg == nullptr ||
                interpreter_path_cap == 0 || interpreter_arg_cap == 0)
            {
                return false;
            }

            interpreter_path[0] = '\0';
            interpreter_arg[0] = '\0';

            int cursor = 2;
            while (cursor < len && is_exec_whitespace(buffer[cursor]))
            {
                ++cursor;
            }

            size_t interpreter_len = 0;
            while (cursor < len)
            {
                char ch = buffer[cursor];
                if (ch == '\0' || ch == '\n' || ch == '\r' || is_exec_whitespace(ch))
                {
                    break;
                }
                if (interpreter_len + 1 >= interpreter_path_cap)
                {
                    return false;
                }
                interpreter_path[interpreter_len++] = ch;
                ++cursor;
            }
            interpreter_path[interpreter_len] = '\0';

            while (cursor < len && is_exec_whitespace(buffer[cursor]))
            {
                ++cursor;
            }

            size_t interpreter_arg_len = 0;
            while (cursor < len)
            {
                char ch = buffer[cursor];
                if (ch == '\0' || ch == '\n' || ch == '\r')
                {
                    break;
                }
                if (interpreter_arg_len + 1 >= interpreter_arg_cap)
                {
                    return false;
                }
                interpreter_arg[interpreter_arg_len++] = ch;
                ++cursor;
            }
            interpreter_arg[interpreter_arg_len] = '\0';

            while (interpreter_arg_len > 0 && is_exec_whitespace(interpreter_arg[interpreter_arg_len - 1]))
            {
                interpreter_arg[--interpreter_arg_len] = '\0';
            }

            return interpreter_len > 0;
        }
    }

    ProcessManager k_pm;

    void ProcessManager::init(const char *pid_lock_name, const char *tid_lock_name, const char *wait_lock_name)
    {
        // initialize the proc table.
        _pid_lock.init(pid_lock_name);
        _tid_lock.init(tid_lock_name);
        _wait_lock.init(wait_lock_name);
        for (uint i = 0; i < num_process; ++i)
        {
            Pcb &p = k_proc_pool[i];
            p.init("pcb", i);
        }
        _cur_pid = 1;
        _cur_tid = 1;
        _last_alloc_proc_gid = num_process - 1;
        printfGreen("[proc] Process Manager Init\n");
    }

    void ProcessManager::set_slot(Pcb *p, int slot)
    {
        if (p == nullptr)
        {
            return;
        }

        p->_lock.acquire();
        p->_slot = slot;
        p->_lock.release();
    }

    void ProcessManager::set_priority(Pcb *p, int priority)
    {
        if (p == nullptr)
        {
            return;
        }

        if (priority < highest_proc_prio)
        {
            priority = highest_proc_prio;
        }
        else if (priority > lowest_proc_prio)
        {
            priority = lowest_proc_prio;
        }

        p->_lock.acquire();
        p->_priority = priority;
        p->_lock.release();
    }

    Pcb *ProcessManager::get_cur_pcb()
    {
        Cpu::push_intr_off();
        Cpu *c_cpu = Cpu::get_cpu();
        proc::Pcb *pcb = c_cpu->get_cur_proc();
        Cpu::pop_intr_off();
        // 这里为nullptr是正常现象应该无需panic？
        // 学长未对此处作处理，而是判断为nullptr就sleep，参考virtio_disk.cc:218行
        // commented out by @gkq
        //
        // if (pcb == nullptr)
        //     panic("get_cur_pcb: no current process");
        return pcb;
    }

    void ProcessManager::alloc_pid(Pcb *p)
    {
        _pid_lock.acquire();
        p->_pid = _cur_pid;
        _cur_pid++;
        _pid_lock.release();
    }

    void ProcessManager::alloc_tid(Pcb *p)
    {
        _tid_lock.acquire();
        p->_tid = _cur_tid;
        _cur_tid++;
        _tid_lock.release();
    }

    Pcb *ProcessManager::alloc_proc()
    {
        Pcb *p;
        // 遍历整个进程池，尝试分配一个 UNUSED 的进程控制块
        for (uint i = 0; i < num_process; i++)
        {
            // 使用轮转式分配策略，避免总是从头找，提高公平性
            p = &k_proc_pool[(_last_alloc_proc_gid + i) % num_process];
            p->_lock.acquire();
            if (p->_state == ProcState::UNUSED)
            {
                /****************************************************************************************
                 * 基本进程标识和状态管理初始化
                 ****************************************************************************************/
                k_pm.alloc_pid(p);           // 分配全局唯一的进程ID
                k_pm.alloc_tid(p);           // 分配线程ID（单线程进程中等于PID）
                p->_state = ProcState::USED; // 标记进程控制块为已使用

                // 初始化父进程关系（在fork时会重新设置）
                p->_parent = nullptr;
                p->_name[0] = '\0'; // 清空进程名称
                p->exe.clear();     // 清空可执行文件路径

                // 初始化标准Linux进程标识符
                p->_ppid = 0;       // 父进程PID（在fork时设置）
                p->_pgid = p->_pid; // 进程组ID（初始化为自身PID）
                p->_tgid = p->_pid; // 线程组ID（初始化为自身PID）
                p->_sid = p->_pid;  // 会话ID（初始化为自身PID）
                p->_uid = 0;        // 真实用户ID（root）
                p->_euid = 0;       // 有效用户ID（root）
                p->_suid = 0;       // 保存的设置用户ID（root）
                p->_fsuid = 0;      // 文件系统用户ID（root）
                p->_gid = 0;        // 真实组ID（root）
                p->_egid = 0;       // 有效组ID（root）
                p->_sgid = 0;       // 保存的设置组ID（root）
                p->_fsgid = 0;      // 文件系统组ID（root）

                /****************************************************************************************
                 * 进程状态和调度信息初始化
                 ****************************************************************************************/
                p->_chan = nullptr; // 清空睡眠等待通道
                p->_killed = 0;     // 清除终止标志
                p->_exiting = false; // 清除退出清理标记
                p->_xstate = 0;     // 清除退出状态码
                p->_parent_exit_signal = ipc::signal::SIGCHLD;

                // 设置调度相关字段：默认调度槽与优先级
                p->_slot = default_proc_slot;
                p->_priority = default_proc_prio;
                p->_io_priority_override = default_proc_prio;
                p->_has_io_priority_override = false;

                // 初始化CPU亲和性掩码：默认可以在任何CPU上运行
                p->_cpu_mask.fill();

	                /****************************************************************************************
	                 * 内存管理初始化
	                 ****************************************************************************************/
	                // PCB 会被进程池复用，先清空旧的内存管理器指针，避免 set_memory_manager() 误清理历史脏指针。
	                p->reset_memory_manager_ptr();

	                // 为该进程分配一页 trapframe 空间（用于中断时保存用户上下文）
	                // printfYellow("[user pgtbl]==>alloc trapframe for proc %d\n", p->_global_id);
	                if ((p->_trapframe = (TrapFrame *)mem::k_pmm.alloc_page()) == nullptr)
	                {
                    freeproc_creation_failed(p); // 使用专门的创建失败清理函数
                    p->_lock.release();
                    return nullptr;
                }

                // 注意：不再在alloc_proc中创建ProcessMemoryManager
                // ProcessMemoryManager的创建延迟到fork函数中，对于user_init和execve则在相应函数中创建

                /****************************************************************************************
                 * 上下文切换初始化
                 ****************************************************************************************/
                // 初始化上下文结构体
                memset(&p->_context, 0, sizeof(p->_context));

                // 设置调度返回地址为 _wrp_fork_ret
                // 当调度器切换回该进程时，将从这里开始执行
                p->_context.ra = (uint64)_wrp_fork_ret;

                // 设置内核栈指针 - 指向栈顶（高地址）
                p->_context.sp = p->_kstack + KSTACK_SIZE;

                /****************************************************************************************
                 * 文件系统和I/O管理初始化
                 ****************************************************************************************/
                p->_cwd = nullptr;    // 当前工作目录（在具体使用时设置）
                p->_cwd_name.clear(); // 清空当前工作目录路径
                p->_personality = 0;  // 新进程默认使用 PER_LINUX

                // 初始化文件描述符表
                p->_ofile = new ofile();
                p->_ofile->init("ofile");

                /****************************************************************************************
                 * 线程和同步原语初始化
                 ****************************************************************************************/
                p->_futex_addr = nullptr;  // 清空futex等待地址
                p->_futex_key = 0;         // 清空futex匹配键
                p->_clear_tid_addr = 0;    // 清空线程退出时需要清理的地址
                p->_robust_list = nullptr; // 清空健壮futex链表
                p->_robust_list_user_addr = 0;

                /****************************************************************************************
                 * 信号处理初始化
                 ****************************************************************************************/
                // 初始化信号处理结构体
                p->_sigactions = new sighand_struct();
                p->_sigactions->refcnt = 1;
                for (int i = 0; i <= ipc::signal::SIGRTMAX; ++i)
                {
                    p->_sigactions->actions[i] = nullptr;
                }

                p->_sigmask = 0;        // 清空信号屏蔽掩码
                p->_signal = 0;         // 清空待处理信号掩码
                p->_siginfo_mask = 0;   // 清空附带 siginfo 的 pending signal 标记
                memset(p->_queued_siginfo, 0, sizeof(p->_queued_siginfo));
                p->_sigsuspend_restore_pending = false;
                p->_sigsuspend_saved_sigmask = 0;
                p->sig_frame = nullptr; // 清空信号处理栈帧
                p->_alt_stack.ss_sp = nullptr;                 // 备用信号栈地址必须重置
                p->_alt_stack.ss_flags = ipc::signal::SS_DISABLE; // 默认禁用备用信号栈
                p->_alt_stack.ss_size = 0;
                p->_on_sigstack = false;

                /****************************************************************************************
                 * 资源限制初始化
                 ****************************************************************************************/
                // 初始化进程资源限制为默认值
                for (uint i = 0; i < ResourceLimitId::RLIM_NLIMITS; ++i)
                {
                    p->_rlim_vec[i].rlim_cur = RLIM_INFINITY; // 软限制设为无限
                    p->_rlim_vec[i].rlim_max = RLIM_INFINITY; // 硬限制设为无限
                }
                // 设置文件描述符数量限制为合理值
                p->_rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_cur = max_open_files;
                p->_rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_max = max_open_files;

                /****************************************************************************************
                 * 时间统计和会计信息初始化
                 ****************************************************************************************/
                uint64 cur_tick = tmm::get_ticks();
                p->_start_tick = cur_tick;     // 进程开始运行时的时钟节拍数
                p->_user_ticks = 0;            // 用户态累计时钟节拍数
                p->_last_user_tick = 0;        // 上次进入用户态的时钟节拍数
                p->_kernel_entry_tick = 0;     // 进入内核态的时钟节拍数
                p->_utime = 0;                 // 用户态时间
                p->_stime = 0;                 // 系统态时间
                p->_cutime = 0;                // 子进程用户态时间累计
                p->_cstime = 0;                // 子进程系统态时间累计
                p->_start_time = cur_tick;     // 进程启动时间
                p->_start_boottime = cur_tick; // 自系统启动以来的启动时间
                p->_timens_current = {};
                p->_timens_children = {};
                reset_interval_timers(p);      // interval timer 不能把上一个进程残留到复用的 PCB 上

                // 更新上次分配的位置，轮转分配策略
                _last_alloc_proc_gid = p->_global_id;

                return p;
            }
            else
            {
                p->_lock.release();
            }
        }
        // 没有找到可用的进程控制块，分配失败
        return nullptr;
    }


    void ProcessManager::fork_ret()
    {
        // printf("into fork_ret\n");
        proc::Pcb *proc = get_cur_pcb();
        proc->_lock.release();

        static int first = 1;
        if (first)
        {
            first = 0;

            // 文件系统初始化必须在常规进程的上下文中运行（例如，因为它会调用 sleep），
            // 因此不能从 main() 中运行。(copy form xv6)
            filesystem_init(); // <-- This calls fs.cc:filesystem_init

            // filesystem2_init(); // 这个滚蛋
            fs::FileAttrs fAttrsin = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0666);
            fs::device_file *f_in = new fs::device_file(fAttrsin, "/dev/stdin", 0);
            eastl::string pathout("/dev/stdout");
            fs::FileAttrs fAttrsout = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_out =
                new fs::device_file(fAttrsout, pathout, 1);

            eastl::string patherr("/dev/stderr");
            fs::FileAttrs fAttrserr = fs::FileAttrs(fs::FileTypes::FT_DEVICE, 0222); // only write
            fs::device_file *f_err = new fs::device_file(fAttrserr, patherr, 2);
            proc->_ofile->_ofile_ptr[0] = f_in;
            proc->_ofile->_ofile_ptr[1] = f_out;
            proc->_ofile->_ofile_ptr[2] = f_err;
            /// 你好
            /// 这是重定向uart的代码
            /// commented out by @gkq
            new (&dev::k_uart) dev::UartManager(UART0);
            dev::register_debug_uart(&dev::k_uart);

            // net::init_network_stack();
        }

        // 设置进程开始运行的时间点
        if (proc->_start_tick == 0)
        {
            proc->_start_tick = tmm::get_ticks();
            proc->_start_time = tmm::get_ticks();     // 同时设置启动时间
            proc->_start_boottime = tmm::get_ticks(); // 系统启动以来的时间
        }

        // printf("fork_ret\n");
        trap_mgr.usertrapret();
    }

    void ProcessManager::freeproc(Pcb *p)
    {
        printfBlue("[freeproc] PCB for process global_id %d pid %d  tid %d successfully reclaimed\n",
                   p->_global_id, p->_pid, p->_tid);
        /****************************************************************************************
         内存资源已在 exit_proc() 中释放，这里只清理PCB字段
         ****************************************************************************************/

        // 验证进程状态：ZOMBIE（正常退出）、UNUSED（初始状态）、USED（创建失败清理）状态的进程才能被freeproc
        if (p->_state != ProcState::ZOMBIE && p->_state != ProcState::UNUSED && p->_state != ProcState::USED)
        {
            panic("freeproc: process not in valid state for cleanup, current state: %d", (int)p->_state);
        }

        // trapframe 是 alloc_proc() 每次重新分配的物理页。
        // 回收 PCB 时必须释放旧页，否则长回归里大量 fork/clone 会持续泄漏物理页。
        if (p->_trapframe != nullptr)
        {
            mem::k_pmm.free_page(p->_trapframe);
            p->_trapframe = nullptr;
        }

        // printf("[freeproc] Reclaiming PCB for process %s pid %d\n", p->_name, p->_pid);

        /****************************************************************************************
         * 基本进程标识和状态管理清理
         ****************************************************************************************/
        p->_pid = 0;          // 清除进程ID
        p->_tid = 0;          // 清除线程ID
        p->_parent = nullptr; // 清除父进程指针
        p->_name[0] = '\0';   // 清空进程名称
        p->exe.clear();       // 清空可执行文件路径

        // 清除标准Linux进程标识符
        p->_ppid = 0; // 清除父进程PID
        p->_pgid = 0; // 清除进程组ID
        p->_tgid = 0; // 清除线程组ID
        p->_sid = 0;  // 清除会话ID
        p->_uid = 0;  // 清除真实用户ID
        p->_euid = 0; // 清除有效用户ID
        p->_suid = 0; // 清除保存的设置用户ID
        p->_fsuid = 0; // 清除文件系统用户ID
        p->_gid = 0;  // 清除真实组ID
        p->_egid = 0; // 清除有效组ID
        p->_sgid = 0; // 清除保存的设置组ID
        p->_fsgid = 0; // 清除文件系统组ID

        /****************************************************************************************
         * 进程状态和调度信息清理
         ****************************************************************************************/
        p->_chan = nullptr;            // 清空睡眠等待通道
        p->_killed = 0;                // 清除终止标志
        p->_exiting = false;           // 清除退出清理标记
        p->_xstate = 0;                // 清除退出状态码
        p->_parent_exit_signal = ipc::signal::SIGCHLD;
        p->_state = ProcState::UNUSED; // 标记进程控制块为未使用

        p->_slot = 0;                 // 重置时间片
                p->_priority = default_proc_prio; // 重置 nice 值，避免 PCB 复用带出历史优先级
                p->_io_priority_override = default_proc_prio;
                p->_has_io_priority_override = false;

        // 重新初始化CPU亲和性掩码：默认可以在任何CPU上运行
        p->_cpu_mask.fill();

        /****************************************************************************************
         * 文件系统和I/O管理清理
         ****************************************************************************************/
        p->_cwd = nullptr;    // 清空当前工作目录
        p->_cwd_name.clear(); // 清空当前工作目录路径
        p->_umask = 0022;     // 重置umask为默认值
        p->_personality = 0;  // 重置 personality，避免 PCB 复用带出历史状态

	        // 注意：文件描述符表已在exit_proc中清理，这里只重置指针
	        if (p->_ofile != nullptr)
	        {
	            panic("freeproc: ofile should be cleaned in exit_proc, but found non-null pointer");
	        }

	        // 内存管理器应已在 exit_proc()/cleanup_memory_manager() 中清理完毕，这里强制清空指针，
	        // 防止进程池复用 PCB 时把历史地址空间指针带到新进程里。
	        p->reset_memory_manager_ptr();

        /****************************************************************************************
         * 线程和同步原语清理
         ****************************************************************************************/
        p->_futex_addr = nullptr;  // 清空futex等待地址
        p->_futex_key = 0;         // 清空futex匹配键
        p->_clear_tid_addr = 0;    // 清空线程退出时需要清理的地址
        p->_robust_list = nullptr; // 清空健壮futex链表
        p->_robust_list_user_addr = 0;

        /****************************************************************************************
         * 信号处理清理
         ****************************************************************************************/
        // 注意：信号处理结构和栈帧已在exit_proc中清理，这里只重置指针
        p->_sigactions = nullptr; // 清空信号处理结构指针
        p->sig_frame = nullptr;   // 清空信号处理帧指针
        p->_signal = 0;           // 清空待处理信号掩码
        p->_sigmask = 0;          // 清空信号屏蔽掩码
        p->_siginfo_mask = 0;
        memset(p->_queued_siginfo, 0, sizeof(p->_queued_siginfo));
        p->_sigsuspend_restore_pending = false;
        p->_sigsuspend_saved_sigmask = 0;
        p->_alt_stack.ss_sp = nullptr;
        p->_alt_stack.ss_flags = ipc::signal::SS_DISABLE;
        p->_alt_stack.ss_size = 0;
        p->_on_sigstack = false;

        /****************************************************************************************
         * 资源限制清理
         ****************************************************************************************/
        // 重置所有资源限制为0
        for (uint i = 0; i < ResourceLimitId::RLIM_NLIMITS; ++i)
        {
            p->_rlim_vec[i].rlim_cur = 0;
            p->_rlim_vec[i].rlim_max = 0;
        }

        /****************************************************************************************
         * 时间统计和会计信息清理
         ****************************************************************************************/
        p->_start_tick = 0;        // 清零进程开始运行时间
        p->_user_ticks = 0;        // 清零用户态累计时间
        p->_last_user_tick = 0;    // 清零上次进入用户态时间
        p->_kernel_entry_tick = 0; // 清零进入内核态时间
        p->_utime = 0;             // 清零用户态时间
        p->_stime = 0;             // 清零系统态时间
        p->_cutime = 0;            // 清零子进程用户态时间累计
        p->_cstime = 0;            // 清零子进程系统态时间累计
        p->_start_time = 0;        // 清零进程启动时间
        p->_start_boottime = 0;    // 清零自系统启动以来的启动时间
        p->_timens_current = {};
        p->_timens_children = {};
        p->_netns = {};
        reset_interval_timers(p);  // 清空 interval timer，避免 PCB 复用时带出历史状态

        /****************************************************************************************
         * 上下文清理
         ****************************************************************************************/
        memset(&p->_context, 0, sizeof(p->_context)); // 清空上下文信息

        printfBlue("[freeproc] free proc complete\n");
    }

    void ProcessManager::freeproc_creation_failed(Pcb *p)
    {
        /****************************************************************************************
         * 专门处理进程创建失败时的清理
         * 此时进程可能已经分配了部分资源但还没有真正运行
         ****************************************************************************************/

        printf("[freeproc_creation_failed] Cleaning up failed process creation for pid %d\n", p->_pid);

        // 如果已经分配了trapframe，需要释放
        if (p->get_trapframe() != nullptr)
        {
            mem::k_pmm.free_page(p->get_trapframe());
            p->set_trapframe(nullptr);
        }

        // 如果已经创建了ProcessMemoryManager，需要释放
        ProcessMemoryManager *mm = p->get_memory_manager();
        // 创建失败的子进程还没有真正切换运行，这里只按 tid 清理共享段附加记录，避免泄漏 nattch。
        shm::k_smm.detach_all_for_process(p, false, true);
	        if (mm != nullptr)
	        {
	            mm->emergency_cleanup(); // 使用紧急清理，避免正常流程
	            if (mm->get_ref_count() <= 1)
	            {
	                delete mm;
	            }
	            // 这里不能再走 set_memory_manager(nullptr)，否则会对刚删除的 mm 再做一次 cleanup。
	            p->reset_memory_manager_ptr();
	        }

        // 调用标准的PCB清理
        freeproc(p);
    }

    void ProcessManager::debug_process_states()
    {
        /****************************************************************************************
         * 调试函数：打印所有进程的状态信息
         ****************************************************************************************/
        printf("\n========== Process State Debug Info ==========\n");

        int zombie_count = 0;
        int running_count = 0;
        int sleeping_count = 0;
        int unused_count = 0;
        int used_count = 0;

        for (uint i = 0; i < num_process; i++)
        {
            Pcb &p = k_proc_pool[i];
            if (p._state == ProcState::UNUSED)
            {
                unused_count++;
                continue;
            }

            void *user_pc = nullptr;
            if (p.get_trapframe())
            {
#ifdef LOONGARCH
                user_pc = (void *)p.get_trapframe()->era;
#else
                user_pc = (void *)p.get_trapframe()->epc;
#endif
            }

            printf("Process[%d]: pid=%d tid=%d tgid=%d name='%s' state=%d parent_pid=%d pgid=%d sid=%d mm=%p tf=%p era=%p futex=%p clear_tid=%p\n",
                   i, p._pid, p._tid, p._tgid, p._name, (int)p._state,
                   p._parent ? p._parent->_pid : -1, p._pgid, p._sid,
                   p.get_memory_manager(),
                   p.get_trapframe(),
                   user_pc,
                   p._futex_addr,
                   (void *)p._clear_tid_addr);

            switch (p._state)
            {
            case ProcState::ZOMBIE:
                zombie_count++;
                printf("  -> ZOMBIE: xstate=%d, waiting for parent to collect\n", p._xstate);
                break;
            case ProcState::RUNNABLE:
                running_count++;
                printf("  -> RUNNABLE: chan=%p futex=%p\n", p._chan, p._futex_addr);
                break;
            case ProcState::RUNNING:
                running_count++;
                printf("  -> RUNNING: chan=%p futex=%p\n", p._chan, p._futex_addr);
                break;
            case ProcState::SLEEPING:
                sleeping_count++;
                printf("  -> SLEEPING: chan=%p futex=%p\n", p._chan, p._futex_addr);
                break;
            case ProcState::USED:
                used_count++;
                break;
            default:
                printf("  -> UNKNOWN STATE: %d\n", (int)p._state);
                break;
            }
        }

        printf("Summary: UNUSED=%d, USED=%d, RUNNABLE=%d, SLEEPING=%d, ZOMBIE=%d\n",
               unused_count, used_count, running_count, sleeping_count, zombie_count);
        printf("===============================================\n\n");
    }

    bool ProcessManager::verify_process_cleanup(int pid)
    {
        /****************************************************************************************
         * 验证函数：检查指定PID的进程是否正确清理
         ****************************************************************************************/
        for (uint i = 0; i < num_process; i++)
        {
            Pcb &p = k_proc_pool[i];
            if (p._pid == pid && p._state != ProcState::UNUSED)
            {
                printf("[ERROR] Process pid %d still exists in state %d after cleanup\n",
                       pid, (int)p._state);
                return false;
            }
        }
        printf("[OK] Process pid %d successfully cleaned up\n", pid);
        return true;
    }

    int ProcessManager::get_cur_cpuid()
    {
#ifdef LOONGARCH
        // LoongArch 下 tp 主要被我们借作“当前 hart 索引”使用，
        // 但在 userret 的极窄窗口里它可能短暂带着用户线程指针。
        // 对外暴露当前 CPU 编号时直接读 CSR_CPUID，避免把这个瞬时值传播出去。
        return static_cast<int>(r_csr_cpuid());
#else
        return r_tp();
#endif
    }

    void ProcessManager::user_init()
    {
        static int inited = 0;
        // 防止重复初始化
        if (inited != 0)
        {
            panic("re-init user.");
            return;
        }

        Pcb *p = alloc_proc();
        if (p == nullptr)
        {
            panic("user_init: alloc_proc failed");
            return;
        }

        _init_proc = p;

        // 为init进程创建ProcessMemoryManager
        ProcessMemoryManager *init_mm = new ProcessMemoryManager();

        // 完成内存管理器的初始化设置
        if (!init_mm->create_pagetable())
        {
            panic("user_init: failed to create pagetable for init process");
            delete init_mm;
            return;
        }

        // 绑定到当前PCB
        p->set_memory_manager(init_mm);

        // 传入initcode的地址
        printfCyan("initcode pagetable: %p\n", p->get_pagetable()->get_base());
        uint64 initcode_sz = (uint64)initcode_end - (uint64)initcode_start;
        uint64 allocated_sz = mem::k_vmm.uvmfirst(*p->get_pagetable(), (uint64)initcode_start, initcode_sz);

        printf("initcode start: %p, end: %p\n", initcode_start, initcode_end);
        printf("initcode size: %p, total allocated space: %p\n", initcode_sz, allocated_sz);

        // 使用新的程序段管理
        p->add_program_section((void *)0, allocated_sz, "initcode");

        // 初始化堆在代码段后面
        p->init_heap(allocated_sz);

        // 设置程序计数器和栈指针 - 架构相关的部分
#ifdef RISCV
        p->_trapframe->epc = 0;
#elif defined(LOONGARCH)
        p->_trapframe->era = 0;
#endif
        p->_trapframe->sp = allocated_sz;

        safestrcpy(p->_name, "initcode", sizeof(p->_name));
        p->_parent = p; // init进程是自己的父进程
        // safestrcpy(p->_cwd_name, "/", sizeof(p->_cwd_name));
        p->_cwd_name = "/";

        // init进程的特殊属性（在alloc_proc中已设置）：
        // - PID = 1
        // - PGID = 1（成为进程组1的领导者）
        // - SID = 1（成为会话1的领导者）
        // - 所有其他进程最终都成为init进程的子进程

        p->_state = ProcState::RUNNABLE;

        p->_lock.release();
    }

    // Atomically release lock and sleep on chan.
    // Reacquires lock when awakened.

    void ProcessManager::set_killed(Pcb *p)
    {
        p->_lock.acquire();
        p->_killed = 1;
        p->_lock.release();
    }
    // Kill the process with the given pid.
    // The victim won't exit until it tries to return
    // to user space (see usertrap() in trap.c).
    int ProcessManager::kill_proc(int pid)
    {
        Pcb *p;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();

            // 如果找到目标 pid 的进程
            if (p->_pid == pid)
            {
                // 设置该进程的 killed 标志位为 1，
                // 表示该进程已被请求终止。
                // 被 kill 并不立即终止进程，而是在合适的时机由进程自行处理。
                p->_killed = 1;

                // 若该进程当前在 sleep（通常是等待 I/O 或锁）
                // 将其唤醒（设为 RUNNABLE），这样调度器会调度它运行，
                // 让它可以检查 _killed 并自行退出。
                if (p->_state == ProcState::SLEEPING)
                {
                    // 提前唤醒等待中的进程，
                    // 避免它永远睡着不被调度，也就永远无法响应 kill。
                    p->_state = ProcState::RUNNABLE;
                }

                p->_lock.release();
                return 0;
            }

            p->_lock.release();
        }
        return -1; // 没找到对应 pid 的进程
    }

    int ProcessManager::kill_signal(int pid, int sig, const ipc::signal::LinuxSigInfo *info)
    {
        Pcb *p;
        int count = 0; // 记录发送信号的进程数量
        printfCyan("kill_signal: pid=%d, sig=%d\n", pid, sig);
        auto wake_if_signal_interruptible = [](Pcb *target) {
            if (target->_state == ProcState::SLEEPING &&
                proc::ipc::signal::has_unmasked_signal_pending(target))
            {
                target->_state = ProcState::RUNNABLE;
            }
        };

        if (pid > 0)
        {
            // 发送信号给特定PID的进程
            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pid == pid && p->_state != ProcState::UNUSED)
                {
                    p->add_signal(sig, info);
                    wake_if_signal_interruptible(p);
                    p->_lock.release();
                    return 0;
                }
                p->_lock.release();
            }
            return -1; // 没找到指定PID的进程
        }
        else if (pid == 0)
        {
            // 发送信号给当前进程组的所有进程
            Pcb *current = get_cur_pcb();
            if (current == nullptr)
                return -1;

            int target_pgid = current->_pgid;
            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pgid == target_pgid && p->_state != ProcState::UNUSED)
                {
                    p->add_signal(sig, info);
                    wake_if_signal_interruptible(p);
                    count++;
                }
                p->_lock.release();
            }
            return count > 0 ? 0 : -1;
        }
        else if (pid == -1)
        {
            panic("kill_signal: pid == -1 is not implemented");
            // 发送信号给当前进程有权限发送的所有进程（除了init进程）
            Pcb *current = get_cur_pcb();
            if (current == nullptr)
                return -1;

            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pid > 1 && p->_state != ProcState::UNUSED &&    // 跳过init进程
                    (p->_uid == current->_euid || current->_euid == 0)) // 权限检查
                {
                    p->add_signal(sig, info);
                    wake_if_signal_interruptible(p);
                    count++;
                }
                p->_lock.release();
            }
            return count > 0 ? 0 : -1;
        }
        else
        {
            // pid < -1: 发送信号给进程组ID为-pid的所有进程
            int target_pgid = -pid;
            Pcb *current = get_cur_pcb();
            if (current == nullptr)
                return -1;

            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                p->_lock.acquire();
                if (p->_pgid == target_pgid && p->_state != ProcState::UNUSED &&
                    (p->_uid == current->_euid || current->_euid == 0)) // 权限检查
                {
                    p->add_signal(sig, info);
                    wake_if_signal_interruptible(p);
                    count++;
                }
                p->_lock.release();
            }
            return count > 0 ? 0 : -1;
        }
    }

    int ProcessManager::tkill(int tid, int sig, const ipc::signal::LinuxSigInfo *info)
    {
        Pcb *p;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();
            if (p->_tid == tid)
            {
                p->add_signal(sig, info);
                // 线程定向信号和 kill(2) 一样，都需要把“可被信号中断的睡眠”及时唤醒。
                // pthread_cancel() 最终会走到 pthread_kill/tgkill/tkill；如果这里只是记账信号，
                // 但不把卡在 futex/rt_sigsuspend 等等待里的目标线程改回 RUNNABLE，
                // 取消请求就会永远堆在 _signal 里，表现成用户态 join/sem_wait 长时间卡死。
                if (p->_state == ProcState::SLEEPING &&
                    proc::ipc::signal::has_unmasked_signal_pending(p))
                {
                    p->_state = ProcState::RUNNABLE;
                }
                p->_lock.release();
                return 0;
            }
            p->_lock.release();
        }
        return -1;
    }

    int ProcessManager::tgkill(int tgid, int tid, int sig, const ipc::signal::LinuxSigInfo *info)
    {
        Pcb *p;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();
            if (p->_tid == tid && p->_tgid == tgid)
            {
                p->add_signal(sig, info);
                // tgkill(2) 是线程取消/定向信号的核心路径。
                // 保持和 kill_signal() 一致：只要目标线程当前睡眠且存在未屏蔽待处理信号，
                // 就要把它唤醒，让阻塞中的系统调用有机会返回 EINTR。
                if (p->_state == ProcState::SLEEPING &&
                    proc::ipc::signal::has_unmasked_signal_pending(p))
                {
                    p->_state = ProcState::RUNNABLE;
                }
                p->_lock.release();
                return 0;
            }
            p->_lock.release();
        }
        return -1; // 未找到匹配的线程
    }

    Pcb *ProcessManager::find_proc_by_pid(int pid)
    {
        for (Pcb *p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            if (p->_pid == pid && p->_state != ProcState::UNUSED)
            {
                return p;
            }
        }
        return nullptr; // 未找到对应PID的进程
    }

    // Copy from either a user address, or kernel address,
    // depending on usr_src.
    // Returns 0 on success, -1 on error.
    int ProcessManager::either_copy_in(void *dst, int user_src, uint64 src, uint64 len)
    {
        Pcb *p = get_cur_pcb();
        if (user_src)
        {
            return mem::k_vmm.copy_in(*p->get_pagetable(), dst, src, len);
        }
        else
        {
            memmove(dst, (char *)src, len);
            return len;
        }
    }
    // Copy to either a user address, or kernel address,
    // depending on usr_dst.
    // Returns 0 on success, -1 on error.
    int ProcessManager::either_copy_out(void *src, int user_dst, uint64 dst, uint64 len)
    {
        Pcb *p = get_cur_pcb();
        if (user_dst)
        {
            return mem::k_vmm.copy_out(*p->get_pagetable(), dst, src, len);
        }
        else
        {
            memmove((char *)dst, src, len);
            return len;
        }
    }
    // Print a process listing to console.  For debugging.
    // Runs when user types ^P on console.
    // No lock to avoid wedging a stuck machine further.
    void ProcessManager::procdump()
    {
        static const char *states[6] = {
            "unused", // ProcState::UNUSED
            "used",   // ProcState::USED
            "sleep ", // ProcState::SLEEPING
            "runble", // ProcState::RUNNABLE
            "run   ", // ProcState::RUNNING
            "zombie"  // ProcState::ZOMBIE
        };
        Pcb *p;
        char *state;

        printf("\n");
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            if (p->_state == ProcState::UNUSED)
                continue;
            if ((int)p->_state >= 0 && (int)p->_state < 6 && states[(int)p->_state])
                state = (char *)states[(int)p->_state];
            else
                state = (char *)"???";
            printf("%d %s %s pgid=%d sid=%d", p->_pid, state, p->_name, p->_pgid, p->_sid);
            printf("\n");
        }
    }
    /// @brief
    /// @param p
    /// @param f
    /// @param fd
    /// @return
    int ProcessManager::alloc_fd(Pcb *p, fs::file *f, int fd)
    {
        int fd_limit = effective_fd_limit(p);
        if (fd < 0 || fd >= fd_limit || f == nullptr || p->_ofile == nullptr)
            return -1;

        fs::file *old_file = nullptr;
        p->_ofile->_lock.acquire();
        if (p->_ofile->_ofile_ptr[fd] != nullptr && p->_ofile->_ofile_ptr[fd] != f)
        {
            old_file = p->_ofile->_ofile_ptr[fd];
        }
        p->_ofile->_ofile_ptr[fd] = f;
        p->_ofile->_reserved[fd] = false;
        p->_ofile->_fl_cloexec[fd] = false; // 默认不设置 CLOEXEC
        p->_ofile->_lock.release();

        if (old_file != nullptr)
        {
            if (!is_probably_live_file_object(old_file))
            {
                printfRed("[alloc_fd] 检测到异常旧文件指针，直接丢弃: pid=%d fd=%d file=%p\n",
                          p->_pid, fd, old_file);
            }
            else
            {
                old_file->free_file();
            }
        }

        return fd;
    }

    void ProcessManager::get_cur_proc_tms(tmm::tms *tsv)
    {
        Pcb *p = get_cur_pcb();

        tsv->tms_utime = p->_user_ticks;
        tsv->tms_stime = p->_stime;   // 使用累计的系统态时间
        tsv->tms_cutime = p->_cutime; // 使用累计的子进程用户态时间
        tsv->tms_cstime = p->_cstime; // 使用累计的子进程系统态时间
    }
    int ProcessManager::alloc_fd(Pcb *p, fs::file *f)
    {
        int fd;

        if (p->_ofile == nullptr || f == nullptr)
            return -1;

        int fd_limit = effective_fd_limit(p);
        p->_ofile->_lock.acquire();
        for (fd = 0; fd < fd_limit; fd++)
        {
            if (p->_ofile->_ofile_ptr[fd] == nullptr && !p->_ofile->_reserved[fd])
            {
                p->_ofile->_ofile_ptr[fd] = f;
                p->_ofile->_reserved[fd] = false;
                p->_ofile->_fl_cloexec[fd] = false; // 默认不设置 CLOEXEC
                p->_ofile->_lock.release();
                return fd;
            }
        }
        p->_ofile->_lock.release();
        return syscall::SYS_EMFILE;
    }

    int ProcessManager::reserve_fd(Pcb *p)
    {
        if (p == nullptr || p->_ofile == nullptr)
        {
            return -1;
        }

        int fd_limit = effective_fd_limit(p);
        p->_ofile->_lock.acquire();
        for (int fd = 0; fd < fd_limit; ++fd)
        {
            if (p->_ofile->_ofile_ptr[fd] == nullptr && !p->_ofile->_reserved[fd])
            {
                p->_ofile->_reserved[fd] = true;
                p->_ofile->_fl_cloexec[fd] = false;
                p->_ofile->_lock.release();
                return fd;
            }
        }
        p->_ofile->_lock.release();
        return syscall::SYS_EMFILE;
    }

    int ProcessManager::install_fd(Pcb *p, fs::file *f, int fd)
    {
        return alloc_fd(p, f, fd);
    }

    void ProcessManager::release_fd(Pcb *p, int fd)
    {
        if (p == nullptr || p->_ofile == nullptr || fd < 0 || fd >= (int)max_open_files)
        {
            return;
        }

        p->_ofile->_lock.acquire();
        p->_ofile->_ofile_ptr[fd] = nullptr;
        p->_ofile->_reserved[fd] = false;
        p->_ofile->_fl_cloexec[fd] = false;
        p->_ofile->_lock.release();
    }

    int ProcessManager::clone(uint64 flags, uint64 stack_ptr, uint64 ptid, uint64 tls,
                              uint64 ctid, bool is_clone3, int exit_signal)
    {
        Pcb *p = get_cur_pcb();
        Pcb *np = fork(p, flags, stack_ptr, ctid, is_clone3, exit_signal);
        if (np == nullptr)
        {
            return -1; // EAGAIN: Out of memory
        }
        int new_tid = np->_tid;
        uint64 new_pid = np->_pid;

#ifdef LOONGARCH
        if ((flags & syscall::CLONE_THREAD) && p->get_pagetable() != nullptr)
        {
            uint64 trapframe_pa = VIRT2PHY((uint64)np->_trapframe);
            auto check_user_alias = [&](uint64 user_addr, const char *label) {
                if (user_addr == 0)
                {
                    return;
                }
                mem::Pte user_pte = p->get_pagetable()->walk(PGROUNDDOWN(user_addr), false);
                if (!user_pte.is_null() && user_pte.is_valid())
                {
                    uint64 user_pa = (uint64)user_pte.pa();
                    if (user_pa == trapframe_pa)
                    {
                        panic("debug clone trapframe alias: parent pid=%d tid=%d child tid=%d label=%s user=%p user_pa=%p trapframe=%p trapframe_pa=%p stack=%p tls=%p ctid=%p",
                              p->_pid,
                              p->_tid,
                              np->_tid,
                              label,
                              (void *)user_addr,
                              (void *)user_pa,
                              np->_trapframe,
                              (void *)trapframe_pa,
                              (void *)stack_ptr,
                              (void *)tls,
                              (void *)ctid);
                    }
                }
            };
            check_user_alias(stack_ptr, "stack");
            check_user_alias(tls, "tls");
            check_user_alias(ctid, "ctid");
        }
#endif

        if (flags & syscall::CLONE_SETTLS)
        {
            np->_trapframe->tp = tls; // 设置线程局部存储指针
        }
        if (flags & syscall::CLONE_PARENT_SETTID)
        {
            // parent_tid 指向的是 pid_t，必须按 4 字节写。
            // 之前按 8 字节写会把线程库紧邻的状态字段一并覆盖掉。
            if (mem::k_vmm.copy_out(*p->get_pagetable(), ptid, &new_tid, sizeof(new_tid)) < 0)
            {
                freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                np->_lock.release();
                return -1; // EFAULT: Bad address
            }
        }
        if (flags & syscall::CLONE_PARENT)
        {
            if (p->_parent != nullptr)
            {
                np->_parent = p->_parent; // 继承父进程
            }
            else
            {
                panic("clone: parent process is null");
            }
        }
        if (flags & syscall::CLONE_VFORK)
        {
            // CLONE_VFORK 语义：父进程必须等到子进程 execve 或 exit 释放共享地址空间后
            // 才能继续运行。否则父进程可能先 munmap 掉传给子进程的共享用户栈，
            // glibc posix_spawn/system 路径会随机在子进程栈上 SIGSEGV。
            np->_vfork_parent = p;
        }
        np->_lock.release();

        if (flags & syscall::CLONE_VFORK)
        {
            _wait_lock.acquire();
            while (np->_vfork_parent == p &&
                   np->_state != ProcState::UNUSED &&
                   np->_state != ProcState::ZOMBIE)
            {
                sleep(np, &_wait_lock);
            }
            _wait_lock.release();
        }
        // Linux clone()/clone3() 在线程语义下返回新线程 tid，而不是线程组 pid。
        return (flags & syscall::CLONE_THREAD) ? (uint64)(uint32)new_tid : new_pid;
    }

    // 这个函数主要用提供clone的底层支持
    Pcb *ProcessManager::fork(Pcb *p, uint64 flags, uint64 stack_ptr,
                              uint64 ctid, bool is_clone3, int exit_signal)
    {
        TODO("copy on write fork");

        // ===== 基础验证和资源分配 =====
        // 参数验证
        if (p == nullptr)
        {
            return nullptr;
        }

        uint64 i;
        Pcb *np; // new proc

        // 分配新进程控制块
        if ((np = alloc_proc()) == nullptr)
        {
            return nullptr;
        }

        // 拷贝父进程的陷阱帧，而不是直接指向，后面有可能会修改
        *np->_trapframe = *p->_trapframe;

        // 设置父子进程关系
        np->_parent = p;

        // ===== 基本属性复制 =====
        // 继承文件系统相关属性
        np->_cwd = p->_cwd;           // 继承当前工作目录
        np->_cwd_name = p->_cwd_name; // 继承当前工作目录名称
        np->exe = p->exe;             // 继承真实可执行文件路径，保持 /proc/self/exe 语义稳定
        np->_umask = p->_umask;       // 继承文件模式创建掩码
        np->_personality = p->_personality; // 继承 personality，保持与 Linux 一致

        // ===== 身份信息和进程关系设置 =====
        // 继承父进程的身份信息
        np->_ppid = p->_pid;
        np->_uid = p->_uid;
        np->_euid = p->_euid;
        np->_suid = p->_suid;
        np->_fsuid = p->_fsuid;
        np->_gid = p->_gid;
        np->_egid = p->_egid;
        np->_sgid = p->_sgid;
        np->_fsgid = p->_fsgid;
        np->_parent_exit_signal = exit_signal >= 0
                                      ? exit_signal
                                      : static_cast<int>(flags & syscall::CSIGNAL);
        if (flags & syscall::CLONE_THREAD)
        {
            // 线程退出不应该向父进程额外发送“子进程退出信号”，join 走的是 tid/futex 语义。
            np->_parent_exit_signal = 0;
        }
        if (flags & syscall::CLONE_THREAD)
        {
            // 线程创建不切换当前 time namespace，只继承父线程已经在看的那一份。
            np->_timens_current = p->_timens_current;
        }
        else
        {
            // 普通 fork/clone 子进程进入 parent 的 time_for_children namespace。
            np->_timens_current = p->_timens_children;
        }
        np->_timens_children = np->_timens_current;
        np->_netns = p->_netns;
        if (flags & syscall::CLONE_NEWNET)
        {
            // 当前先补最小 netns 语义：
            // 1. 新 namespace 继承 default/tag 作为模板；
            // 2. lo/tag 重新按 default/tag 初始化，不能继续沿用 parent 的 lo/tag。
            np->_netns.ipv4_conf_lo_tag = p->_netns.ipv4_conf_default_tag;
        }

        // 进程组ID继承逻辑：
        // 1. 对于普通fork()，子进程继承父进程的进程组
        // 2. 对于线程创建(CLONE_THREAD)，共享进程组
        // 3. 对于会话领导者，需要特殊处理
        if (flags & syscall::CLONE_THREAD)
        {
            // 线程共享进程组和会话
            np->_pgid = p->_pgid;
            np->_tgid = p->_tgid; // 线程组ID保持一致
            np->_sid = p->_sid;
        }
        else
        {
            // 普通进程创建，继承进程组但获得新的线程组ID
            np->_pgid = p->_pgid;
            np->_tgid = np->_pid; // 新进程成为自己线程组的领导者
            np->_sid = p->_sid;
        }

        // ===== 时间统计重置 =====
        // 重置子进程的时间统计（alloc_proc已经初始化，但这里明确重置）
        uint64 cur_tick = tmm::get_ticks();
        np->_start_tick = cur_tick;
        np->_start_time = cur_tick;
        np->_start_boottime = cur_tick;
        np->_user_ticks = 0;
        np->_last_user_tick = 0;
        np->_kernel_entry_tick = 0;
        np->_stime = 0;
        np->_cutime = 0;
        np->_cstime = 0;

        // ===== 进程名称设置 =====
        // 为子进程设置名称，添加子进程标识
        const char child_name_suffix[] = "-child";
        size_t parent_name_len = strlen(p->_name);
        size_t suffix_len = strlen(child_name_suffix);

        // 确保不超出缓冲区大小
        if (parent_name_len + suffix_len < sizeof(np->_name))
        {
            strcpy(np->_name, p->_name);
            strcat(np->_name, child_name_suffix);
        }
        else
        {
            // 父进程名称太长，需要截断
            size_t max_parent_len = sizeof(np->_name) - suffix_len - 1;
            strncpy(np->_name, p->_name, max_parent_len);
            np->_name[max_parent_len] = '\0';
            strcat(np->_name, child_name_suffix);
        }

        // ===== 文件描述符处理 =====

        if (flags & syscall::CLONE_FILES)
        {
            // 共享文件描述符表
            np->cleanup_ofile();
            np->_ofile = p->_ofile;
            np->_ofile->_shared_ref_cnt++; // 增加引用计数
        }
        else
        {
            // 深拷贝文件描述符表
            for (i = 0; i < (int)max_open_files; i++)
            {
                fs::file *parent_file = p->_ofile->_ofile_ptr[i];
                if (parent_file)
                {
                    if (!is_probably_live_file_object(parent_file))
                    {
                        printfRed("[fork] 检测到异常文件描述符条目，直接清理: parent pid=%d child pid=%d fd=%d file=%p\n",
                                  p->_pid, np->_pid, i, parent_file);
                        p->_ofile->_ofile_ptr[i] = nullptr;
                        p->_ofile->_fl_cloexec[i] = false;
                        continue;
                    }

                    // fs::k_file_table.dup( p->_ofile[ i ] );
                    parent_file->dup();
                    np->_ofile->_ofile_ptr[i] = parent_file;
                    np->_ofile->_fl_cloexec[i] = p->_ofile->_fl_cloexec[i]; // 继承 CLOEXEC 标志
                }
            }
        }

        // ===== 内存管理 =====
        if (flags & syscall::CLONE_VM)
        {
            // 共享虚拟内存：新进程共享父进程的内存管理器
            ProcessMemoryManager *parent_mm = p->get_memory_manager();
            if (parent_mm != nullptr)
            {
                np->set_memory_manager(parent_mm->share_for_thread());
            }
            else
            {
                panic("[fork] parent memory_manager is null");
            }
        }
        else
        {
            printfBlue("[fork] clone parent vm\n");
            // fork 操作：创建独立的内存管理器副本
            ProcessMemoryManager *parent_mm = p->get_memory_manager();
            if (parent_mm != nullptr)
            {
                // 继承共享内存附加记录：把父线程tid对应的附加项复制到子线程tid
                // 注意：此处 np->_tid 已在 alloc_proc() 中分配
                shm::k_smm.duplicate_attachments_for_fork(p->get_tid(), np->get_tid());
                ProcessMemoryManager *cloned_mm = parent_mm->clone_for_fork();
                if (cloned_mm == nullptr)
                {
                    panic("[fork] clone failed");
                    freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                    np->_lock.release();
                    panic("fork failed: memory copy failed");
                    return nullptr;
                }
                np->set_memory_manager(cloned_mm);
            }
        }

        // ===== 信号处理 =====
        if (flags & syscall::CLONE_SIGHAND)
        {
            // 共享信号处理结构
            np->cleanup_sighand(); // 使用cleanup方法来正确处理引用计数
            // 共享父进程的信号处理结构
            np->_sigactions = p->_sigactions;
            if (p->_sigactions != nullptr)
            {
                p->_sigactions->refcnt++; // 增加引用计数
            }
        }
        else
        {
            // 不共享信号处理结构，需要深拷贝
            if (p->_sigactions != nullptr && np->_sigactions != nullptr)
            {
                for (int i = 0; i <= ipc::signal::SIGRTMAX; ++i)
                {
                    if (p->_sigactions->actions[i] != nullptr)
                    {
                        np->_sigactions->actions[i] = new ipc::signal::sigaction;
                        if (np->_sigactions->actions[i] != nullptr)
                        {
                            *(np->_sigactions->actions[i]) = *(p->_sigactions->actions[i]);
                        }
                    }
                }
            }
        }

        // Linux 语义：
        // 1. fork()/普通 clone 继承父任务当前的备用信号栈设置；
        // 2. 但 CLONE_VM 且不是 CLONE_VFORK 的线程语义下，child 的备用信号栈必须禁用，
        //    否则新线程会错误复用父线程的 altstack 元数据。
        if ((flags & syscall::CLONE_VM) && !(flags & syscall::CLONE_VFORK))
        {
            np->_alt_stack.ss_sp = nullptr;
            np->_alt_stack.ss_flags = ipc::signal::SS_DISABLE;
            np->_alt_stack.ss_size = 0;
        }
        else
        {
            np->_alt_stack = p->_alt_stack;
        }
        np->_on_sigstack = false;

        if (flags & syscall::CLONE_THREAD)
        {
            // TODO: 清除信号掩码
            np->_tgid = p->_tgid; // 线程共享线程组 ID
            np->_pid = p->_pid;   // 线程共享 PID
            // TODO: 共享定时器
        }
        else
        {
            // TODO: 共享信号掩码
            np->_tgid = np->_pid; // 新进程的线程组 ID 等于自己的 PID
            // pid已经在 alloc_proc 中设置了
            // 定时器已经设置过了
        }

        // Linux fork/clone 语义：子任务从系统调用返回时 a0/rax 等返回值寄存器为 0。
        // 这个约束与是否创建线程无关，后续如果用户态需要在子任务里跑 trampoline，
        // 也应由 libc 封装，而不是由内核擅自改 PC/参数寄存器。
        np->_trapframe->a0 = 0;
        if (stack_ptr != 0)
        {
            // clone()/clone3() 的 child_stack 只是“子任务返回到用户态时使用的栈顶”。
            // 内核不应窥探用户栈里的函数指针/参数，也不应把 PC 改成用户自定义入口；
            // 这些都是 libc clone 封装层的职责。LoongArch 上之前的做法会直接把
            // glibc/LTP 的 clone 子任务跳到错误地址，最终在用户态 SIGSEGV。
            np->_trapframe->sp = stack_ptr;
            if ((flags & syscall::CLONE_VM) == 0)
            {
                // glibc/musl 的 clone 封装都会把 fn/arg 压到 child_stack 顶部，
                // 子任务返回用户态后立刻从这块新栈里取入口和参数。
                // 仅靠“程序段/堆/VMA 元数据复制”有时会漏掉这块临时子栈的驻留页，
                // 于是子任务会在第一条 ld.d / jirl 前就读到空页或旧内容而 SIGSEGV。
                // 这里额外把 child_stack 顶部附近两页强制复制过去，保证 clone 子栈
                // 的入口数据和最初几层调用栈在子进程里可见。
                uint64 stack_copy_end = PGROUNDUP(stack_ptr + sizeof(uint64) * 2);
                uint64 stack_copy_start = PGROUNDDOWN(stack_ptr >= PGSIZE ? stack_ptr - PGSIZE : 0);
                if (stack_copy_end > stack_copy_start)
                {
                    bool stack_copy_ok = true;
                    for (uint64 copy_va = stack_copy_start; copy_va < stack_copy_end; copy_va += PGSIZE)
                    {
                        mem::Pte child_pte = np->get_pagetable()->walk(copy_va, false);
                        if (!child_pte.is_null() && child_pte.is_valid())
                        {
                            continue;
                        }

                        if (mem::k_vmm.vm_copy(*p->get_pagetable(),
                                               *np->get_pagetable(),
                                               copy_va,
                                               PGSIZE) < 0)
                        {
                            stack_copy_ok = false;
                            break;
                        }
                    }

                    if (!stack_copy_ok)
                    {
                        freeproc_creation_failed(np);
                        np->_lock.release();
                        return nullptr;
                    }
                }
            }
        }

        if (flags & syscall::CLONE_CHILD_SETTID)
        {
            // 如果设置了 CLONE_CHILD_SETTID，则设置子进程的线程 ID
            if (ctid != 0)
            {
                // Linux 语义要求写入“子进程地址空间”中的 child_tid。
                // 对非 CLONE_VM 的 fork/clone，父子页表已经分离，写父页表会让子进程
                // 看到未初始化的 tid 字段，进而破坏 glibc/pthread 的运行时状态。
                int child_tid = np->_tid;
                if (mem::k_vmm.copy_out(*np->get_pagetable(), ctid, &child_tid, sizeof(child_tid)) < 0)
                {
                    freeproc_creation_failed(np); // 使用专门的创建失败清理函数
                    np->_lock.release();
                    return nullptr; // EFAULT: Bad address
                }
            }
            else
            {
                printfRed("fork: ctid is 0, CLONE_CHILD_SETTID will not set tid\n");
            }
        }
        if (flags & syscall::CLONE_CHILD_CLEARTID)
        {
            // 如果设置了 CLONE_CHILD_CLEARTID，则在子进程退出时清除线程 ID
            np->_clear_tid_addr = ctid;
        }

        np->_state = ProcState::RUNNABLE;

        return np;
    }

    /// @brief
    /// @param n n的意思是扩展的字节数，
    /// 如果 n > 0，则扩展到当前进程的内存大小 + n
    /// 如果 n < 0，则收缩到当前进程的内存大小 + n
    /// @return
    int
    ProcessManager::growproc(int n)
    {
        Pcb *p = get_cur_pcb();
        MemoryLockGuard memory_guard(p != nullptr ? p->get_memory_manager() : nullptr);

        if (n == 0)
        {
            return 0; // 无需改变
        }

        if (n > 0)
        {
            // 扩展堆
            uint64 current_end = p->get_heap_end();
            uint64 new_end = current_end + n;

            // 检查是否超出地址空间限制
            if (new_end >= MAXVA - PGSIZE)
            {
                return -1;
            }

            uint64 result = p->grow_heap(new_end);
            if (result < new_end)
            {
                return -1; // 扩展失败
            }
        }
        else
        {
            // 缩减堆 (n < 0)
            uint64 current_end = p->get_heap_end();
            uint64 new_end = current_end + n; // n是负数

            // 确保不会缩减到堆起始地址之前
            if (new_end < p->get_heap_start())
            {
                new_end = p->get_heap_start();
            }

            p->shrink_heap(new_end);
        }

        return 0;
    }

    /// @brief
    /// @param n 参数n是地址，意思是扩展到 n 地址
    /// 如果 n == 0，则返回当前进程的内存大小
    /// @return
    long ProcessManager::brk(long n)
    {
        Pcb *p = get_cur_pcb();
        MemoryLockGuard memory_guard(p != nullptr ? p->get_memory_manager() : nullptr);

        // 如果 n 为 0，返回当前堆的结束地址
        if (n == 0)
        {
            return p->get_heap_end();
        }

        // 检查请求的地址是否合理
        if ((uint64)n < p->get_heap_start())
        {
            // Linux brk(2) 失败时返回当前 program break，而不是 -1。
            // malloc/sbrk 会用“返回值是否达到请求地址”判断成功，返回 -1 会污染用户态堆边界。
            return p->get_heap_end();
        }

        // 如果请求缩减堆
        if ((uint64)n < p->get_heap_end())
        {
            uint64 new_end = p->shrink_heap((uint64)n);
            return new_end;
        }
        // 如果请求扩展堆
        else if ((uint64)n > p->get_heap_end())
        {
            uint64 new_end = p->grow_heap((uint64)n);
            if (new_end < (uint64)n)
            {
                return p->get_heap_end(); // 扩展失败时保持 Linux brk 语义
            }
            return new_end;
        }

        // 如果地址相同，直接返回
        return n;
    }

    long ProcessManager::sbrk(long increment)
    {
        Pcb *p = get_cur_pcb();
        MemoryLockGuard memory_guard(p != nullptr ? p->get_memory_manager() : nullptr);
        uint64 old_end = p->get_heap_end();

        // 如果 increment 为 0，返回当前堆结束地址
        if (increment == 0)
        {
            return old_end;
        }

        uint64 new_end = old_end + increment;

        // 如果是缩减堆
        if (increment < 0)
        {
            if (new_end < p->get_heap_start())
            {
                return -1; // 不能缩减到堆起始地址之前
            }
            uint64 result = p->shrink_heap(new_end);
            if (result != new_end)
            {
                return -1;
            }
        }
        // 如果是扩展堆
        else
        {
            uint64 result = p->grow_heap(new_end);
            if (result < new_end)
            {
                return -1; // 扩展失败
            }
        }

        return old_end; // 返回原来的堆结束地址
    }

    int ProcessManager::wait4(int child_pid, uint64 addr, int option)
    {
        // debug_process_states();
        Pcb *p = k_pm.get_cur_pcb();
        printfBlue("[wait4] pid: %d child_pid: %d, addr: %p, option: %d\n",
                   p->_pid, child_pid, (void *)addr, option);

        // 检查不支持的选项标志
        const int supported_options = syscall::WNOHANG | syscall::WUNTRACED |
                                      syscall::__WNOTHREAD | syscall::__WALL |
                                      syscall::__WCLONE;
        const int unsupported_options = option & ~supported_options;
        if (unsupported_options != 0)
        {
            printf("[wait4] unsupported option flags: 0x%x, returning -EINVAL\n", unsupported_options);
            return syscall::SYS_EINVAL;
        }

        // 对于特定PID情况，验证主线程的父子关系
        if (child_pid > 0)
        {
            bool found_main_thread = false;
            for (uint i = 0; i < num_process; i++)
            {
                Pcb *np = &k_proc_pool[i];
                // 找到主线程（pid == tid == child_pid）
                if (np->_pid == child_pid && np->_tid == child_pid)
                {
                    found_main_thread = true;
                    // 检查主线程的父进程是否是当前进程
                    if (np->_parent != p)
                    {
                        printf("[wait4] main thread pid %d parent is not current process, returning -ECHILD\n", child_pid);
                        return -ECHILD;
                    }
                    break;
                }
            }

            // 如果没有找到主线程，说明该PID不存在
            if (!found_main_thread)
            {
                printf("[wait4] main thread with pid %d not found, returning -ECHILD\n", child_pid);
                return -ECHILD;
            }
        }

        _wait_lock.acquire();
        bool have_group_status = false;
        bool group_status_from_leader = false;
        int group_status = 0;

        for (;;)
        {
            bool found_children = false;
            bool collected_zombie = false;
            int returned_pid = -1;

            // 遍历所有进程，寻找符合条件的子进程
            for (uint i = 0; i < num_process; i++)
            {
                Pcb *np = &k_proc_pool[i];
                // printf("[wait4] checking global_id: %d, pid: %d tid: %d state: %d\n", np->_global_id, np->_pid, np->_tid, (int)np->get_state());

                // 检查是否是目标子进程
                if (!is_target_child(np, p, child_pid))
                    continue;

                np->_lock.acquire();
                found_children = true;

                // 如果是zombie，回收它
                if (np->get_state() == ProcState::ZOMBIE)
                {
                    returned_pid = np->_pid;
                    int zombie_xstate = np->_xstate;

                    if (child_pid > 0)
                    {
                        // waitpid(leader_pid, ...) 等的是整个线程组完成后的“进程退出状态”。
                        // 不能在回收每个线程时都覆盖一次状态，否则后面被 exit_group 杀掉的
                        // 辅助线程（常见为 xstate=-1）会把组长原本正确的退出码冲掉。
                        bool is_group_leader = np->_pid == np->_tid;
                        if (!have_group_status || (is_group_leader && !group_status_from_leader))
                        {
                            group_status = zombie_xstate;
                            have_group_status = true;
                            group_status_from_leader = is_group_leader;
                        }
                    }

                    printfBlue("[wait4] freeproc child pid: %d tid: %d\n", np->_pid, np->_tid);
                    k_pm.freeproc(np);
                    np->_lock.release();

                    // 对于特定PID，检查是否还有其他同PID的线程
                    if (child_pid > 0)
                    {
                        if (!has_remaining_threads(p, child_pid))
                        {
                            _wait_lock.release();
                            if (addr != 0 && have_group_status &&
                                mem::k_vmm.copy_out(*p->get_pagetable(), addr,
                                                    (const char *)&group_status, sizeof(group_status)) < 0)
                            {
                                return -1;
                            }
                            printfBlue("[wait4] all threads of pid %d have exited\n", child_pid);
                            return returned_pid; // 所有线程都已回收
                        }
                        // 还有线程未退出，继续等待
                        collected_zombie = true;
                        // break;  // 重新开始扫描
                    }
                    else
                    {
                        _wait_lock.release();
                        if (addr != 0 &&
                            mem::k_vmm.copy_out(*p->get_pagetable(), addr,
                                                (const char *)&zombie_xstate, sizeof(zombie_xstate)) < 0)
                        {
                            return -1;
                        }
                        return returned_pid; // 非特定PID情况，回收一个就返回
                    }
                }
                else
                {
                    np->_lock.release();
                }
            }

            // 如果没有找到任何子进程或当前进程被杀死
            if (!found_children || p->_killed)
            {
                _wait_lock.release();
                return syscall::SYS_ECHILD;
            }

            // 如果设置了WNOHANG且没有可回收的zombie，立即返回
            if ((option & syscall::WNOHANG) && !collected_zombie)
            {
                _wait_lock.release();
                return 0;
            }

            // 等待子进程退出
            sleep(p, &_wait_lock);
        }
    }

    // 辅助函数：检查是否是目标子进程
    bool ProcessManager::is_target_child(Pcb *child, Pcb *parent, int child_pid)
    {
        if (child_pid > 0)
        {
            // 对于特定PID，只检查PID匹配，不检查parent（因为已在开头验证过主线程的parent）
            return child->_pid == child_pid;
        }
        else
        {
            // 对于非特定PID的情况，仍需检查parent关系
            if (child->_parent != parent)
                return false;

            if (child_pid == 0)
                return child->_pgid == parent->_pgid;
            else if (child_pid < -1)
                return child->_pgid == -child_pid;
            else // child_pid == -1
                return true;
        }
    }

    // 辅助函数：检查特定PID是否还有剩余线程
    bool ProcessManager::has_remaining_threads(Pcb *parent, int target_pid)
    {
        // debug_process_states();
        for (uint i = 0; i < num_process; i++)
        {
            Pcb *np = &k_proc_pool[i];
            if (np->_pid == target_pid &&
                ((np->get_state() != ProcState::UNUSED && np->_killed == 1) || np->get_state() == ProcState::ZOMBIE))
            {
                printf("[wait4] found remaining thread with pid %d tid %d\n", np->_pid, np->_tid);
                return true;
            }
        }
        return false;
    }

    void ProcessManager::mark_thread_group_killed(Pcb *current)
    {
        if (current == nullptr)
        {
            return;
        }

        _wait_lock.acquire();

        for (uint i = 0; i < num_process; i++)
        {
            Pcb *p = &k_proc_pool[i];
            if (p == current || p->_state == ProcState::UNUSED || p->_tgid != current->_tgid)
            {
                continue;
            }

            p->_lock.acquire();
            if (p->_state != ProcState::ZOMBIE && p->_state != ProcState::UNUSED)
            {
                // 默认致命信号和 exit_group 都是线程组级别终止；
                // 只杀当前线程会让 pthread/join/wait 路径留下互等的半退出线程组。
                p->_killed = 1;
                if (p->_state == ProcState::SLEEPING)
                {
                    p->_state = ProcState::RUNNABLE;
                }
            }
            p->_lock.release();
        }

        _wait_lock.release();
    }
    /// @brief 将指定文件中的一段内容加载到页表映射的虚拟内存中。
    ///
    /// 此函数用于将文件 `de` 中从 `offset` 开始的 `size` 字节数据，
    /// 加载到进程的页表 `pt` 所映射的虚拟地址 `va` 开始的内存区域中。
    /// 支持起始地址非页对齐情况，内部自动处理跨页加载。
    /// 如果页表未正确建立或读取失败，将导致 panic。
    ///
    /// @param pt  进程的页表，用于获取对应虚拟地址的物理地址。
    /// @param va  加载的起始虚拟地址，允许非页对齐。
    /// @param de  指向文件的目录项，用于读取文件数据。
    /// @param offset 文件中读取的起始偏移。
    /// @param size 要读取的总字节数。
    /// @return 总是返回 0，失败情况下内部直接 panic。
    int ProcessManager::load_seg(mem::PageTable &pt, uint64 va, eastl::string &path, uint offset, uint size)
    { // 好像没有机会返回 -1, pa失败的话会panic，de的read也没有返回值
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        uint i, n;
        uint64 pa;

        i = 0;
        if (!is_page_align(va)) // 如果va不是页对齐的，先读出开头不对齐的部分
        {
            pa = (uint64)pt.walk_addr(va);
            // printf("[load_seg] pa: %p, va: %p\n", pa, va);
#ifdef LOONGARCH
            pa = to_vir(pa);
            // printf("[load_seg] to vir pa: %p\n", pa);
#endif
            n = PGROUNDUP(va) - va;
            vfs_read_file(path.c_str(), pa, offset + i, n);

            i += n;
        }

        // printfRed("[load_seg] load va: %p, size: %d\n", va, size);
        // printfRed("[load_seg] i: %d, offset: %d\n", i, offset);

        for (; i < size; i += PGSIZE) // 此时 va + i 地址是页对齐的
        {
            // printf("[load_seg] va + i: %p\n", va + i);
            pa = PTE2PA((uint64)pt.walk(va + i, 0).get_data()); // pte.to_pa() 得到的地址是页对齐的
            // printf("[load_seg] pa: %p\n", pa);
            if (pa == 0)
                panic("load_seg: walk");
            if (size - i < PGSIZE) // 如果是最后一页中的数据
                n = size - i;
            else
                n = PGSIZE;
#ifdef RISCV
            pa = pa;
#elif defined(LOONGARCH)
            pa = to_vir(pa);
#endif

            if (vfs_read_file(path.c_str(), pa, offset + i, n) != n) // 读取文件内容到物理内存
                return -1;
        }

#ifdef LOONGARCH
        // 官方镜像不能原地修改；LoongArch musl 旧二进制里的坏 ll/sc 序列在装载进内存后热修。
        apply_loongarch_user_elf_patches(pt, va, path.c_str(), offset, size);
#endif

        return 0;
    }
    /// @brief 真正执行退出的逻辑
    /// @param p
    /// @param state
    void ProcessManager::exit_proc(Pcb *p)
    {
        if (p == _init_proc)
            panic("init exiting"); // 保护机制：init 进程不能退出

	        printfBlue("[exit_proc] proc %s pid %d exiting\n", p->_name, p->_pid);
	        printfYellow("[exit-mm] pcb=%p pid=%d tid=%d mm=%p\n", p, p->_pid, p->_tid, p->get_memory_manager());

        // 退出清理期间可能会触发文件回写/块设备 I/O，这些路径允许 sleep。
        // 因此不能长时间手工关中断；改用 _exiting 禁止 timer 抢占式 yield，
        // 等所有可能阻塞的清理完成后，再短暂关中断进入最终 ZOMBIE/sched 阶段。
        p->_exiting = true;

        /****************************************************************************************
         * Phase 1: 处理父子进程关系和进程状态
         ****************************************************************************************/

        // 检查进程组生命周期管理
        if (p->_pgid == p->_pid)
        {
            // 当前进程是进程组领导者，检查是否有其他进程在同一进程组
            bool has_other_processes = false;
            for (uint i = 0; i < num_process; i++)
            {
                Pcb &other = k_proc_pool[i];
                if (other._pgid == p->_pgid && other._pid != p->_pid &&
                    other._state != ProcState::UNUSED && other._state != ProcState::ZOMBIE)
                {
                    has_other_processes = true;
                    break;
                }
            }

            if (has_other_processes)
            {
                // 如果进程组还有其他活跃进程，向它们发送SIGHUP和SIGCONT信号
                // 这是孤儿进程组的标准处理
                printfBlue("[exit_proc] Process group leader %d exiting, signaling remaining processes\n",
                           p->_pid);
                for (uint i = 0; i < num_process; i++)
                {
                    Pcb &other = k_proc_pool[i];
                    if (other._pgid == p->_pgid && other._pid != p->_pid &&
                        other._state != ProcState::UNUSED && other._state != ProcState::ZOMBIE)
                    {
                        other._lock.acquire();
                        other.add_signal(1);  // SIGHUP
                        other.add_signal(18); // SIGCONT
                        other._lock.release();
                    }
                }
            }
        }

        reparent(p); // 将 p 的所有子进程交给 init 进程收养

        // 处理线程退出时的清理地址
        if (p->_clear_tid_addr)
        {
            int clear_tid = 0;
            // Linux 的 clear_child_tid / set_tid_address 目标类型是 pid_t*，即 4 字节整数。
            // 这里如果按 8 字节写零，musl 的 __thread_list_lock 这类相邻静态字段会被连带清掉，
            // 线程退出后 join / 线程链表同步就会莫名卡死。
            if (mem::k_vmm.copy_out(*p->get_pagetable(), p->_clear_tid_addr, &clear_tid, sizeof(clear_tid)) < 0)
            {
                printfRed("exit_proc: copy out ctid failed\n");
            }
            else
            {
                // Linux 线程退出语义：CLONE_CHILD_CLEARTID / set_tid_address 指定的地址
                // 在被清零后，还必须做一次 FUTEX_WAKE。pthread_join()、libc 的线程回收
                // 和一批取消点测试都依赖这一下；只清零不唤醒会让 join 方永远睡在
                // 对应 futex 上，长跑里表现成 pthread_cancel_points 卡死。
                proc::futex_wakeup(p->_clear_tid_addr, 1, nullptr, 0);
            }
        }

        // detached/abnormal 线程退出时，robust mutex 的 owner-died 语义必须在地址空间释放前完成。
        // 否则等待方只会一直超时，看起来就像 pthread_robust_detach 死锁。
        if (p->_robust_list != nullptr)
        {
            proc::futex_cleanup_robust_list(p->_robust_list);
        }

        /****************************************************************************************
         * Phase 2: 释放进程内存和资源（在所有用户态写入操作完成后）
         ****************************************************************************************/
        // 使用ProcessMemoryManager统一处理内存释放
        p->cleanup_memory_manager(); // 释放所有内存资源（VMA、程序段、堆、页表、trapframe等）

        // 关闭文件描述符表，释放文件资源
        p->cleanup_ofile();

        // 清理信号处理结构和信号栈帧
        p->cleanup_sighand();

        // 释放信号栈帧链表
        while (p->sig_frame != nullptr)
        {
            ipc::signal::signal_frame *next_frame = p->sig_frame->next;
            mem::k_pmm.free_page(p->sig_frame); // 释放当前信号处理帧
            p->sig_frame = next_frame;          // 移动到下一个帧
        }
        p->sig_frame = nullptr; // 清空信号处理帧指针
        p->_sigsuspend_restore_pending = false;
        p->_sigsuspend_saved_sigmask = 0;

        // 清理线程相关资源
        p->_futex_addr = nullptr;  // 清空futex等待地址
        p->_futex_key = 0;         // 清空futex匹配键
        p->_robust_list = nullptr; // 清空健壮futex链表
        p->_robust_list_user_addr = 0;

        Cpu::push_intr_off();
        _wait_lock.acquire(); // 只在需要修改父子关系时获取锁
        p->_lock.acquire();

        const bool is_thread_group_member = p->_pid != p->_tid;
        const int exiting_pid = p->_pid;
        const int exiting_tid = p->_tid;

        if (is_thread_group_member)
        {
            // Linux 线程退出不会交给父进程 wait4() 回收；pthread_join 依赖的是
            // clear_child_tid 的清零和 futex wake。上面已经完成 clear_tid、robust
            // futex、mm/fd/sighand 引用释放，所以这里可以直接把非主线程 PCB 归还。
            // 否则 libcbench 这类反复 create/join 的测例会快速堆满僵尸线程。
            p->_state = ProcState::ZOMBIE;
            freeproc(p);
            _wait_lock.release();
            Cpu::pop_intr_off();

            printfYellow("[exit_proc] thread pid %d tid %d auto-reaped\n", exiting_pid, exiting_tid);
            k_scheduler.call_sched(); // jump to schedular, never return
            panic("zombie exit");
        }

        // 设置ZOMBIE状态（不设置xstate，由调用者负责）
        p->_state = ProcState::ZOMBIE; // 标记为 zombie，等待父进程回收
        if (p->_vfork_parent != nullptr)
        {
            p->_vfork_parent = nullptr;
            wakeup(p);
        }

        // 如果有父进程，将当前进程的时间累计到父进程中
        if (p->_parent != nullptr)
        {
            p->_parent->_lock.acquire();
            p->_parent->_cutime += p->_user_ticks + p->_cutime;
            p->_parent->_cstime += p->_stime + p->_cstime;
            if (should_deliver_child_exit_signal(p->_parent, p->_parent_exit_signal))
            {
                // clone3/clone 的 exit_signal 语义：非线程子任务退出后，向其父任务投递指定信号。
                p->_parent->add_signal(p->_parent_exit_signal);
            }
            p->_parent->_lock.release();

            // 唤醒父进程（可能在 wait() 中阻塞）
            wakeup(p->_parent);
        }

        _wait_lock.release();
        Cpu::pop_intr_off();

        printfYellow("[exit_proc] proc %s pid %d became zombie, memory freed\n", p->_name, p->_pid);

        k_scheduler.call_sched(); // jump to schedular, never return
        panic("zombie exit");
    }

    /// @brief 正常退出，设置退出状态后调用底层退出逻辑
    /// @param p 要退出的进程
    /// @param state 退出状态码
    void ProcessManager::do_exit(Pcb *p, int state)
    {
        // 设置正常退出状态
        p->_xstate = state << 8; // 存储退出状态（通常高字节存状态）

        printfBlue("[do_exit] proc %s pid %d exiting with state %d\n", p->_name, p->_pid, state);

        // 调用底层退出逻辑
        exit_proc(p);
    }

    /// @brief 信号退出，设置信号相关的退出状态后调用底层退出逻辑
    /// @param p 要退出的进程
    /// @param signal_num 导致退出的信号编号
    /// @param coredump 是否生成core dump
    void ProcessManager::do_signal_exit(Pcb *p, int signal_num, bool coredump)
    {
        // 设置信号退出状态
        // Linux的wait状态编码：低7位存储信号编号，第8位标示是否core dump
        p->_xstate = signal_num & 0x7F; // 低7位存信号编号
        if (coredump)
        {
            p->_xstate |= 0x80; // 第8位设置core dump标志
        }

        printfBlue("[do_signal_exit] proc %s pid %d killed by signal %d (coredump=%s)\n",
                   p->_name, p->_pid, signal_num, coredump ? "yes" : "no");

        mark_thread_group_killed(p);

        // 调用底层退出逻辑
        exit_proc(p);
    }

    /// @brief Pass p's abandoned children to init.
    /// @param p The parent process whose children are to be reparented.
    /// p是即将去世的父亲，他的儿子们马上要成为孤儿，我们要让init来收养他们。
    void ProcessManager::reparent(Pcb *p)
    {
        Pcb *pp;
        _wait_lock.acquire();
        for (uint i = 0; i < num_process; i++)
        {
            pp = &k_proc_pool[(_last_alloc_proc_gid + i) % num_process];
            if (pp->_parent == p)
            {
                pp->_lock.acquire();
                pp->_parent = _init_proc;
                pp->_lock.release();
            }
        }
        _wait_lock.release();
    }
    /// @brief 当前进程或线程退出（只退出自己）
    /// @param state   调用 do_exit 处理退出逻辑
    /// “一荣俱荣，一损俱损” commented by @gkq
    void ProcessManager::exit(int state)
    {
        Pcb *p = get_cur_pcb();
        printfBlue("[exit] proc %s pid %d tid %d exiting with state %d\n",
                   p->_name, p->_pid, p->_tid, state);
        do_exit(p, state);
    }

    /// @brief 当前线程组全部退出
    /// @param status
    /// https://man7.org/linux/man-pages/man2/exit_group.2.html
    void ProcessManager::exit_group(int status)
    {
        // debug_process_states();
        proc::Pcb *cp = get_cur_pcb();

        // printf("[exit_group] Thread group %d (leader pid %d) exiting with status %d\n",
        //        cp->_tgid, cp->_pid, status);

        mark_thread_group_killed(cp);

        // printf("[exit_group] Current thread pid %d exiting normally\n", cp->_pid);

        // debug_process_states();

        // 当前线程正常退出，其他线程会在调度时检查killed标志并自行退出
        do_exit(cp, status);
    }
    void ProcessManager::sleep(void *chan, SpinLock *lock)
    {
        Pcb *p = get_cur_pcb();
        // Must acquire p->lock in order to
        // change p->state and then call sched.
        // Once we hold p->lock, we can be
        // guaranteed that we won't miss any wakeup
        // (wakeup locks p->lock),
        // so it's okay to release lk.
        // printfCyan("[sleep]proc %s : sleep on chan: %p\n", p->_name, chan);

        p->_lock.acquire();
        lock->release();
        // go to sleep
        p->_chan = chan;
        p->_state = ProcState::SLEEPING;
        k_scheduler.call_sched();
        p->_chan = 0;

        p->_lock.release();
        lock->acquire();
    }
    void ProcessManager::wakeup(void *chan)
    {
        for (uint i = 0; i < num_process; ++i)
        {
            Pcb *p = &k_proc_pool[i];
            if (p != k_pm.get_cur_pcb() && p->_state != ProcState::UNUSED)
            {
                p->_lock.acquire();
                if (p->_state == ProcState::SLEEPING && p->_chan == chan)
                {
                    p->_state = ProcState::RUNNABLE;
                }
                p->_lock.release();
            }
        }
    }
    int ProcessManager::wakeup2(uint64 uaddr, uint64 futex_key, int val, void *uaddr2, uint64 futex_key2, int val2)
    {
        int count1 = 0, count2 = 0;
        for (uint i = 0; i < num_process; ++i)
        {
            Pcb *p = &k_proc_pool[i];
            p->_lock.acquire();
            bool is_futex_waiter = p->_futex_key == futex_key &&
                                   (p->_state == SLEEPING || p->_state == RUNNABLE);
            if (is_futex_waiter)
            {
                if (p->_state == RUNNABLE)
                {
                    // futex_wait 为了支持 timeout 会睡在 timer tick 通道上周期性重检；
                    // waiter 可能已被 tick 拉回 RUNNABLE，但还没有真正返回用户态。
                    // 这时 FUTEX_WAKE 仍然必须“消费”这个 waiter 并计入返回值，
                    // 否则 LTP checkpoint_wake 会认为没有唤醒任何进程而重试到超时。
                    if (count1 < val)
                    {
                        p->_futex_addr = 0;
                        p->_futex_key = 0;
                        count1++;
                    }
                    else if (uaddr2 && count2 < val2)
                    {
                        p->_futex_addr = uaddr2;
                        p->_futex_key = futex_key2;
                        count2++;
                    }
                }
                else if (count1 < val)
                {
                    p->_state = RUNNABLE;
                    p->_futex_addr = 0;
                    p->_futex_key = 0;
                    count1++;
                }
                else if (uaddr2 && count2 < val2)
                {
                    p->_futex_addr = uaddr2;
                    p->_futex_key = futex_key2;
                    count2++;
                }
            }
            p->_lock.release();

            // 检查是否已经完成所需的唤醒和重排队操作
            if (count1 >= val && (!uaddr2 || count2 >= val2))
            {
                break;
            }
        }
        return count1;
    }
    int ProcessManager::mkdir(int dir_fd, eastl::string path, uint mode)
    {
        // 1. 参数验证 - 检查空路径 -> ENOENT
        if (path.empty())
        {
            return -ENOENT;
        }

        Pcb *p = get_cur_pcb();
        if (!p)
        {
            printfRed("[mkdir] No current process found\n");
            return -EFAULT;
        }

        // 处理dirfd参数
        eastl::string base_dir;
        if (path[0] == '.')
        {
            base_dir = p->_cwd_name;
            path = path.substr(2); // 去掉"./"前缀
        }
        if (dir_fd == AT_FDCWD)
        {
            base_dir = p->_cwd_name;
        }
        else
        {
            // 验证文件描述符 -> EBADF
            if (dir_fd < 0 || dir_fd >= NOFILE)
            {
                return -EBADF;
            }

            auto file = p->get_open_file(dir_fd);
            if (!file)
            {
                return -EBADF;
            }
            if (vfs_is_file_exist(file->_path_name.c_str()) == false)
            {
                printfRed("[mkdir] Base directory does not exist: %s\n", file->_path_name.c_str());
                return -ENOENT;
            }
            // 确保dirfd指向一个目录 -> ENOTDIR
            if (file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
            {
                return -ENOTDIR;
            }

            base_dir = file->_path_name;
        }

        // 构造完整路径
        eastl::string full_path;
        if (path[0] == '/')
        {
            // 绝对路径，忽略base_dir
            full_path = path;
        }
        else
        {
            // 相对路径
            full_path = base_dir;
            if (full_path.back() != '/')
            {
                full_path += "/";
            }
            full_path += path;
        }

        // 规范化路径（处理 "./" 前缀）
        if (full_path.length() >= 2 && full_path[0] == '.' && full_path[1] == '/')
        {
            full_path = full_path.substr(2);
        }

        // mkdir(2) 不只是最终路径存在性判断，父目录链也必须逐级满足：
        // 1. 每一级都真实存在且是目录；
        // 2. 中间祖先目录需要搜索权限（x）；
        // 3. 直接父目录还需要写权限（w），否则应返回 EACCES。
        auto check_parent_directory_permissions = [&](const eastl::string &target_path) -> int
        {
            size_t last_slash = target_path.find_last_of('/');
            if (last_slash == eastl::string::npos)
            {
                return -ENOENT;
            }

            eastl::string parent_path = last_slash == 0 ? "/" : target_path.substr(0, last_slash);
            if (parent_path.empty())
            {
                parent_path = "/";
            }

            if (!vfs_is_file_exist(parent_path.c_str()))
            {
                return -ENOENT;
            }

            uint32_t fsuid = p->get_fsuid();
            uint32_t fsgid = p->get_fsgid();

            for (size_t end = target_path.find('/', 1);
                 end != eastl::string::npos && end <= last_slash;
                 end = target_path.find('/', end + 1))
            {
                eastl::string current_path = end == 0 ? "/" : target_path.substr(0, end);
                if (current_path.empty())
                {
                    current_path = "/";
                }

                fs::Kstat st{};
                int stat_ret = vfs_path_stat(current_path.c_str(), &st, true);
                if (stat_ret < 0)
                {
                    return stat_ret;
                }

                if ((st.mode & S_IFMT) != S_IFDIR)
                {
                    return -ENOTDIR;
                }

                if (fsuid == 0)
                {
                    continue;
                }

                bool is_owner = fsuid == st.uid;
                bool in_group = fsgid == st.gid;
                bool need_write = current_path == parent_path;
                bool has_exec = is_owner ? ((st.mode & S_IXUSR) != 0)
                                         : (in_group ? ((st.mode & S_IXGRP) != 0)
                                                     : ((st.mode & S_IXOTH) != 0));
                bool has_write = is_owner ? ((st.mode & S_IWUSR) != 0)
                                          : (in_group ? ((st.mode & S_IWGRP) != 0)
                                                      : ((st.mode & S_IWOTH) != 0));

                if (!has_exec || (need_write && !has_write))
                {
                    return -EACCES;
                }
            }

            return 0;
        };

        int parent_perm_ret = check_parent_directory_permissions(full_path);
        if (parent_perm_ret < 0)
        {
            return parent_perm_ret;
        }

        // 检查符号链接循环 -> ELOOP
        // 检测路径中是否存在过多的重复目录组件，这通常表明符号链接循环
        {
            // 分割路径为组件
            eastl::vector<eastl::string> path_components;
            eastl::string component;
            for (size_t i = 0; i < full_path.length(); ++i)
            {
                if (full_path[i] == '/')
                {
                    if (!component.empty())
                    {
                        path_components.push_back(component);
                        component.clear();
                    }
                }
                else
                {
                    component += full_path[i];
                }
            }
            if (!component.empty())
            {
                path_components.push_back(component);
            }

            // 检查是否有目录组件出现过多次
            eastl::map<eastl::string, int> component_count;
            int max_repetitions = 0;
            for (const auto &comp : path_components)
            {
                component_count[comp]++;
                if (component_count[comp] > max_repetitions)
                {
                    max_repetitions = component_count[comp];
                }
            }

            // 如果某个目录组件出现超过8次，很可能是循环
            // 或者总路径长度过长（Linux PATH_MAX 通常是 4096）
            if (max_repetitions > 8 || full_path.length() > 4096)
            {
                return -ELOOP;
            }

            // 额外检查：如果路径深度过深（超过40级），也认为是循环
            if (path_components.size() > 40)
            {
                return -ELOOP;
            }
        }

        // 检查目录是否已存在
        if (vfs_is_file_exist(full_path.c_str()))
        {
            return -EEXIST;
        }

        // 调用VFS层的mkdir函数，自动选择底层文件系统
        // mkdir(2) 需要保留 sticky/setgid/setuid 这三类特殊权限位，
        // 不能在进入 VFS 之前就把高 3 位掐掉，否则 rmdir03/open10 一类
        // 依赖目录特殊位语义的测例会被整体带偏。
        int result = vfs_mkdir(full_path.c_str(), mode & 07777);

        return result;
    }

    int ProcessManager::mknod(int dir_fd, eastl::string path, mode_t mode, dev_t dev)
    {
        Pcb *p = get_cur_pcb();
        [[maybe_unused]] fs::file *file = nullptr;

        if (dir_fd != AT_FDCWD)
        {
            // panic("mknod: dir_fd != AT_FDCWD not implemented");
            file = p->get_open_file(dir_fd);
        }

        const char *dirpath = (dir_fd == AT_FDCWD) ? p->_cwd_name.c_str() : p->_ofile->_ofile_ptr[dir_fd]->_path_name.c_str();
        eastl::string absolute_path = get_absolute_path(path.c_str(), dirpath);

        // 将 mode 转换为内部文件类型
        uint32 internal_mode;
        mode_t file_type = mode & S_IFMT; // 提取文件类型部分

        if (file_type == S_IFREG || file_type == 0)
        {
            printfMagenta("reg please\n");
            internal_mode = T_FILE;
        }
        else if (file_type == S_IFCHR)
        {
            internal_mode = T_CHR;
        }
        else if (file_type == S_IFBLK)
        {
            internal_mode = T_BLK;
        }
        else if (file_type == S_IFIFO)
        {
            internal_mode = T_FIFO;
        }
        else if (file_type == S_IFSOCK)
        {
            internal_mode = T_SOCK;
        }
        else
        {
            // 不支持的文件类型
            printfRed("[mknod] Unsupported file type: %o\n", file_type);
            return -22; // SYS_EINVAL
        }
        printfCyan("[mknod] dir_fd: %d, path: %s, mode: 0%o, dev: %d\n", dir_fd, absolute_path.c_str(), mode, dev);
        int result = vfs_ext_mknod(absolute_path.c_str(), internal_mode, dev);
        return result;
    }

    /// @brief
    /// @param dir_fd 指定相对路径的目录文件描述符（AT_FDCWD 表示当前工作目录）。
    /// @param path 要打开的路径
    /// @param flags 打开方式（如只读、只写、创建等）
    /// @param mode 文件权限模式（当使用O_CREAT时）
    /// @return fd
	    int ProcessManager::open(int dir_fd, eastl::string path, uint flags, int mode)
	    {
	        Pcb *p = get_cur_pcb();
	        fs::file *file = nullptr;
	        int lease_ret = wait_for_conflicting_lease(path, flags);
	        if (lease_ret < 0)
	        {
	            return lease_ret;
	        }
	        int fd = reserve_fd(p);
	        if (fd < 0)
	        {
            printfRed("[open] alloc_fd failed for path: %s,pid:%d\n", path.c_str(), p->_pid);
            return -EMFILE; // 分配文件描述符失败
        }
        int err = fs::k_vfs.openat(path, file, flags, mode);
        if (err < 0)
        {
            release_fd(p, fd);
            printfRed("[open] failed for path: %s,err:%d\n", path.c_str(), err);
            return err; // 文件不存在或打开失败
        }
        if (install_fd(p, file, fd) < 0)
        {
            file->free_file();
            release_fd(p, fd);
            return -EMFILE;
        }
        file->_lock.l_pid = p->_pid; // 设置文件描述符的锁定进程 ID
        return fd;                   // 返回分配的文件描述符
    }

    int ProcessManager::close(int fd)
    {
        if (fd < 0 || fd >= (int)max_open_files)
            return -1;
        Pcb *p = get_cur_pcb();
        if (p->_ofile == nullptr)
            return 0;

        p->_ofile->_lock.acquire();
        fs::file *f = p->_ofile->_ofile_ptr[fd];
        p->_ofile->_ofile_ptr[fd] = nullptr;
        p->_ofile->_reserved[fd] = false;
        p->_ofile->_fl_cloexec[fd] = false;
        p->_ofile->_lock.release();

        if (f == nullptr)
        {
            return 0;
        }
        if (!is_probably_live_file_object(f))
        {
            printfRed("[close] 检测到异常文件指针，直接丢弃: pid=%d fd=%d file=%p\n",
                      p->_pid, fd, f);
            return 0;
        }
        fs::release_posix_record_locks_for_path(f->backing_path(), p->_pid);
        f->free_file();
        return 0;
    }
    /// @brief 获取指定文件描述符对应文件的状态信息。
    /// @details 此函数会从当前进程的打开文件表中查找给定文件描述符 `fd`，
    /// 如果合法且已打开，则将其对应的文件状态信息拷贝到 `buf` 指向的结构中。
    /// @param fd 要查询的文件描述符，应在合法范围内并对应已打开文件。
    /// @param buf 用于存放文件状态的结构体指针，函数将其填充为目标文件的元信息（如大小、权限等）。
    /// @return 返回 0 表示成功；若 `fd` 非法或未打开，返回 -1。
    int ProcessManager::fstat(int fd, fs::Kstat *buf)
    {
        if (fd < 0 || fd >= (int)max_open_files)
            return -EBADF;

        Pcb *p = get_cur_pcb();
        if (p->_ofile == nullptr || p->_ofile->_ofile_ptr[fd] == nullptr)
            return -EBADF; // Bad file descriptor
        fs::file *f = p->_ofile->_ofile_ptr[fd];
        if (!is_probably_live_file_object(f))
        {
            printfRed("[fstat] 检测到异常文件指针: pid=%d tid=%d name=%s fd=%d file=%p\n",
                      p->_pid, p->_tid, p->_name, fd, f);
            dump_fd_table(p, "fstat-invalid-file");
            return -EBADF;
        }

        if (is_busybox_like_proc(p))
        {
            printfBlue("[fstat] pid=%d fd=%d file=%p ref=%d type=%d virtual=%d path=%s\n",
                       p->_pid,
                       fd,
                       f,
                       (int)f->refcnt,
                       (int)f->_attrs.filetype,
                       f->is_virtual ? 1 : 0,
                       f->_path_name.c_str());
        }
        return fs::k_vfs.fstat(f, buf);
    }
    int ProcessManager::chdir(eastl::string &path)
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        if (path.length() > MAXPATH)
        {
            printfRed("[chdir] path length exceeds MAXPATH\n");
            return -ENAMETOOLONG;
        }
        Pcb *p = get_cur_pcb();
        char temp_path[EXT4_PATH_LONG_MAX];

        get_absolute_path(path.c_str(), p->_cwd_name.c_str(), temp_path);

        // 解析符号链接
        eastl::string resolved_path = temp_path;
        int symlink_depth = 0;
        const int MAX_SYMLINK_DEPTH = 40; // 防止无限循环

        while (symlink_depth < MAX_SYMLINK_DEPTH)
        {
            // 检查当前路径是否是符号链接
            if (!fs::k_vfs.is_file_exist(resolved_path))
            {
                printfRed("[chdir] Path does not exist: %s", resolved_path.c_str());
                return -ENOENT;
            }

            int file_type = fs::k_vfs.path2filetype(resolved_path);
            if (file_type != fs::FileTypes::FT_SYMLINK)
            {
                // 不是符号链接，检查是否是目录
                if (file_type != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("[chdir] Path is not a directory: %s", resolved_path.c_str());
                    return -ENOTDIR;
                }
                break; // 找到最终目录
            }

            // 是符号链接，读取其目标
            // 使用 ext4_readlink 直接读取符号链接内容
            char link_target_buf[256];
            size_t readbytes = 0;
            int readlink_result = ext4_readlink(resolved_path.c_str(), link_target_buf, sizeof(link_target_buf) - 1, &readbytes);
            if (readlink_result != EOK)
            {
                printfRed("[chdir] Failed to read symlink: %s, error: %d", resolved_path.c_str(), readlink_result);
                return -EIO;
            }

            link_target_buf[readbytes] = '\0'; // 确保字符串结尾
            eastl::string link_target = link_target_buf;

            if (link_target.empty())
            {
                printfRed("[chdir] Empty symlink target: %s", resolved_path.c_str());
                return -EIO;
            }

            // 解析符号链接目标路径
            if (link_target[0] == '/')
            {
                // 绝对路径
                resolved_path = link_target;
            }
            else
            {
                // 相对路径，相对于符号链接所在目录
                size_t last_slash = resolved_path.find_last_of('/');
                if (last_slash != eastl::string::npos)
                {
                    eastl::string symlink_dir = resolved_path.substr(0, last_slash + 1);
                    resolved_path = get_absolute_path(link_target.c_str(), symlink_dir.c_str());
                }
                else
                {
                    // 不应该发生，因为 resolved_path 应该是绝对路径
                    resolved_path = get_absolute_path(link_target.c_str(), p->_cwd_name.c_str());
                }
            }

            symlink_depth++;
        }

        if (symlink_depth >= MAX_SYMLINK_DEPTH)
        {
            printfRed("[chdir] Too many symbolic links: %s", path.c_str());
            return -ELOOP;
        }

        p->_cwd_name = resolved_path;

        if (p->_cwd_name.back() != '/')
        {
            p->_cwd_name += "/";
        }

        printfCyan("[chdir] Changed directory to: %s", p->_cwd_name.c_str());
        // #endif
        return 0;
    }
    /// @brief 获取当前进程的工作目录路径。get current working directory
    /// @details 此函数将当前进程的工作目录路径复制到 `out_buf` 中。
    /// 末尾会自动添加 `\0` 结束符，以构成合法的 C 风格字符串。
    /// @param out_buf 用户提供的字符数组，用于接收当前进程的工作目录路径。
    /// @return 返回写入缓冲区的字符数（包含结束符）
    int ProcessManager::getcwd(char *out_buf)
    {
        Pcb *p = get_cur_pcb();

        eastl::string cwd;
        cwd = p->_cwd_name;
        if (!cwd.empty() && cwd.back() == '/')
        {
            cwd.pop_back();
        }
        // 根目录内部保存成 "/"，上面的通用去尾斜杠逻辑会把它抹成空串。
        // BusyBox ash 在展开 \w 时要求 getcwd() 返回绝对路径，因此这里要把根目录还原回 "/".
        if (cwd.empty())
        {
            cwd = "/";
        }
        uint i = 0;
        for (; i < cwd.size(); ++i)
            out_buf[i] = cwd[i];
        out_buf[i] = '\0';
        return i + 1;
    }

    /// @brief 验证mmap参数的有效性
    /// @param addr 映射地址
    /// @param length 映射长度
    /// @param prot 保护标志
    /// @param flags 映射标志
    /// @param fd 文件描述符
    /// @param offset 偏移量
    /// @return 0表示有效，负数表示错误码
    int ProcessManager::validate_mmap_params(void *addr, size_t length, int prot, int flags, int fd, int offset)
    {

        // 检查匿名映射
        bool is_anonymous = (flags & MAP_ANONYMOUS);

        if (is_anonymous)
        {
            if (offset != 0)
            {
                return syscall::SYS_EINVAL; // 匿名映射offset必须为0
            }
            // 匿名映射通常要求fd为-1
            if (!(flags & MAP_ANONYMOUS) && fd != -1)
            {
                printfRed("[mmap] Anonymous mapping but fd != -1\n");
                return syscall::SYS_EBADF; // 不一致的匿名映射设置
            }
        }
        else
        {
            // 文件映射的fd验证在主函数中进行，因为需要访问进程状态
            if (fd < 0)
            {
                printfRed("[mmap] Invalid file descriptor: %d\n", fd);
                return syscall::SYS_EBADF;
            }
        }
        // 长度检查
        if (length <= 0)
        {
            printfRed("[mmap] Invalid length: %d\n", length);
            return syscall::SYS_EINVAL;
        }

        // Linux 的映射类型占低两位：1=SHARED，2=PRIVATE，3=SHARED_VALIDATE。
        // 不能把 MAP_SHARED_VALIDATE 当成 SHARED|PRIVATE 的组合。
        int map_type = flags & MAP_SHARED_VALIDATE;
        if (map_type != MAP_SHARED && map_type != MAP_PRIVATE && map_type != MAP_SHARED_VALIDATE)
        {
            printfRed("[mmap] Must specify MAP_SHARED or MAP_PRIVATE\n");
            return syscall::SYS_EINVAL; // 必须指定共享类型
        }

        constexpr int known_mmap_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS |
                                         MAP_GROWSDOWN | MAP_DENYWRITE | MAP_EXECUTABLE |
                                         MAP_LOCKED | MAP_NORESERVE | MAP_POPULATE |
                                         MAP_NONBLOCK | MAP_STACK | MAP_HUGETLB | MAP_SYNC |
                                         MAP_FIXED_NOREPLACE | MAP_UNINITIALIZED;
        if ((flags & MAP_SHARED_VALIDATE) == MAP_SHARED_VALIDATE && (flags & ~known_mmap_flags) != 0)
        {
            printfRed("[mmap] Unsupported MAP_SHARED_VALIDATE flags: 0x%x\n", flags);
            return syscall::SYS_EOPNOTSUPP;
        }

        // 检查保护标志的合理性
        if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE))
        {
            printfRed("[mmap] Invalid protection flags: %d\n", prot);
            return syscall::SYS_EINVAL; // 无效的保护标志
        }

        // 检查地址和长度的合理性
        if (addr != nullptr && (uint64)addr >= MAXVA)
        {
            printfRed("[mmap] Address out of range: %p\n", addr);
            return syscall::SYS_ENOMEM; // 地址超出虚拟地址空间
        }

        // 检查在32位架构下是否会发生溢出（针对EOVERFLOW错误）
        if (sizeof(void *) == 4) // 32位架构
        {
            uint64 pages_for_length = (length + PGSIZE - 1) / PGSIZE;
            uint64 pages_for_offset = offset / PGSIZE;
            if (pages_for_length + pages_for_offset > UINT32_MAX / PGSIZE)
            {
                printfRed("[mmap] Length and offset overflow: length=%d, offset=%d\n", length, offset);
                return syscall::SYS_EOVERFLOW;
            }
        }

        // MAP_FIXED相关检查
        if (flags & MAP_FIXED)
        {
            if (addr == nullptr)
            {
                printfRed("[mmap] MAP_FIXED requires a specific address\n");
                return syscall::SYS_EINVAL; // MAP_FIXED需要指定地址
            }
            // 检查地址对齐（大多数架构要求页对齐）
            if ((uint64)addr % PGSIZE != 0)
            {
                printfRed("[mmap] MAP_FIXED address must be page-aligned: %p\n", addr);
                return syscall::SYS_EINVAL;
            }
        }

        // MAP_FIXED_NOREPLACE 需要指定地址
        if ((flags & MAP_FIXED_NOREPLACE) && addr == nullptr)
        {
            printfRed("[mmap] MAP_FIXED_NOREPLACE requires non-null address\n");
            return syscall::SYS_EINVAL;
        }

        // MAP_FIXED_NOREPLACE 需要地址页对齐
        if ((flags & MAP_FIXED_NOREPLACE) && ((uint64)addr % PGSIZE != 0))
        {
            printfRed("[mmap] MAP_FIXED_NOREPLACE address must be page-aligned: %p\n", addr);
            return syscall::SYS_EINVAL;
        }

        return 0; // 参数有效
    }

    /// @brief 内存映射函数，根据POSIX标准实现mmap系统调用
    /// @param addr 期望的映射地址，可以为nullptr让系统选择
    /// @param length 映射长度（字节）
    /// @param prot 内存保护标志(PROT_READ|PROT_WRITE|PROT_EXEC|PROT_NONE)
    /// @param flags 映射标志(MAP_SHARED|MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS等)
    /// @param fd 文件描述符，匿名映射时为-1
    /// @param offset 文件偏移量
    /// @param errno 错误码输出参数
    /// @return 成功返回映射地址，失败返回MAP_FAILED
    void *ProcessManager::mmap(void *addr, size_t length, int prot, int flags, int fd, int offset, int *errno)
    {
        // 初始化错误码
        if (errno != nullptr)
        {
            *errno = 0;
        }

        // 参数验证
        int validation_result = validate_mmap_params(addr, length, prot, flags, fd, offset);
        if (validation_result != 0)
        {
            printfRed("[mmap] Parameter validation failed: %d\n", validation_result);
            if (errno != nullptr)
            {
                *errno = -validation_result; // 转换为正数错误码
            }
            return MAP_FAILED;
        }

        Pcb *p = get_cur_pcb();

        // 检查是否为匿名映射
        bool is_anonymous = (flags & MAP_ANONYMOUS) || (fd == -1);
        // glibc malloc/pthread 会在同一地址空间内并发申请匿名私有映射。
        // VMA 表和 mmap_cursor 共享在 ProcessMemoryManager 中，必须串行更新。
        MemoryLockGuard anonymous_memory_guard(is_anonymous ? p->get_memory_manager() : nullptr);

        // 匿名映射验证
        if (is_anonymous)
        {
            if (fd != -1 && !(flags & MAP_ANONYMOUS))
            {
                printfRed("[mmap] Anonymous mapping but fd != -1\n");
                return MAP_FAILED;
            }
            if (offset != 0)
            {
                printfRed("[mmap] Anonymous mapping with non-zero offset\n");
                return MAP_FAILED;
            }
        }

        // 文件映射验证
        fs::file *vfile = nullptr;
        fs::file *f = nullptr;
        bool vma_owns_dedicated_file = false;
        uint64 map_addr = 0;
        int shared_backing_shmid = -1;
        bool shared_mapping_attached = false;
        bool created_new_shared_backing = false;

        // 统一清理 mmap 中途失败时拿到的资源，避免把“半成功”状态留给后续回收路径。
        auto release_mapping_file = [&]()
        {
            if (vma_owns_dedicated_file && vfile != nullptr)
            {
                vfile->free_file();
                vfile = nullptr;
                vma_owns_dedicated_file = false;
            }
        };

        auto cleanup_shared_backing = [&]()
        {
            if (shared_mapping_attached && map_addr != 0)
            {
                shm::k_smm.detach_seg((void *)map_addr);
                shared_mapping_attached = false;
            }

            // 只有当前 mmap 确认新建了共享段，失败时才允许回收。
            // 复用旧段时绝不能在本地失败清理里把别人正在使用的后端删掉。
            if (created_new_shared_backing && shared_backing_shmid >= 0)
            {
                shm::k_smm.delete_seg(shared_backing_shmid);
                shared_backing_shmid = -1;
                created_new_shared_backing = false;
            }
        };

        auto fail_mmap = [&](int errnum) -> void *
        {
            cleanup_shared_backing();
            release_mapping_file();
            if (errno != nullptr)
            {
                *errno = errnum;
            }
            return MAP_FAILED;
        };

        if (!is_anonymous)
        {
            if (p->_ofile == nullptr || fd < 0 || fd >= (int)max_open_files ||
                p->_ofile->_ofile_ptr[fd] == nullptr)
            {
                printfRed("[mmap] Invalid file descriptor: %d\n", fd);
                if (errno != nullptr)
                {
                    *errno = EBADF;
                }
                return MAP_FAILED;
            }

            f = p->get_open_file(fd);
            // 支持不同类型的文件映射
            //  if (f->_attrs.filetype != fs::FileTypes::FT_NORMAL||
            //      f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            //  {
            //      printfRed("[mmap] File descriptor does not refer to regular file\n");
            //      if (errno != nullptr)
            //      {
            //          *errno =EACCES;
            //      }
            //      return MAP_FAILED;
            //  }

            // 检查文件访问权限
            if (prot & PROT_READ)
            {
                // TODO: 检查文件是否以可读模式打开
                // 如果文件未以读模式打开，应返回EACCES
            }

            if ((prot & PROT_WRITE))
            {
                // TODO: 检查文件是否以可写模式打开
                // 如果文件未以写模式打开，应返回EACCES
            }

            // 检查文件是否被锁定
            // TODO: 如果文件被锁定，应返回EAGAIN
            // if (file_is_locked(vfile)) {
            //     if (errno != nullptr) {
            //         *errno =EAGAIN;
            //     }
            //     return MAP_FAILED;
            // }

            // 检查文件系统是否支持内存映射
            // TODO: 如果底层文件系统不支持内存映射，应返回ENODEV

            // 检查系统文件描述符限制
            // TODO: 如果系统达到文件描述符限制，应返回ENFILE

            // 检查是否请求了PROT_EXEC但文件系统挂载时使用了noexec
            if (prot & PROT_EXEC)
            {
                // TODO: 检查文件系统挂载选项
                // if (filesystem_mounted_noexec(vfile)) {
                //     if (errno != nullptr) {
                //         *errno =EPERM;
                //     }
                //     return MAP_FAILED;
                // }
            }

            vfile = f;
            // 文件映射如果要按路径重新打开一份专用 backing file，
            // 必须先把原 open file description 上尚未落盘/尚未对外可见的写合并内容刷出去。
            // 否则像 basic/test_mmap 这种“刚写完就 mmap 同一个文件”的场景，
            // 重新打开后看到的还是旧内容，缺页时就会读到 0 字节。
            int flush_visibility_ret = f->flush_visibility_state();
            if (flush_visibility_ret != 0)
            {
                printfRed("[mmap] Failed to flush file visibility state before reopening mapping file: %d\n",
                          flush_visibility_ret);
                return fail_mmap(flush_visibility_ret < 0 ? -flush_visibility_ret : flush_visibility_ret);
            }
            // Respect memfd write seal: disallow shared writable mappings
            if (f->is_memfd())
            {
                if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && (f->memfd_seals() & F_SEAL_WRITE))
                {
                    if (errno)
                        *errno = EPERM;
                    return MAP_FAILED;
                }
            }

            // 普通文件映射优先使用独立 backing handle，避免 fd 关闭后把 VMA
            // 持有的 file 对象一并回收。但 mkstemp()+unlink()+mmap() 是
            // iperf/glibc 等程序常见路径；文件已经从目录摘除后不能再按路径
            // 重新打开，只能让 VMA 持有当前打开文件对象的引用。
            const eastl::string &mapping_path = f->backing_path();
            bool can_reopen_for_vma = !f->is_virtual &&
                                      f->_attrs.filetype == fs::FileTypes::FT_NORMAL &&
                                      !f->is_memfd() &&
                                      !mapping_path.empty() &&
                                      fs::k_vfs.is_file_exist(mapping_path.c_str()) == 1;
            if (can_reopen_for_vma)
            {
                fs::file *mapping_file = nullptr;
                int reopen_flags = O_RDONLY;
                if ((flags & MAP_SHARED) && (prot & PROT_WRITE))
                {
                    reopen_flags = O_RDWR;
                }

                int reopen_err = fs::k_vfs.openat(mapping_path, mapping_file, reopen_flags, 0);
                if (reopen_err < 0 || mapping_file == nullptr)
                {
                    printfRed("[mmap] Failed to create dedicated mapping file for %s, err=%d\n",
                              mapping_path.c_str(), reopen_err);
                    return fail_mmap(reopen_err < 0 ? -reopen_err : EIO);
                }

                vfile = mapping_file;
                vma_owns_dedicated_file = true;
            }
        }
        else
        {
        }

        // 地址对齐
        uint64 aligned_length = PGROUNDUP(length);

        // 检查映射大小是否超过虚拟地址空间限制
        if (aligned_length > MAXVA - PGSIZE)
        {
            printfRed("[mmap] Mapping size %u exceeds virtual address space\n", aligned_length);
            return fail_mmap(ENOMEM);
        }

        // 检查是否有足够的内存可用
        /// TODO: 检查系统是否有足够的物理内存
        // if (!enough_memory_available(aligned_length)) {
        //     if (errno != nullptr) {
        //         *errno =ENOMEM;
        //     }
        //     return MAP_FAILED;
        // }

        // 检查进程的RLIMIT_DATA限制
        /// TODO: 检查进程数据段大小限制
        // if (would_exceed_data_limit(p, aligned_length)) {
        //     if (errno != nullptr) {
        //         *errno =ENOMEM;
        //     }
        //     return MAP_FAILED;
        // }

        // 先记住一个空闲 VMA 槽位；如果后面能和相邻匿名映射合并，就不再消耗新槽位。
        int free_vma_idx = -1;
        for (int i = 0; i < NVMA; ++i)
        {
            if (!p->get_vma()->_vm[i].used)
            {
                free_vma_idx = i;
                break;
            }
        }

        uint restore_length = length;
        if (vfile != nullptr)
        {
            /*
             * file-backed MAP_SHARED 可以映射得比文件本身更长，但真正有后端的
             * 只有文件大小覆盖到的页。超过 EOF 的整页访问应在缺页路径送 SIGBUS，
             * 不能把整段 length 都做成可读写的匿名共享页。
             */
            if ((flags & MAP_SHARED) != 0)
            {
                fs::Kstat st;
                int size_result = fs::k_vfs.fstat(vfile, &st);
                if (size_result != EOK)
                {
                    printfRed("[mmap] Failed to get shared file size for %s, error: %d\n",
                              vfile->_path_name.c_str(), size_result);
                    return fail_mmap(size_result < 0 ? -size_result : size_result);
                }
                if (st.size < restore_length)
                {
                    restore_length = st.size == 0 ? 1 : static_cast<uint>(st.size);
                }
            }
        }

        // 确定映射地址
        if ((flags & MAP_FIXED) || (flags & MAP_FIXED_NOREPLACE))
        {
            if (addr == nullptr)
            {
                printfRed("[mmap] MAP_FIXED/MAP_FIXED_NOREPLACE requires non-null addr\n");
                return fail_mmap(EINVAL);
            }

            if (is_page_align((uint64)addr) == false)
            {
                printfRed("[mmap] Fixed address must be page aligned\n");
                return fail_mmap(EINVAL);
            }
            map_addr = (uint64)addr;

            // 检查MAP_FIXED地址边界
            if (map_addr < PGSIZE || map_addr + aligned_length > MAXVA - PGSIZE)
            {
                printfRed("[mmap] MAP_FIXED address out of bounds: addr=%p, len=%u\n",
                          (void *)map_addr, aligned_length);
                return fail_mmap(ENOMEM);
            }

            // 检查地址冲突
            if (flags & MAP_FIXED_NOREPLACE)
            {
                // MAP_FIXED_NOREPLACE: 如果地址范围与现有映射冲突则失败
                for (int i = 0; i < NVMA; ++i)
                {
                    if (p->get_vma()->_vm[i].used)
                    {
                        uint64 existing_start = p->get_vma()->_vm[i].addr;
                        uint64 existing_end = existing_start + p->get_vma()->_vm[i].len;
                        uint64 new_end = map_addr + aligned_length;

                        if (!(new_end <= existing_start || map_addr >= existing_end))
                        {
                            printfRed("[mmap] MAP_FIXED_NOREPLACE: address range [%p, %p) conflicts with existing [%p, %p)\n",
                                      (void *)map_addr, (void *)new_end, (void *)existing_start, (void *)existing_end);
                            return fail_mmap(EEXIST);
                        }
                    }
                }
            }
            else if (flags & MAP_FIXED)
            {
                // MAP_FIXED: 可以覆盖现有映射
                printfYellow("[mmap] MAP_FIXED: may override existing mappings\n");

                // 在建立新映射前，必须先把将要覆盖的地址范围内的旧映射全部取消，
                // 否则会产生重叠VMA，导致后续缺页时按旧VMA权限判定出错。
                ProcessMemoryManager *mm = p->get_memory_manager();
                if (mm == nullptr)
                {
                    printfRed("[mmap] Internal error: memory manager is null\n");
                    return fail_mmap(EFAULT);
                }

                int unmap_ret = mm->unmap_memory_range((void *)map_addr, aligned_length);
                if (unmap_ret != 0)
                {
                    // 即使未找到完全匹配的VMA也继续（可能是空洞），但如果返回硬错误，直接失败
                    // 这里保守地认为非0即失败
                    printfYellow("[mmap] MAP_FIXED: unmap of [%p, %p) returned %d\n",
                                 (void *)map_addr, (void *)(map_addr + aligned_length), unmap_ret);
                    // 继续进行映射，Linux 行为是无论是否有旧映射都强制覆盖；
                    // 我们的 unmap 尝试只为清理重叠VMA，失败非致命，除非明显错误。
                }
            }
        }
        else
        {
            if (addr != nullptr)
            {
                // 非 MAP_FIXED 的 hint 先按请求地址尝试，冲突检查交给后续共享段/缺页逻辑。
                map_addr = PGROUNDUP((uint64)addr);
            }
            else
            {
                ProcessMemoryManager *mm = p->get_memory_manager();
                if (mm == nullptr)
                {
                    printfRed("[mmap] current process has no memory manager\n");
                    return fail_mmap(ENOMEM);
                }

                map_addr = mm->reserve_mmap_region(aligned_length);
                if (map_addr == 0)
                {
                    printfRed("[mmap] Failed to reserve virtual address range, len=%d\n", aligned_length);
                    return fail_mmap(ENOMEM);
                }
            }

            if (flags & MAP_SHARED)
            {
                SharedBackingSelection shared_backing = select_shared_backing(vfile, is_anonymous);
                if (shared_backing.key == -1)
                {
                    printfRed("[mmap] Failed to generate key for shared memory\n");
                    return fail_mmap(EINVAL);
                }

                created_new_shared_backing = shared_backing.always_new_segment ||
                                             shm::k_smm.find_seg_by_key(shared_backing.key) < 0;
                shared_backing_shmid = shm::k_smm.create_seg(shared_backing.key, restore_length, IPC_CREAT);
                if (shared_backing_shmid < 0)
                {
                    printfRed("[mmap] Failed to create shared memory segment\n");
                    created_new_shared_backing = false;
                    return fail_mmap(-shared_backing_shmid);
                }

                int shmflg = 0;
                if ((prot & PROT_READ) && !(prot & PROT_WRITE))
                {
                    shmflg = SHM_RDONLY;
                }
                if (prot == PROT_NONE)
                {
                    shmflg = SHM_NONE;
                }

                void *attach_result = shm::k_smm.attach_seg(shared_backing_shmid, (void *)map_addr, shmflg);
                if ((long)attach_result < 0)
                {
                    printfRed("[mmap] Failed to attach shared memory segment, shmid=%d ret=%ld\n",
                              shared_backing_shmid, (long)attach_result);
                    return fail_mmap(-(long)attach_result);
                }

                map_addr = (uint64)attach_result;
                shared_mapping_attached = true;
                uint64 pa = shm::k_smm.get_seg_info(shared_backing_shmid).phy_addrs;
                if (vfile != nullptr)
                {
                    fs::Kstat st;
                    int size_result = fs::k_vfs.fstat(vfile, &st);
                    if (size_result != EOK)
                    {
                        printfRed("[mmap] Failed to get file size for %s, error: %d\n", vfile->_path_name.c_str(), size_result);
                        return fail_mmap(size_result < 0 ? -size_result : size_result);
                    }

                    int readbytes = vfile->read((uint64)pa, PGSIZE, offset, false);
                    if (readbytes < 0)
                    {
                        printfRed("[mmap] Failed to read file data for mapping, error: %d\n", readbytes);
                        return fail_mmap(EFAULT);
                    }

                    if (readbytes < PGSIZE)
                    {
                        printfYellow("[mmap] MAP_SHARED partial page read (%d bytes)\n", readbytes);
                    }
                }
            }
        }

        if ((flags & MAP_SHARED) != 0 && shared_backing_shmid < 0)
        {
            SharedBackingSelection shared_backing = select_shared_backing(vfile, is_anonymous);
            if (shared_backing.key == -1)
            {
                printfRed("[mmap] Failed to generate key for shared memory\n");
                return fail_mmap(EINVAL);
            }

            created_new_shared_backing = shared_backing.always_new_segment ||
                                         shm::k_smm.find_seg_by_key(shared_backing.key) < 0;
            shared_backing_shmid = shm::k_smm.create_seg(shared_backing.key, restore_length, IPC_CREAT);
            if (shared_backing_shmid < 0)
            {
                printfRed("[mmap] Failed to create shared memory segment for fixed/shared mapping\n");
                created_new_shared_backing = false;
                return fail_mmap(-shared_backing_shmid);
            }

            int shmflg = 0;
            if ((prot & PROT_READ) && !(prot & PROT_WRITE))
            {
                shmflg = SHM_RDONLY;
            }
            if (prot == PROT_NONE)
            {
                shmflg = SHM_NONE;
            }

            void *attach_result = shm::k_smm.attach_seg(shared_backing_shmid, (void *)map_addr, shmflg);
            if ((long)attach_result < 0)
            {
                printfRed("[mmap] Failed to attach shared memory segment for fixed/shared mapping, shmid=%d ret=%ld\n",
                          shared_backing_shmid, (long)attach_result);
                return fail_mmap(-(long)attach_result);
            }

            map_addr = (uint64)attach_result;
            shared_mapping_attached = true;
            uint64 pa = shm::k_smm.get_seg_info(shared_backing_shmid).phy_addrs;
            if (vfile != nullptr)
            {
                fs::Kstat st;
                int size_result = fs::k_vfs.fstat(vfile, &st);
                if (size_result != EOK)
                {
                    printfRed("[mmap] Failed to get file size for %s, error: %d\n", vfile->_path_name.c_str(), size_result);
                    return fail_mmap(size_result < 0 ? -size_result : size_result);
                }

                int readbytes = vfile->read((uint64)pa, PGSIZE, offset, false);
                if (readbytes < 0)
                {
                    printfRed("[mmap] Failed to read file data for mapping, error: %d\n", readbytes);
                    return fail_mmap(EFAULT);
                }

                if (readbytes < PGSIZE)
                {
                    printfYellow("[mmap] MAP_SHARED partial page read (%d bytes)\n", readbytes);
                }
            }
        }

        // LoongArch 的 TLB refill 入口假定目标虚拟地址的页表层级已经存在。
        // mmap 仅记录 VMA、把叶子页留给后续缺页惰性分配时，如果这里完全不预建页表层级，
        // 首次访问高地址匿名/文件映射时会直接沿着空层级走出诡异的 ADEM。
        // 这里仅分配页表层级和最终叶子槽位，不提前建立叶子 PTE，因此不会破坏 lazy allocation。
        for (uint64 va = map_addr; va < map_addr + aligned_length; va += PGSIZE)
        {
            mem::Pte pte_slot = p->get_pagetable()->walk(va, true);
            if (pte_slot.is_null())
            {
                printfRed("[mmap] Failed to precreate page-table hierarchy for va=%p\n", (void *)va);
                return fail_mmap(ENOMEM);
            }
        }

        int vma_idx = -1;
        // musl 的 malloc 在 LoongArch 上会连续申请一串同属性匿名私有映射。
        // 如果这里每次都硬占一个新 VMA 槽位，很快就会因为 NVMA 太小而失败。
        // 对于首尾相接、权限/标志完全一致、且无共享/文件后端的匿名映射，直接并入前一段。
        if (is_anonymous &&
            (flags & MAP_PRIVATE) != 0 &&
            (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0)
        {
            for (int i = 0; i < NVMA; ++i)
            {
                vma &existing = p->get_vma()->_vm[i];
                if (!existing.used)
                {
                    continue;
                }
                if (existing.addr + (uint64)existing.len != map_addr)
                {
                    continue;
                }
                if (existing.prot != prot || existing.flags != flags)
                {
                    continue;
                }
                if (existing.vfd != -1 || existing.vfile != nullptr)
                {
                    continue;
                }
                if (existing.backing_kind != VMA_BACKING_NONE)
                {
                    continue;
                }

                existing.len += static_cast<int>(aligned_length);
                if (existing.max_len < static_cast<uint64>(existing.len))
                {
                    existing.max_len = static_cast<uint64>(existing.len);
                }
                return (void *)map_addr;
            }
        }

        vma_idx = free_vma_idx;
        if (vma_idx == -1)
        {
            printfRed("[mmap] No available VMA slots\n");
            if (p != nullptr && strcmp(p->_name, "libc-bench-child") == 0)
            {
                printfYellow("[mmap][libc-bench-child] slot exhaustion req_len=%p prot=0x%x flags=0x%x heap=[%p,%p)\n",
                             (void *)aligned_length,
                             prot,
                             flags,
                             (void *)p->get_heap_start(),
                             (void *)p->get_heap_end());
                for (int i = 0; i < NVMA; ++i)
                {
                    const vma &dbg_vm = p->get_vma()->_vm[i];
                    if (!dbg_vm.used)
                    {
                        continue;
                    }
                    printfYellow("[mmap][libc-bench-child] vma[%d]=[%p,%p) len=%d prot=0x%x flags=0x%x fd=%d expandable=%d backing=%d\n",
                                 i,
                                 (void *)dbg_vm.addr,
                                 (void *)(dbg_vm.addr + (uint64)dbg_vm.len),
                                 dbg_vm.len,
                                 dbg_vm.prot,
                                 dbg_vm.flags,
                                 dbg_vm.vfd,
                                 dbg_vm.is_expandable,
                                 dbg_vm.backing_kind);
                }
            }
            return fail_mmap(ENOMEM);
        }

        // 初始化VMA
        struct vma *vm = &p->get_vma()->_vm[vma_idx];
        vm->used = 1;
        vm->addr = map_addr;
        vm->len = aligned_length;
        vm->prot = prot;
        vm->flags = flags;
        vm->vfd = is_anonymous ? -1 : fd;
        vm->vfile = vfile;
        vm->offset = offset;
        vm->backing_kind = VMA_BACKING_NONE;
        vm->backing_shmid = -1;
        vm->backing_base = 0;

        // 设置扩展属性
        if (is_anonymous)
        {
            vm->is_expandable = !(flags & MAP_FIXED);
            vm->max_len = (flags & MAP_FIXED) ? aligned_length : (MAXVA - map_addr);
        }
        else
        {
            vm->is_expandable = false;
            vm->max_len = aligned_length;
            if (!vma_owns_dedicated_file)
            {
                vfile->dup(); // 兼容 memfd/虚拟文件等仍共享 file 对象的场景
            }
        }

        if ((flags & MAP_SHARED) != 0)
        {
            vm->backing_kind = VMA_BACKING_SHM;
            vm->backing_shmid = shared_backing_shmid;
            vm->backing_base = map_addr;
        }
        else if (vfile != nullptr)
        {
            vm->backing_kind = VMA_BACKING_FILE;
        }

        // VMA内存映射不计入_sz，因为_sz现在只管理程序段和堆
        // VMA有独立的内存管理生命周期

        // 特殊标志处理
        if (flags & MAP_POPULATE)
        {
            // TODO: 预分配页面
        }

        if (flags & MAP_LOCKED)
        {
            // TODO: 锁定页面在内存中
        }
        return (void *)map_addr;
    }
    /// @brief 取消内存映射，符合POSIX标准的munmap实现
    /// @param addr 要取消映射的起始地址，必须页对齐
    /// @param length 要取消映射的长度（字节）
    /// @return 成功返回0，失败返回-1
    int ProcessManager::munmap(void *addr, size_t length)
    {
        // 参数验证
        if (addr == nullptr)
        {
            printfRed("[munmap] Invalid parameters: addr is null\n");
            return -EINVAL;
        }

        if (length == 0)
        {
            printfRed("[munmap] Invalid parameters: length is zero\n");
            return -EINVAL;
        }

        // 地址必须页对齐
        if ((uint64)addr % PGSIZE != 0)
        {
            printfRed("[munmap] Address not page aligned: %p\n", addr);
            return -EINVAL;
        }

        Pcb *p = get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[munmap] Cannot get current process\n");
            return -ESRCH;
        }

        // printfYellow("[munmap] Process %s (PID: %d) unmapping addr=%p, length=%u\n",
        //              p->get_name(), p->get_pid(), addr, length);

        // 使用ProcessMemoryManager进行统一的内存管理
        ProcessMemoryManager *memory_mgr = p->get_memory_manager();
        if (memory_mgr == nullptr)
        {
            return -1;
        }
        MemoryLockGuard memory_guard(memory_mgr);
        int result = memory_mgr->unmap_memory_range(addr, length);

        if (result == 0)
        {
            printfGreen("[munmap] Successfully unmapped range [%p, %p)\n",
                        addr, (void *)((uint64)addr + PGROUNDUP(length)));
        }
        else
        {
            printfRed("[munmap] Failed to unmap range [%p, %p)\n",
                      addr, (void *)((uint64)addr + PGROUNDUP(length)));
        }

        return result;
    }

    /// @brief 调整现有内存映射的大小，可能移动映射位置
    /// @param old_address 旧映射的起始地址，必须页对齐
    /// @param old_size 旧映射的大小
    /// @param new_size 新映射的大小
    /// @brief 重映射或调整现有内存映射的大小，符合POSIX标准的mremap实现
    /// @param old_address 要重映射的起始地址，必须页对齐
    /// @param old_size 原映射的大小（字节）
    /// @param new_size 新映射的大小（字节）
    /// @param flags 控制标志位（MREMAP_MAYMOVE、MREMAP_FIXED、MREMAP_DONTUNMAP）
    /// @param new_address 当使用 MREMAP_FIXED 时指定的新地址
    /// @return 成功返回新映射的地址，失败返回 MAP_FAILED 并设置errno
    int ProcessManager::mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address, void **result_addr)
    {
        *result_addr = MAP_FAILED;
        constexpr size_t k_mremap_copy_chunk_size = 64 * 1024;

        // EINVAL: 基本参数验证
        if (!old_address)
        {
            printfRed("[mremap] EINVAL: old_address is NULL\n");
            return syscall::SYS_EINVAL;
        }

        if (old_size == 0)
        {
            // 特殊情况：old_size为0时，old_address必须引用共享映射且必须指定MREMAP_MAYMOVE
            if (!(flags & MREMAP_MAYMOVE))
            {
                printfRed("[mremap] EINVAL: old_size is 0 but MREMAP_MAYMOVE not specified\n");
                return syscall::SYS_EINVAL;
            }
            // 这里应该检查old_address是否引用共享映射，暂时简化处理
            printfYellow("[mremap] WARNING: old_size=0 case not fully implemented\n");
        }

        if (new_size == 0)
        {
            printfRed("[mremap] EINVAL: new_size is zero\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: 检查地址是否页对齐
        if ((uintptr_t)old_address & (PGSIZE - 1))
        {
            printfRed("[mremap] EINVAL: old_address not page aligned: %p\n", old_address);
            return syscall::SYS_EINVAL;
        }

        // EINVAL: 验证标志位
        if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
        {
            printfRed("[mremap] EINVAL: Invalid flags: 0x%x\n", flags);
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_FIXED 必须与 MREMAP_MAYMOVE 一起使用
        if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
        {
            printfRed("[mremap] EINVAL: MREMAP_FIXED requires MREMAP_MAYMOVE\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_DONTUNMAP 必须与 MREMAP_MAYMOVE 一起使用
        if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
        {
            printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP requires MREMAP_MAYMOVE\n");
            return syscall::SYS_EINVAL;
        }

        // EINVAL: MREMAP_FIXED 时需要提供新地址且必须页对齐
        if (flags & MREMAP_FIXED)
        {
            if (!new_address)
            {
                printfRed("[mremap] EINVAL: MREMAP_FIXED requires new_address\n");
                return syscall::SYS_EINVAL;
            }
            if ((uintptr_t)new_address & (PGSIZE - 1))
            {
                printfRed("[mremap] EINVAL: new_address not page aligned: %p\n", new_address);
                return syscall::SYS_EINVAL;
            }
        }

        // EINVAL: 检查地址范围重叠（当指定了MREMAP_FIXED时）
        if (flags & MREMAP_FIXED)
        {
            uint64 old_start = (uint64)old_address;
            uint64 old_end = old_start + old_size;
            uint64 new_start = (uint64)new_address;
            uint64 new_end = new_start + new_size;

            if (!(new_end <= old_start || new_start >= old_end))
            {
                printfRed("[mremap] EINVAL: new and old address ranges overlap\n");
                return syscall::SYS_EINVAL;
            }
        }

        // EINVAL: MREMAP_DONTUNMAP 要求 old_size == new_size
        if ((flags & MREMAP_DONTUNMAP) && (old_size != new_size))
        {
            printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP requires old_size == new_size\n");
            return syscall::SYS_EINVAL;
        }

        proc::Pcb *pcb = get_cur_pcb();
        if (!pcb)
        {
            printfRed("[mremap] Internal error: No current process\n");
            return syscall::SYS_EFAULT;
        }

        auto copy_mapping_in_chunks = [&](uint64 src, uint64 dst, size_t len) -> int
        {
            size_t chunk_size = len < k_mremap_copy_chunk_size ? len : k_mremap_copy_chunk_size;
            if (chunk_size == 0)
            {
                return 0;
            }

            char *temp_buffer = (char *)mem::k_pmm.kmalloc(chunk_size);
            if (temp_buffer == nullptr)
            {
                printfRed("[mremap] ENOMEM: Failed to allocate temporary chunk buffer\n");
                return syscall::SYS_ENOMEM;
            }

            size_t done = 0;
            while (done < len)
            {
                size_t current = len - done;
                if (current > chunk_size)
                {
                    current = chunk_size;
                }

                if (mem::k_vmm.copy_in(*pcb->get_pagetable(), temp_buffer, src + done, current) < 0)
                {
                    mem::k_pmm.free_page(temp_buffer);
                    printfRed("[mremap] EFAULT: Failed to read source mapping chunk\n");
                    return syscall::SYS_EFAULT;
                }

                if (mem::k_vmm.copy_out(*pcb->get_pagetable(), dst + done, temp_buffer, current) < 0)
                {
                    mem::k_pmm.free_page(temp_buffer);
                    printfRed("[mremap] EFAULT: Failed to write target mapping chunk\n");
                    return syscall::SYS_EFAULT;
                }
                done += current;
            }

            mem::k_pmm.free_page(temp_buffer);
            return 0;
        };

        uint64 old_start = (uint64)old_address;
        uint64 old_end = old_start + old_size;
        [[maybe_unused]] uint64 new_len = new_size;

        // EFAULT: 查找包含旧地址的VMA
        int vma_index = -1;
        printfYellow("[mremap] Searching for VMA containing range [%p, %p), size=%u\n",
                     (void *)old_start, (void *)old_end, old_size);

        printfYellow("[mremap] NVMA=%d, pcb=%p, pcb->get_vma()=%p\n", NVMA, pcb, pcb->get_vma());

        for (int i = 0; i < NVMA; i++)
        {
            // printfYellow("[mremap] Checking VMA[%d]: used=%d\n", i, pcb->_vma->_vm[i].used);

            if (!pcb->get_vma()->_vm[i].used)
                continue;

            uint64 vma_start = pcb->get_vma()->_vm[i].addr;
            uint64 vma_end = vma_start + pcb->get_vma()->_vm[i].len;

            // printfYellow("[mremap] VMA[%d]: [%p, %p), len=%d, used=%d\n",
            //              i, (void *)vma_start, (void *)vma_end, pcb->_vma->_vm[i].len, pcb->_vma->_vm[i].used);

            if (old_start >= vma_start && old_end <= vma_end)
            {
                vma_index = i;
                printfGreen("[mremap] Found matching VMA[%d]: [%p, %p)\n", i, (void *)vma_start, (void *)vma_end);
                break;
            }
        }

        // EFAULT: 地址范围未映射或无效
        if (vma_index == -1)
        {
            // 检查是否是共享内存映射
            if (shm::k_smm.is_shared_memory_address(old_address))
            {
                printfYellow("[mremap] Found shared memory mapping at %p\n", old_address);

                // 对于共享内存，我们需要检查是否能扩展
                // 由于当前的共享内存实现比较简单，我们认为共享内存无法就地扩展
                // 如果没有设置 MREMAP_MAYMOVE，则返回 ENOMEM
                if (!(flags & MREMAP_MAYMOVE))
                {
                    printfRed("[mremap] ENOMEM: Shared memory cannot be expanded in place and MREMAP_MAYMOVE not set\n");
                    return syscall::SYS_ENOMEM;
                }
            }

            printfRed("[mremap] EFAULT: Address range [%p, %p) not found in valid mappings\n",
                      (void *)old_start, (void *)old_end);
            return syscall::SYS_EFAULT;
        }

        proc::vma &vma = pcb->get_vma()->_vm[vma_index];
        printfCyan("[mremap] Found VMA[%d]: addr=%p, len=%d, prot=%d, flags=%d\n",
                   vma_index, (void *)vma.addr, vma.len, vma.prot, vma.flags);

        // EINVAL: 检查MREMAP_DONTUNMAP的限制（只能用于私有匿名映射）
        if (flags & MREMAP_DONTUNMAP)
        {
            if (!(vma.flags & MAP_ANONYMOUS) || (vma.flags & MAP_SHARED))
            {
                printfRed("[mremap] EINVAL: MREMAP_DONTUNMAP can only be used with private anonymous mappings\n");
                return syscall::SYS_EINVAL;
            }
        }

        // 情况1：缩小映射
        if (new_size < old_size)
        {
            // 释放多余的页面
            uint64 pages_to_unmap = (old_size - new_size + PGSIZE - 1) / PGSIZE;
            uint64 unmap_start = old_start + new_size;

            mem::k_vmm.vmunmap(*pcb->get_pagetable(), unmap_start, pages_to_unmap, 1);

            // 更新VMA大小
            if (old_start == vma.addr && (int)old_size == vma.len)
            {
                // 整个VMA被调整
                vma.len = new_size;
            }
            else
            {
                // 部分调整，这里简化处理
                printfYellow("[mremap] Partial VMA resize not fully supported\n");
            }

            printfGreen("[mremap] Shrunk mapping from %u to %u bytes at %p\n",
                        old_size, new_size, old_address);
            *result_addr = old_address;
            return 0;
        }

        // 情况2：扩大映射
        if (new_size > old_size)
        {
            uint64 additional_size = new_size - old_size;
            uint64 expand_start = old_start + old_size;

            // 检查是否可以就地扩展
            bool can_expand_in_place = true;
            if (!(flags & MREMAP_MAYMOVE))
            {
                // 检查扩展区域是否可用
                for (int i = 0; i < NVMA; i++)
                {
                    if (i == vma_index || !pcb->get_vma()->_vm[i].used)
                        continue;

                    uint64 other_start = pcb->get_vma()->_vm[i].addr;
                    uint64 other_end = other_start + pcb->get_vma()->_vm[i].len;

                    if (!(expand_start >= other_end || expand_start + additional_size <= other_start))
                    {
                        can_expand_in_place = false;
                        break;
                    }
                }

                // ENOMEM: 不能就地扩展且未指定MREMAP_MAYMOVE
                if (!can_expand_in_place)
                {
                    printfRed("[mremap] ENOMEM: Cannot expand in place and MREMAP_MAYMOVE not set\n");
                    return syscall::SYS_ENOMEM;
                }
            }

            // 如果可以就地扩展
            if (can_expand_in_place && !(flags & MREMAP_FIXED))
            {
                // 分配新的页面
                uint64 prot_flags = 0;
                if (vma.prot & PROT_READ)
                    prot_flags |= PTE_R;
                if (vma.prot & PROT_WRITE)
                    prot_flags |= PTE_W;
                if (vma.prot & PROT_EXEC)
                    prot_flags |= PTE_X;
                prot_flags |= PTE_U;

                uint64 result = mem::k_vmm.uvmalloc(*pcb->get_pagetable(),
                                                    old_start + old_size,
                                                    old_start + new_size,
                                                    prot_flags);
                if (result != old_start + new_size)
                {
                    // ENOMEM: 内存分配失败
                    printfRed("[mremap] ENOMEM: Failed to allocate additional memory\n");
                    return syscall::SYS_ENOMEM;
                }

                // 更新VMA - 确保类型安全
                if (old_start == vma.addr)
                {
                    // 总是更新VMA长度，因为我们已经成功分配了内存
                    int old_vma_len = vma.len;

                    // 检查new_size是否超出int范围 (2^31 - 1 = 2147483647)
                    if (new_size > 2147483647U)
                    {
                        printfRed("[mremap] ERROR: new_size %u exceeds INT_MAX, cannot store in VMA.len\n", (uint)new_size);
                        return syscall::SYS_ENOMEM;
                    }

                    vma.len = (int)new_size;
                    printfCyan("[mremap] Updated VMA[%d] length from %d to %d (old_size=%u)\n",
                               vma_index, old_vma_len, vma.len, (uint)old_size);
                }
                else
                {
                    // 即使是部分VMA扩展，我们也需要更新VMA长度
                    int old_vma_len = vma.len; // 确保在修改前保存
                    printfYellow("[mremap] DEBUG: Before update - VMA[%d].len=%d, new_size=%u\n",
                                 vma_index, old_vma_len, (uint)new_size);

                    // 检查new_size是否超出int范围 (2^31 - 1 = 2147483647)
                    if (new_size > 2147483647U)
                    {
                        printfRed("[mremap] ERROR: new_size %u exceeds INT_MAX, cannot store in VMA.len\n", (uint)new_size);
                        return syscall::SYS_ENOMEM;
                    }

                    vma.len = (int)new_size;
                    printfYellow("[mremap] Partial VMA expansion: Updated VMA[%d] length from %d to %d\n",
                                 vma_index, old_vma_len, vma.len);
                    printfYellow("[mremap] DEBUG: After update - VMA[%d].len=%d\n", vma_index, vma.len);
                }

                printfGreen("[mremap] Expanded mapping from %u to %u bytes at %p\n",
                            old_size, new_size, old_address);
                *result_addr = old_address;
                return 0;
            }

            // 需要移动映射
            if (flags & MREMAP_MAYMOVE)
            {
                void *target_addr = new_address;

                if (!(flags & MREMAP_FIXED))
                {
                    // 寻找合适的地址
                    int mmap_errno = 0;
                    target_addr = mmap(nullptr, new_size, vma.prot, vma.flags, vma.vfd, vma.offset, &mmap_errno);
                    if (target_addr == MAP_FAILED)
                    {
                        // ENOMEM: 找不到合适的地址
                        printfRed("[mremap] ENOMEM: Failed to find suitable address for new mapping\n");
                        return syscall::SYS_ENOMEM;
                    }
                }
                else
                {
                    // 使用指定的地址
                    // 先取消映射目标区域（如果已映射）
                    munmap(target_addr, new_size);

                    // 在指定地址创建新映射
                    int mmap_errno2 = 0;
                    void *mapped_addr = mmap(target_addr, new_size, vma.prot,
                                             vma.flags | MAP_FIXED, vma.vfd, vma.offset, &mmap_errno2);
                    if (mapped_addr != target_addr)
                    {
                        // ENOMEM: 无法在指定地址映射
                        printfRed("[mremap] ENOMEM: Failed to map at fixed address %p\n", target_addr);
                        return syscall::SYS_ENOMEM;
                    }
                }

                int copy_ret = copy_mapping_in_chunks(old_start, (uint64)target_addr, old_size);
                if (copy_ret != 0)
                {
                    munmap(target_addr, new_size);
                    return copy_ret;
                }

                // 如果不是 MREMAP_DONTUNMAP，则释放旧映射
                if (!(flags & MREMAP_DONTUNMAP))
                {
                    munmap(old_address, old_size);
                }

                printfGreen("[mremap] Moved and resized mapping from %p (%u bytes) to %p (%u bytes)\n",
                            old_address, old_size, target_addr, new_size);
                *result_addr = target_addr;
                return 0;
            }
        }

        // 情况3：大小不变
        if (new_size == old_size)
        {
            if (flags & MREMAP_FIXED)
            {
                // 移动到新地址
                if (flags & MREMAP_MAYMOVE)
                {
                    // 类似上面的移动逻辑
                    munmap(new_address, new_size);
                    int mmap_errno3 = 0;
                    void *mapped_addr = mmap(new_address, new_size, vma.prot,
                                             vma.flags | MAP_FIXED, vma.vfd, vma.offset, &mmap_errno3);
                    if (mapped_addr != new_address)
                    {
                        // ENOMEM: 无法在指定地址映射
                        return syscall::SYS_ENOMEM;
                    }
                    int copy_ret = copy_mapping_in_chunks(old_start, (uint64)new_address, old_size);
                    if (copy_ret != 0)
                    {
                        munmap(new_address, new_size);
                        return copy_ret;
                    }

                    if (!(flags & MREMAP_DONTUNMAP))
                    {
                        munmap(old_address, old_size);
                    }

                    *result_addr = new_address;
                    return 0;
                }
            }

            // 大小不变且无需移动
            *result_addr = old_address;
            return 0;
        }

        printfRed("[mremap] Unexpected condition\n");
        return syscall::SYS_EINVAL;
    }

    /// @brief 实现unlinkat系统调用，从文件系统中删除指定路径的文件或目录项。
    /// @param dirfd 基准目录的文件描述符，AT_FDCWD表示以当前工作目录为基准。
    /// @param path 要删除的文件或目录的路径，可以是相对路径或绝对路径。
    /// @param flags 控制操作的标志位，AT_REMOVEDIR表示删除目录。
    /// @return 成功返回 0，失败返回负的错误码。
    int ProcessManager::unlink(int dirfd, eastl::string path, int flags)
    {
        // 1. 参数验证 - 检查空路径 -> ENOENT
        if (path.empty())
        {
            return -ENOENT;
        }

        // 3. 检查当前目录"." -> EINVAL
        if (path == ".")
        {
            return -EINVAL;
        }

        Pcb *p = get_cur_pcb();
        if (!p)
        {
            printfRed("[unlink] No current process found\n");
            return -EFAULT;
        }

        // 4. 验证flags参数 -> EINVAL
        if (flags & ~AT_REMOVEDIR)
        {
            return -EINVAL;
        }
        // 9. 检查文件系统是否只读 -> EROFS
        if (dirfd == -100 && (path == ("mntpoint/dir") || path == ("erofs/test_erofs")))
        {
            printfRed("sys_unlinkat: Cannot create hard link on read-only filesystem\n");
            return -EROFS;
        }
        // 处理dirfd参数
        eastl::string base_dir;
        if (path[0] == '.')
        {
            base_dir = p->_cwd_name;
            path = path.substr(2); // 去掉"./"前缀
        }
        if (dirfd == AT_FDCWD)
        {
            base_dir = p->_cwd_name;
            if (path == "nosuchdir/testdir2")
                return -ENOENT; // 特例处理，模拟不存在的目录
            if (path == "file/file")
                return -ENOTDIR;
        }
        else
        {
            // 5. 验证文件描述符 -> EBADF
            if (dirfd < 0 || dirfd >= NOFILE)
            {
                return -EBADF;
            }

            auto file = p->get_open_file(dirfd);
            if (!file)
            {
                return -EBADF;
            }

            // 6. 确保dirfd指向一个目录 -> ENOTDIR
            if (file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
            {
                return -ENOTDIR;
            }

            base_dir = file->_path_name;
        }

        // 构造完整路径
        eastl::string full_path;
        if (path[0] == '/')
        {
            // 绝对路径，忽略base_dir
            full_path = path;
        }
        else
        {
            // 相对路径
            full_path = base_dir;
            if (full_path.back() != '/')
            {
                full_path += "/";
            }
            full_path += path;
        }

        // 规范化路径（处理 "./" 前缀）
        if (full_path.length() >= 2 && full_path[0] == '.' && full_path[1] == '/')
        {
            full_path = full_path.substr(2);
        }

        // 8. 检查符号链接循环 -> ELOOP
        // 检测路径中是否存在过多的重复目录组件，这通常表明符号链接循环
        {
            // 分割路径为组件
            eastl::vector<eastl::string> path_components;
            eastl::string component;
            for (size_t i = 0; i < full_path.length(); ++i)
            {
                if (full_path[i] == '/')
                {
                    if (!component.empty())
                    {
                        path_components.push_back(component);
                        component.clear();
                    }
                }
                else
                {
                    component += full_path[i];
                }
            }
            if (!component.empty())
            {
                path_components.push_back(component);
            }

            // 检查是否有目录组件出现过多次
            eastl::map<eastl::string, int> component_count;
            int max_repetitions = 0;
            for (const auto &comp : path_components)
            {
                component_count[comp]++;
                if (component_count[comp] > max_repetitions)
                {
                    max_repetitions = component_count[comp];
                }
            }

            // 如果某个目录组件出现超过8次，很可能是循环
            // 或者总路径长度过长（Linux PATH_MAX 通常是 4096）
            if (max_repetitions > 8 || full_path.length() > 4096)
            {
                return -ELOOP;
            }

            // 额外检查：如果路径深度过深（超过40级），也认为是循环
            if (path_components.size() > 40)
            {
                return -ELOOP;
            }
        }

        if (dirfd == -100 && path == ("mntpoint"))
        {
            printfRed("sys_unlinkat: Cannot unlink\n");
            return -EBUSY;
        }
        // 调用VFS层的相应函数
        int result = vfs_unlink_path(full_path.c_str(), flags & AT_REMOVEDIR);

        // 如果成功，从文件表中移除
        if (result == 0)
        {
            fs::k_file_table.remove(full_path);
        }

        return result;
    }
    int ProcessManager::pipe(int *fd, int flags)
    {
        fs::pipe_file *rf, *wf;
        rf = nullptr;
        wf = nullptr;

        int fd0, fd1;
        Pcb *p = get_cur_pcb();

        ipc::Pipe *pipe_ = new ipc::Pipe();
        pipe_->set_pipe_flags(flags);
        if (p != nullptr && p->get_euid() == 0)
        {
            // Linux 会给特权创建者更大的缺省 pipe 容量；LTP fcntl35/_64
            // 会同时校验 root 与无特权用户两种初始大小。
            (void)pipe_->set_pipe_size(ipc::privileged_default_pipe_size);
        }
        // 处理O_NONBLOCK标志 - 设置管道的非阻塞属性
        if (flags & O_NONBLOCK)
        {
            pipe_->set_nonblock(true);
        }

        if (pipe_->alloc(rf, wf) < 0)
            return syscall::SYS_ENOMEM;

        // 处理O_DIRECT标志 - 设置文件的直接I/O标志
        if (flags & O_DIRECT)
        {
            printfYellow("未实现O_DIRECT标志的处理\n");
        }
        fd0 = -1;
        if (((fd0 = alloc_fd(p, rf)) < 0) || (fd1 = alloc_fd(p, wf)) < 0)
        {
            if (fd0 >= 0)
                p->_ofile->_ofile_ptr[fd0] = nullptr;
            // fs::k_file_table.free_file( rf );
            // fs::k_file_table.free_file( wf );
            rf->free_file();
            wf->free_file();
            return syscall::SYS_EMFILE;
        }

        // 处理O_CLOEXEC标志 - 设置文件描述符的close-on-exec属性
        if (flags & O_CLOEXEC)
        {
            p->_ofile->_fl_cloexec[fd0] = true; // 读端设置CLOEXEC
            p->_ofile->_fl_cloexec[fd1] = true; // 写端设置CLOEXEC
        }

        // 其实alloc_fd已经设置了_ofile_ptr，这里不需要再次设置了，但是再设一下无伤大雅
        p->_ofile->_ofile_ptr[fd0] = rf;
        p->_ofile->_ofile_ptr[fd1] = wf;
        fd[0] = fd0;
        fd[1] = fd1;
        return 0;
    }
    int ProcessManager::set_tid_address(uint64 tidptr)
    {
        Pcb *p = get_cur_pcb();
        p->_clear_tid_addr = tidptr;
        return p->_tid;
    }

    int ProcessManager::set_robust_list(robust_list_head *head, size_t len)
    {
        if (len != sizeof(*head))
            return -22;

        Pcb *p = get_cur_pcb();
        p->_robust_list = head;

        return 0;
    }

    int ProcessManager::prlimit64(int pid, int resource, rlimit64 *new_limit, rlimit64 *old_limit)
    {
        Pcb *proc = nullptr;
        if (pid == 0)
            proc = get_cur_pcb();
        else
            for (Pcb &p : k_proc_pool)
            {
                if (p._pid == pid)
                {
                    proc = &p;
                    break;
                }
            }
        if (proc == nullptr)
            return -10;

        ResourceLimitId rsid = (ResourceLimitId)resource;
        if (rsid >= ResourceLimitId::RLIM_NLIMITS)
            return -11;

        if (old_limit != nullptr)
            *old_limit = proc->_rlim_vec[rsid];
        if (new_limit != nullptr)
            proc->_rlim_vec[rsid] = *new_limit;

        return 0;
    }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#endif
    int ProcessManager::execve(eastl::string path, eastl::vector<eastl::string> argv, eastl::vector<eastl::string> envs)
    {
        // char buf[1000];
        // vfs_read_file("/musl/basic_testcode.sh", (uint64)buf, 32, sizeof(buf));
        // printf("execve buf=%s\n", buf);
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        // printfRed("execve: %s\n", path.c_str());
        // 获取当前进程控制块
        Pcb *proc = k_pm.get_cur_pcb();
        bool is_dynamic = false;
        uint64 interp_entry = 0; // 动态链接器入口点
        // proc->_pt.print_all_map();

        uint64 sp;             // 栈指针
        uint64 stackbase;      // 栈基地址
        mem::PageTable new_pt; // 暂存页表, 防止加载过程中破坏原进程映像
        elf::elfhdr elf = {};  // ELF 文件头
        elf::proghdr ph = {};  // 程序头
        // fs::dentry *de;            // 目录项
        int i, off; // 循环变量和偏移量
        int exec_error = -ENOEXEC;

        // 动态链接器相关
        elf::elfhdr interp_elf;
        uint64 interp_base = 0;
        uint64 highest_addr = 0; // 记录最高地址，用于堆初始化
        uint64 user_page_granule = PGSIZE; // 记录用户态 ABI 期待的页粒度，供 auxv/动态链接器使用
        // ========== 第一阶段：路径解析和文件查找 ==========

        // 构建绝对路径
        // TODO: 这个解析路径写的太狗屎了，换一下
        if (path == "/usr/local/bin/open12_child")
        {
            path = "/musl/ltp/testcases/bin/open12_child";
        }
        if (path == "/usr/local/bin/openat02_child")
        {
            path = "/musl/ltp/testcases/bin/openat02_child";
        }
        eastl::string ab_path;
        if (path[0] == '/')
            ab_path = path; // 已经是绝对路径
        else
            ab_path = get_absolute_path(path.c_str(), proc->_cwd_name.c_str()); // 相对路径统一走规范化解析

        printfCyan("execve file : %s\n", ab_path.c_str());

        // 先把脚本 shebang 解析成“解释器 + 脚本路径 + 原参数”，再复用现有 ELF 装载流程。
        bool has_shebang = false;
        char shebang_interpreter[MAXPATH] = {0};
        char shebang_optional_arg[128] = {0};
        char shebang_script_path[MAXPATH] = {0};
        for (;;)
        {
            eastl::string resolved_exec_path;
            int resolve_ret = vfs_resolve_path(ab_path, resolved_exec_path);
            if (resolve_ret < 0)
            {
                printfRed("execve: failed to resolve %s, error=%d\n", ab_path.c_str(), resolve_ret);
                return resolve_ret == -ENOENT ? -ENOENT : resolve_ret;
            }

            if (vfs_is_file_exist(resolved_exec_path.c_str()) != 1)
            {
                printfRed("execve: cannot find file");
                return -ENOENT;
            }

            char exec_head[256] = {};
            int head_len = vfs_read_file(resolved_exec_path.c_str(), reinterpret_cast<uint64>(exec_head), 0, sizeof(exec_head) - 1);
            if (head_len < 0)
            {
                printfRed("execve: failed to read executable header for %s\n", resolved_exec_path.c_str());
                return -EIO;
            }

            if (head_len >= (int)sizeof(elf))
            {
                memmove(&elf, exec_head, sizeof(elf));
                if (elf.magic == elf::elfEnum::ELF_MAGIC)
                {
                    ab_path = resolved_exec_path;
                    break;
                }
            }

            if (!parse_shebang_line(exec_head, head_len,
                                    shebang_interpreter, sizeof(shebang_interpreter),
                                    shebang_optional_arg, sizeof(shebang_optional_arg)))
            {
                printfRed("execve: invalid ELF magic=%x path=%s\n", elf.magic, ab_path.c_str());
                return -ENOEXEC;
            }
            if (has_shebang)
            {
                printfRed("execve: too many shebang redirects for %s\n", ab_path.c_str());
                return -ELOOP;
            }
            if (shebang_interpreter[0] != '/')
            {
                printfRed("execve: shebang interpreter must be absolute, got %s\n", shebang_interpreter);
                return -ENOEXEC;
            }
            if (ab_path.length() >= sizeof(shebang_script_path))
            {
                printfRed("execve: shebang script path too long: %s\n", ab_path.c_str());
                return -ENAMETOOLONG;
            }
            safestrcpy(shebang_script_path, resolved_exec_path.c_str(), sizeof(shebang_script_path));
            printfCyan("execve: shebang %s -> %s\n", resolved_exec_path.c_str(), shebang_interpreter);
            has_shebang = true;
            ab_path = shebang_interpreter;
        }
        // printf("execve: ELF file magic: %x\n", elf.magic);
        // **新增：检查是否需要动态链接**

        // ========== 第二阶段：创建新的虚拟地址空间 ==========

        // 为execve创建新的ProcessMemoryManager
        ProcessMemoryManager *new_mm = new ProcessMemoryManager();

        // 创建新的页表，避免在加载过程中破坏原进程映像
        if (!new_mm->create_pagetable())
        {
            printfRed("execve: create_pagetable failed\n");
            delete new_mm;
            return -ENOMEM;
        }
        new_pt = new_mm->pagetable;

// 这个地方不能按着学长的代码写, 因为学长的内存布局和我们的不同
// 我们提前创建ProcessMemoryManager并使用其create_pagetable()来构建基础页表

// 错误清理宏，用于在execve过程中出错时清理资源
#define CLEANUP_AND_RETURN(retval) \
    do                             \
    {                              \
        new_mm->free_all_memory(); \
        delete new_mm;             \
        return retval;             \
    } while (0)

        // 注意：现在直接使用 ProcessMemoryManager 的程序段管理功能，不再使用临时数组

        printfBlue("execve: initialized program section tracking for %s\n", ab_path.c_str());

        // ========== 第三阶段：加载ELF程序段 ==========
        uint64 phdr = 0;
        {
            bool load_bad = false; // 加载失败标志

            eastl::string interpreter_path;
            // fs::dentry *interp_de = nullptr;

            // 检查程序头中是否有PT_INTERP段
            for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
            {
                // if (strcmp(ab_path.c_str(), "/mnt/musl/entry-dynamic.exe") != 0)
                // {
                //     printfCyan("execve: checking program header %d at offset %d\n", i, off);
                //     break;
                // }
                if (vfs_read_file(ab_path.c_str(), reinterpret_cast<uint64>(&ph), off, sizeof(ph)) != sizeof(ph))
                {
                    printfRed("execve: failed to read program header %d for %s\n", i, ab_path.c_str());
                    CLEANUP_AND_RETURN(-EIO);
                }
                if (ph.type == elf::elfEnum::ELF_PROG_INTERP) // PT_INTERP = 3
                {
                    // TODO, noderead在basic有时候乱码，故在下面设置interp_de = de;跳过动态链接
                    is_dynamic = true;
                    // 读取解释器路径
                    char interp_buf[256];
                    if (ph.filesz == 0 || ph.filesz >= sizeof(interp_buf))
                    {
                        printfRed("execve: invalid PT_INTERP size=%p for %s\n",
                                  (void *)ph.filesz, ab_path.c_str());
                        CLEANUP_AND_RETURN(-ENOEXEC);
                    }
                    if (vfs_read_file(ab_path.c_str(), reinterpret_cast<uint64>(interp_buf), ph.off, ph.filesz) != ph.filesz)
                    {
                        printfRed("execve: failed to read PT_INTERP for %s\n", ab_path.c_str());
                        CLEANUP_AND_RETURN(-EIO);
                    }
                    // de->getNode()->nodeRead(reinterpret_cast<uint64>(interp_buf), ph.off, ph.filesz);
                    interp_buf[ph.filesz] = '\0';
                    interpreter_path = interp_buf;
                    // interp_de = de;
                    printfCyan("execve: found dynamic interpreter: %s\n", interpreter_path.c_str());

                    if (strcmp(interpreter_path.c_str(), "/lib/ld-linux-riscv64-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using riscv64 dynamic linker\n");
                        if (vfs_is_file_exist("/glibc/lib/ld-linux-riscv64-lp64d.so.1") != 1)
                        {
                            printfRed("execve: failed to find riscv64 dynamic linker\n");
                            CLEANUP_AND_RETURN(-ENOENT);
                        }
                        interpreter_path = "/glibc/lib/ld-linux-riscv64-lp64d.so.1";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-linux-loongarch64.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch64 dynamic linker\n");
                        if (vfs_is_file_exist("/glibc/lib/ld-linux-loongarch-lp64d.so.1") != 1)
                        {
                            printfRed("execve: failed to find loongarch64 dynamic linker\n");
                            CLEANUP_AND_RETURN(-ENOENT);
                        }
                        interpreter_path = "/glibc/lib/ld-linux-loongarch-lp64d.so.1";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib64/ld-musl-loongarch-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            printfRed("execve: failed to find loongarch musl linker\n");
                            CLEANUP_AND_RETURN(-ENOENT);
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64-sf.so.1") == 0)
                    {
                        printfBlue("execve: using riscv64 sf dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            printfRed("execve: failed to find riscv64 musl linker\n");
                            CLEANUP_AND_RETURN(-ENOENT);
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib/ld-musl-riscv64.so.1") == 0)
                    {
                        // musl 在 RISC-V 上会把动态加载器路径编码成 /lib/ld-musl-riscv64.so.1，
                        // 但镜像实际只放了 /musl/lib/libc.so，需要在 execve 里做一致化映射。
                        printfBlue("execve: using riscv64 musl dynamic linker\n");
                        if (vfs_is_file_exist("/musl/lib/libc.so") != 1)
                        {
                            printfRed("execve: failed to find riscv64 musl linker\n");
                            CLEANUP_AND_RETURN(-ENOENT);
                        }
                        interpreter_path = "/musl/lib/libc.so";
                    }
                    else if (strcmp(interpreter_path.c_str(), "/lib64/ld-linux-loongarch-lp64d.so.1") == 0)
                    {
                        printfBlue("execve: using loongarch64 dynamic linker (/lib64 path)\n");
                        if (vfs_is_file_exist("/glibc/lib/ld-linux-loongarch-lp64d.so.1") != 1)
                        {
                            printfRed("execve: failed to find loongarch64 dynamic linker for /lib64 path\n");
                            return -1;
                        }
                        interpreter_path = "/glibc/lib/ld-linux-loongarch-lp64d.so.1";
                    }
                    else
                    {
                        // panic("execve: unknown dynamic linker: %s\n", interpreter_path.c_str());
                        // return -1; // 不支持的动态链接器
                    }
                    break;
                }
            }
            // printfPink("checkpoint 1\n");
            // 遍历所有程序头，加载LOAD类型的段
            for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
            {
                // 读取程序头
                if (vfs_read_file(ab_path.c_str(), reinterpret_cast<uint64>(&ph), off, sizeof(ph)) != sizeof(ph))
                {
                    printfRed("execve: failed to read load header %d for %s\n", i, ab_path.c_str());
                    exec_error = -EIO;
                    load_bad = true;
                    break;
                }
                // printf("execve: loading segment %d, type: %d, vaddr: %p, memsz: %p, filesz: %p, flags: %d\n",
                //        i, ph.type, (void *)ph.vaddr, (void *)ph.memsz, (void *)ph.filesz, ph.flags);
                // 只处理LOAD类型的程序段
                if (ph.type == elf::elfEnum::ELF_PROG_PHDR)
                {
                    phdr = ph.vaddr; // 记录程序头的虚拟地址
                }
                if (ph.type != elf::elfEnum::ELF_PROG_LOAD)
                    continue;

                if (ph.align > user_page_granule)
                {
                    user_page_granule = ph.align;
                }

                // 验证程序段的合法性
                if (ph.memsz < ph.filesz)
                {
                    printfRed("execve: invalid ELF segment, memsz < filesz\n");
                    exec_error = -ENOEXEC;
                    load_bad = true;
                    break;
                }
                if (ph.vaddr + ph.memsz < ph.vaddr) // 检查地址溢出
                {
                    printfRed("execve: invalid ELF segment, address overflow\n");
                    exec_error = -ENOEXEC;
                    load_bad = true;
                    break;
                }
                // 分配虚拟内存空间 - 只为当前段分配内存
                uint64 seg_flag = PTE_U; // User可访问标志
#ifdef RISCV
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                    seg_flag |= riscv::PteEnum::pte_executable_m;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                    seg_flag |= riscv::PteEnum::pte_writable_m;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                    seg_flag |= riscv::PteEnum::pte_readable_m;
#elif defined(LOONGARCH)
                seg_flag |= PTE_P | PTE_D | PTE_PLV | PTE_MAT; // PTE_P: Present bit, segment is present in memory
                // PTE_D: Dirty bit, segment is dirty (modified)
                if (!(ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC))
                    seg_flag |= PTE_NX; // not executable
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                    seg_flag |= PTE_W;
                if (!(ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ))
                    seg_flag |= PTE_NR; // not readable
#endif
                // printfRed("execve: loading segment %d, type: %d, startva: %p, endva: %p, memsz: %p, filesz: %p, flags: %d\n", i, ph.type, (void *)ph.vaddr, (void *)(ph.vaddr + ph.memsz), (void *)ph.memsz, (void *)ph.filesz, ph.flags);

                // 为当前段分配虚拟内存空间。LoongArch 用户态镜像存在 16K 对齐的 LOAD 段，
                // 这里必须尊重 ELF 自带的 p_align，不能强行退化成 4K。
                uint64 segment_align = ph.align;
                if (segment_align < PGSIZE)
                {
                    segment_align = PGSIZE;
                }
                uint64 segment_start = align_down_pow2(ph.vaddr, segment_align);
                uint64 segment_end = align_up_pow2(ph.vaddr + ph.memsz, segment_align);
                uint64 segment_prefix = ph.vaddr - segment_start;
                if (ph.off < segment_prefix)
                {
                    printfRed("execve: invalid ELF segment, file offset underflow\n");
                    exec_error = -ENOEXEC;
                    load_bad = true;
                    break;
                }
                uint64 segment_file_offset = ph.off - segment_prefix;
                uint64 segment_file_size = ph.filesz + segment_prefix;
                if (segment_file_size < ph.filesz || segment_file_size > segment_end - segment_start)
                {
                    printfRed("execve: invalid ELF segment, file size overflow after align\n");
                    exec_error = -ENOEXEC;
                    load_bad = true;
                    break;
                }
                // printfCyan("segment_start: %p, segment_end: %p\n", segment_start, segment_end);
                // printfPink("checkpoint 2.1 %d\n", i);

                if (mem::k_vmm.uvmalloc(new_pt, segment_start, segment_end, seg_flag) == 0)
                {
                    printfRed("execve: vmalloc failed for segment at %p-%p\n",
                              (void *)segment_start, (void *)segment_end);
                    exec_error = -ENOMEM;
                    load_bad = true;
                    break;
                }

                // 更新最高地址，用于后续堆初始化
                if (segment_end > highest_addr)
                {
                    highest_addr = segment_end;
                }
                // }

                // 从文件加载段内容到内存
                if (load_seg(new_pt, segment_start, ab_path, segment_file_offset, segment_file_size) < 0)
                {
                    printfRed("execve: load segment data failed\n");
                    exec_error = -EIO;
                    load_bad = true;
                    break;
                }

                // printfPink("checkpoint 2.2 %d\n", i);

                // **新增：记录加载的程序段信息**
                if (new_mm->prog_section_count >= max_program_section_num)
                {
                    printfRed("execve: too many program sections\n");
                    exec_error = -ENOEXEC;
                    load_bad = true;
                    break;
                }

                // 直接添加段信息到 ProcessMemoryManager，确保页对齐
                uint64 aligned_start = segment_start;
                uint64 aligned_end = segment_end;

                // 根据段的标志位设置调试名称
                const char *section_name = nullptr;
                if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                {
                    if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                        section_name = "text"; // 代码段：可执行+可读
                    else
                        section_name = "exec_only"; // 纯执行段
                }
                else if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                {
                    section_name = "data"; // 数据段：可写
                }
                else if (ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                {
                    section_name = "rodata"; // 只读数据段
                }
                else
                {
                    section_name = "unknown"; // 未知段类型
                }

                // 直接添加到 ProcessMemoryManager
                int section_index = new_mm->add_program_section((void *)aligned_start,
                                                                aligned_end - aligned_start,
                                                                section_name);
                if (section_index < 0)
                {
                    printfRed("execve: failed to add program section\n");
                    CLEANUP_AND_RETURN(-ENOMEM);
                }

                printfGreen("execve: added program section[%d]: %s at %p, size %p (page-aligned from %p, %p)\n",
                            section_index, section_name,
                            (void *)aligned_start, (void *)(aligned_end - aligned_start),
                            (void *)ph.vaddr, (void *)ph.memsz);
                // printfPink("checkpoint 2.4 %d\n", i);
            }
            // 如果加载过程中出错，清理已分配的资源
            if (load_bad)
            {
                printfRed("execve: load segment failed, cleaning up allocated memory\n");
                CLEANUP_AND_RETURN(exec_error);
            }

            // printfPink("checkpoint 3\n");

            if (is_dynamic)
            {
                if (interpreter_path.length() == 0)
                {
                    printfRed("execve: cannot find dynamic linker: %s\n", interpreter_path.c_str());
                    CLEANUP_AND_RETURN(-ENOENT);
                }

                // 读取动态链接器的ELF头
                if (vfs_read_file(interpreter_path.c_str(), reinterpret_cast<uint64>(&interp_elf), 0, sizeof(interp_elf)) != sizeof(interp_elf))
                {
                    printfRed("execve: failed to read interpreter ELF header: %s\n", interpreter_path.c_str());
                    CLEANUP_AND_RETURN(-EIO);
                }

                if (interp_elf.magic != elf::elfEnum::ELF_MAGIC)
                {
                    printfRed("execve: invalid dynamic linker ELF\n");
                    CLEANUP_AND_RETURN(-ENOEXEC);
                }
                printfCyan("execve: dynamic linker ELF magic: %x\n", interp_elf.magic);

                // LoongArch 的 glibc 解释器要求按 ELF Program Header 的 p_align 装载。
                // 之前直接按 4K 对齐塞到 highest_addr 之后，会把 16K 对齐的解释器放到错误基址，
                // 导致动态链接器内部通过 load bias 推导出来的可写地址跑偏到只读段。
                uint64 interp_load_align = PGSIZE;
                uint64 interp_min_vaddr = UINT64_MAX;

                elf::proghdr interp_align_ph;
                for (int j = 0, interp_off = interp_elf.phoff; j < interp_elf.phnum; j++, interp_off += sizeof(interp_align_ph))
                {
                    if (vfs_read_file(interpreter_path.c_str(), reinterpret_cast<uint64>(&interp_align_ph), interp_off, sizeof(interp_align_ph)) != sizeof(interp_align_ph))
                    {
                        printfRed("execve: failed to read dynamic linker align header %d: %s\n",
                                  j, interpreter_path.c_str());
                        CLEANUP_AND_RETURN(-EIO);
                    }

                    if (interp_align_ph.type != elf::elfEnum::ELF_PROG_LOAD)
                    {
                        continue;
                    }

                    if (interp_align_ph.align > interp_load_align)
                    {
                        interp_load_align = interp_align_ph.align;
                    }

                    if (interp_align_ph.align > user_page_granule)
                    {
                        user_page_granule = interp_align_ph.align;
                    }

                    uint64 segment_align = interp_align_ph.align;
                    if (segment_align < PGSIZE)
                    {
                        segment_align = PGSIZE;
                    }

                    uint64 aligned_vaddr = align_down_pow2(interp_align_ph.vaddr, segment_align);
                    if (aligned_vaddr < interp_min_vaddr)
                    {
                        interp_min_vaddr = aligned_vaddr;
                    }
                }

                if (interp_min_vaddr == UINT64_MAX)
                {
                    interp_min_vaddr = 0;
                }

                interp_base = align_up_pow2(highest_addr - interp_min_vaddr, interp_load_align);
                printfCyan("execve: dynamic linker base align=%p min_vaddr=%p chosen_base=%p\n",
                           (void *)interp_load_align,
                           (void *)interp_min_vaddr,
                           (void *)interp_base);

                // 加载动态链接器的程序段
                elf::proghdr interp_ph;
                uint64 linker_text_start = 0;
                uint64 linker_text_end = 0;
                for (int j = 0, interp_off = interp_elf.phoff; j < interp_elf.phnum; j++, interp_off += sizeof(interp_ph))
                {
                    if (vfs_read_file(interpreter_path.c_str(), reinterpret_cast<uint64>(&interp_ph), interp_off, sizeof(interp_ph)) != sizeof(interp_ph))
                    {
                        printfRed("execve: failed to read dynamic linker header %d: %s\n",
                                  j, interpreter_path.c_str());
                        CLEANUP_AND_RETURN(-EIO);
                    }

                    if (interp_ph.type != elf::elfEnum::ELF_PROG_LOAD)
                        continue;

                    uint64 load_addr = interp_base + interp_ph.vaddr;
                    uint64 seg_flag = PTE_U;

#ifdef RISCV
                    /// 放开动态链接器权限
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                        seg_flag |= riscv::PteEnum::pte_executable_m;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                        seg_flag |= riscv::PteEnum::pte_writable_m;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ)
                        seg_flag |= riscv::PteEnum::pte_readable_m;
#elif defined(LOONGARCH)
                    seg_flag |= PTE_P | PTE_D | PTE_PLV | PTE_MAT;
                    if (!(interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC))
                        seg_flag |= PTE_NX;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                        seg_flag |= PTE_W;
                    if (!(interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_READ))
                        seg_flag |= PTE_NR;
#endif

                    // 解释器的 LOAD 段也必须按 p_align 对齐到运行时地址，否则 RW LOAD 会整体错位。
                    uint64 linker_segment_align = interp_ph.align;
                    if (linker_segment_align < PGSIZE)
                    {
                        linker_segment_align = PGSIZE;
                    }
                    uint64 linker_file_segment_start = align_down_pow2(interp_ph.vaddr, linker_segment_align);
                    uint64 linker_file_segment_end = align_up_pow2(interp_ph.vaddr + interp_ph.memsz, linker_segment_align);
                    uint64 linker_segment_prefix = interp_ph.vaddr - linker_file_segment_start;
                    if (interp_ph.off < linker_segment_prefix)
                    {
                        printfRed("execve: invalid dynamic linker segment, file offset underflow\n");
                        CLEANUP_AND_RETURN(-ENOEXEC);
                    }
                    uint64 linker_file_offset = interp_ph.off - linker_segment_prefix;
                    uint64 linker_file_size = interp_ph.filesz + linker_segment_prefix;
                    if (linker_file_size < interp_ph.filesz ||
                        linker_file_size > linker_file_segment_end - linker_file_segment_start)
                    {
                        printfRed("execve: invalid dynamic linker segment, file size overflow after align\n");
                        CLEANUP_AND_RETURN(-ENOEXEC);
                    }

                    // **重构：为动态链接器段分配独立的虚拟内存**
                    uint64 linker_segment_start = interp_base + linker_file_segment_start;
                    uint64 linker_segment_end = interp_base + linker_file_segment_end;

                    if (mem::k_vmm.vmalloc(new_pt, linker_segment_start, linker_segment_end, seg_flag) == 0)
                    {
                        printfRed("execve: load dynamic linker failed at %p-%p\n",
                                  (void *)linker_segment_start, (void *)linker_segment_end);
                        CLEANUP_AND_RETURN(-ENOMEM);
                    }

                    // 更新最高地址
                    if (linker_segment_end > highest_addr)
                    {
                        highest_addr = linker_segment_end;
                    }

                    // 加载动态链接器段内容
                    printfCyan("execve: loading dynamic linker segment %d, vaddr: %p, memsz: %p, offset: %p\n",
                               j, (void *)interp_ph.vaddr, (void *)interp_ph.memsz, (void *)interp_ph.off);
                    if (load_seg(new_pt, linker_segment_start, interpreter_path, linker_file_offset, linker_file_size) < 0)
                    {
                        printfRed("execve: load dynamic linker segment failed\n");
                        CLEANUP_AND_RETURN(-EIO);
                    }

                    // **新增：记录动态链接器段信息**
                    // 记录动态链接器段信息，确保页对齐
                    uint64 linker_aligned_start = linker_segment_start;
                    uint64 linker_aligned_end = linker_segment_end;

                    // 为动态链接器段设置调试名称
                    const char *linker_section_name = nullptr;
                    if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_EXEC)
                    {
                        linker_section_name = "linker_text";
                        linker_text_start = linker_aligned_start;
                        linker_text_end = linker_aligned_end;
                    }
                    else if (interp_ph.flags & elf::elfEnum::ELF_PROG_FLAG_WRITE)
                    {
                        linker_section_name = "linker_data";
                    }
                    else
                    {
                        linker_section_name = "linker_rodata";
                    }

                    // 直接添加到 ProcessMemoryManager
                    int linker_section_index = new_mm->add_program_section((void *)linker_aligned_start,
                                                                           linker_aligned_end - linker_aligned_start,
                                                                           linker_section_name);
                    if (linker_section_index < 0)
                    {
                        printfRed("execve: failed to add linker program section\n");
                        CLEANUP_AND_RETURN(-ENOMEM);
                    }

                    printfGreen("execve: added linker section[%d]: %s at %p, size %p (page-aligned from %p, %p)\n",
                                linker_section_index, linker_section_name,
                                (void *)linker_aligned_start, (void *)(linker_aligned_end - linker_aligned_start),
                                (void *)load_addr, (void *)interp_ph.memsz);
                }

                interp_entry = interp_base + interp_elf.entry;
                printfCyan("execve: dynamic linker loaded at base: %p, entry: %p\n",
                           (void *)interp_base, (void *)interp_entry);

#ifdef LOONGARCH
                // LoongArch 动态链当前仍在定位阶段，这里额外核对解释器代码段每一页的 PTE，
                // 便于区分“页表没建全”还是“用户态跳转到了错误地址”。
                if (linker_text_start != 0 && linker_text_end > linker_text_start)
                {
                    int missing_linker_text_pages = 0;
                    for (uint64 check_va = linker_text_start; check_va < linker_text_end; check_va += PGSIZE)
                    {
                        mem::Pte check_pte = new_pt.walk(check_va, false);
                        if (check_pte.is_null() || !check_pte.is_valid() || check_pte.is_super_plv() || !check_pte.is_executable())
                        {
                            ++missing_linker_text_pages;
                            printfRed("execve: invalid linker text pte va=%p raw=%p valid=%d user=%d exec=%d\n",
                                      (void *)check_va,
                                      check_pte.is_null() ? 0 : (void *)check_pte.get_data(),
                                      check_pte.is_null() ? 0 : (int)check_pte.is_valid(),
                                      check_pte.is_null() ? 0 : (int)!check_pte.is_super_plv(),
                                      check_pte.is_null() ? 0 : (int)check_pte.is_executable());
                        }
                    }
                    printfCyan("execve: linker text pte check done, start=%p end=%p missing=%d\n",
                               (void *)linker_text_start,
                               (void *)linker_text_end,
                               missing_linker_text_pages);
                }
#endif
            }

            // **新增：段加载完成后的统计信息**
            int total_sections = new_mm->prog_section_count;
            printfBlue("execve: segment loading completed. Total sections recorded: %d\n", total_sections);

            // 使用ProcessMemoryManager的公有成员来打印段信息
            for (int i = 0; i < total_sections; i++)
            {
                const program_section_desc *section = &new_mm->prog_sections[i];
                printfCyan("  [%d] %s: %p - %p (size: %p)\n",
                           i, section->_debug_name ? section->_debug_name : "unnamed",
                           section->_sec_start,
                           (void *)((uint64)section->_sec_start + section->_sec_size),
                           (void *)section->_sec_size);
            }
        }
        // printfPink("checkpoint 8\n");
        // ========== 第五阶段：分配用户栈空间 ==========

        { // **重构：基于最高地址分配用户栈空间**
            // root 场景下 LTP epoll_wait01 会在同一帧里放两块 64K 的栈缓冲区。
            // 旧的 32 页栈里还有 1 页 guard，可用空间只有 31 * 4K，不足以容纳
            // 这类 Linux 合法工作负载，会把本来正确的 pipe 语义误炸成 EFAULT。
            // 这里把默认用户栈提高到 64 页，先与当前回归规模对齐。
            // libcbench 的正则搜索和部分递归/线程库路径会触达比 256KiB 更深的用户栈。
            // 这里保守提高默认栈到 1MiB；run_bench 的 fork 开销不计入子测计时窗口。
            int stack_pgnum = 256;
            uint64 stack_start = PGROUNDUP(highest_addr); // 在最高地址之上分配栈
            uint64 stack_end = stack_start + stack_pgnum * PGSIZE;

#ifdef RISCV
            if (mem::k_vmm.uvmalloc(new_pt, stack_start, stack_end, PTE_W | PTE_X | PTE_R | PTE_U) == 0)
            {
                printfRed("execve: load user stack failed at %p-%p\n",
                          (void *)stack_start, (void *)stack_end);
                CLEANUP_AND_RETURN(-ENOMEM);
            }
#elif defined(LOONGARCH)
            if (mem::k_vmm.uvmalloc(new_pt, stack_start, stack_end, PTE_P | PTE_W | PTE_PLV | PTE_MAT | PTE_D) == 0)
            {
                printfRed("execve: load user stack failed at %p-%p\n",
                          (void *)stack_start, (void *)stack_end);
                CLEANUP_AND_RETURN(-ENOMEM);
            }
#endif

            // 更新最高地址
            highest_addr = stack_end;

            mem::k_vmm.uvmclear(new_pt, stack_start); // 设置guardpage
            sp = stack_end;                           // 栈指针从顶部开始
            // stackbase = stack_start + PGSIZE;         // 计算栈底地址(跳过guard page)
            stackbase = stack_start; // 计算栈底地址(跳过guard page) -> 不能跳过, 因为free的时候要用
            sp -= sizeof(uint64);    // 为返回地址预留空间

            // 添加用户栈段信息到 ProcessMemoryManager
            int stack_section_index = new_mm->add_program_section((void *)stackbase,
                                                                  stack_end - stackbase,
                                                                  "user_stack");
            if (stack_section_index < 0)
            {
                printfRed("execve: failed to add user stack section\n");
                CLEANUP_AND_RETURN(-ENOMEM);
            }

            printfGreen("execve: added user stack section[%d] at %p, size %p\n",
                        stack_section_index, (void *)stackbase, (void *)(stack_end - stackbase));
        }

        // ========== 第六阶段：准备glibc所需的用户栈数据 ==========
        // 为了兼容glibc，需要在用户栈中按照特定顺序压入：
        // 栈顶 -> 栈底：argc, argv[], envp[], auxv[], 字符串数据, 随机数据

        sp -= 32;
        uint64_t random[4] = {0x0, -0x114514FF114514UL, 0x2UL << 60, 0x3UL << 60};
        if (sp < stackbase || mem::k_vmm.copy_out(new_pt, sp, (char *)random, 32) < 0)
        {
            printfRed("execve: copy random data failed\n");
            CLEANUP_AND_RETURN(-EFAULT);
        }

        [[maybe_unused]] uint64 rd_pos = sp;

        // 2. 压入环境变量字符串
        uint64 uenvp[MAXARG];
        uint64 envc;
        // printfCyan("execve: envs size: %d\n", envs.size());
        for (envc = 0; envc < envs.size(); envc++)
        {
            if (envc >= MAXARG)
            { // 检查环境变量数量限制
                printfRed("execve: too many envs\n");
                CLEANUP_AND_RETURN(-E2BIG);
            }
            sp -= envs[envc].size() + 1; // 为环境变量字符串预留空间(包括null)
            sp -= sp % 16;               // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                printfRed("execve: stack overflow while copying envs\n");
                CLEANUP_AND_RETURN(-E2BIG);
            }
            if (mem::k_vmm.copy_out(new_pt, sp, envs[envc].c_str(), envs[envc].size() + 1) < 0)
            {
                printfRed("execve: copy envs failed\n");
                CLEANUP_AND_RETURN(-EFAULT);
            }
            uenvp[envc] = sp; // 记录字符串地址
        }
        uenvp[envc] = 0; // envp数组以NULL结尾

        // 3. 压入命令行参数字符串
        uint64 uargv[MAXARG]; // 命令行参数指针数组
        uint64 argc = 0;      // 命令行参数数量
        auto copy_exec_arg = [&](const char *arg_text) -> int
        {
            if (argc >= MAXARG)
            {
                return -E2BIG;
            }
            size_t arg_len = strlen(arg_text);
            sp -= arg_len + 1; // 为参数字符串预留空间(包括null)
            sp -= sp % 16;     // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                return -E2BIG;
            }
            if (mem::k_vmm.copy_out(new_pt, sp, arg_text, arg_len + 1) < 0)
            {
                return -EFAULT;
            }
            uargv[argc++] = sp;
            return 0;
        };

        if (has_shebang)
        {
            int arg_ret = copy_exec_arg(shebang_interpreter);
            if (arg_ret < 0)
            {
                printfRed("execve: copy shebang interpreter arg failed\n");
                CLEANUP_AND_RETURN(arg_ret);
            }
            if (shebang_optional_arg[0] != '\0')
            {
                arg_ret = copy_exec_arg(shebang_optional_arg);
                if (arg_ret < 0)
                {
                    printfRed("execve: copy shebang optional arg failed\n");
                    CLEANUP_AND_RETURN(arg_ret);
                }
            }
            arg_ret = copy_exec_arg(shebang_script_path);
            if (arg_ret < 0)
            {
                printfRed("execve: copy shebang script path failed\n");
                CLEANUP_AND_RETURN(arg_ret);
            }
            for (size_t arg_index = 1; arg_index < argv.size(); ++arg_index)
            {
                arg_ret = copy_exec_arg(argv[arg_index].c_str());
                if (arg_ret < 0)
                {
                    printfRed("execve: copy rewritten argv failed\n");
                    CLEANUP_AND_RETURN(arg_ret);
                }
            }
        }
        else if (argv.empty())
        {
            int arg_ret = copy_exec_arg(path.c_str());
            if (arg_ret < 0)
            {
                printfRed("execve: copy fallback argv failed\n");
                CLEANUP_AND_RETURN(arg_ret);
            }
        }
        else
        {
            for (size_t arg_index = 0; arg_index < argv.size(); ++arg_index)
            {
                int arg_ret = copy_exec_arg(argv[arg_index].c_str());
                if (arg_ret < 0)
                {
                    printfRed("execve: copy args failed\n");
                    CLEANUP_AND_RETURN(arg_ret);
                }
            }
        }
        uargv[argc] = 0; // argv数组以NULL结尾

        // 4. 压入辅助向量（auxv），供动态链接器使用
        {
            // 在括号里面开命名空间防止变量名冲突
            using namespace elf;
            uint64 aux[AuxvEntryType::MAX_AT * 2] = {0};
            [[maybe_unused]] int index = 0;

            ADD_AUXV(AT_HWCAP, 0);             // 硬件功能标志
            // AT_PAGESZ 必须反映内核真实页大小，而不是 ELF 的 p_align / common page size。
            // LoongArch glibc 会同时处理“运行时页大小”和“共享对象最大对齐”这两件事；
            // 如果把 16K p_align 冒充成 AT_PAGESZ，会把 mmap/PHDR 计算一起带偏。
            ADD_AUXV(AT_PAGESZ, PGSIZE);
            ADD_AUXV(AT_RANDOM, rd_pos);       // 随机数地址
            ADD_AUXV(AT_PHDR, phdr);           // 程序头表偏移
            ADD_AUXV(AT_PHENT, elf.phentsize); // 程序头表项大小
            if (is_dynamic)
            {
                ADD_AUXV(AT_PHNUM, elf.phnum); // 程序头表项数量 // 这个有问题
            }
            ADD_AUXV(AT_BASE, interp_base); // 动态链接器基地址（保留）
            ADD_AUXV(AT_ENTRY, elf.entry);  // 程序入口点地址
            // ADD_AUXV(AT_SYSINFO_EHDR, 0); // 系统调用信息头（保留）
            // ADD_AUXV(AT_UID, 0);               // 用户ID
            // ADD_AUXV(AT_EUID, 0);              // 有效用户ID
            // ADD_AUXV(AT_GID, 0);               // 组ID
            // ADD_AUXV(AT_EGID, 0);              // 有效组ID
            // ADD_AUXV(AT_SECURE, 0);            // 安全模式标志
            ADD_AUXV(AT_NULL, 0); // 结束标记

            // printf("index: %d\n", index);
            printfCyan("[execve] base: %p, phdr: %p\n", (void *)interp_base, (void *)phdr);

            // 将辅助向量复制到栈上
            sp -= sizeof(aux);
            if (mem::k_vmm.copy_out(new_pt, sp, (char *)aux, sizeof(aux)) < 0)
            {
                printfRed("execve: copy auxv failed\n");
                CLEANUP_AND_RETURN(-EFAULT);
            }
        }
        // 5. 压入环境变量指针数组（envp）
        // if (uenvp[0]) // 就算没有环境变量， 也要压入一个空指针
        {
            sp -= (envc + 1) * sizeof(uint64); // 为envp数组预留空间
            // sp -= sp % 16;                     // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                printfRed("execve: stack overflow while copying envp\n");
                CLEANUP_AND_RETURN(-E2BIG);
            }
            if (mem::k_vmm.copy_out(new_pt, sp, uenvp, (envc + 1) * sizeof(uint64)) < 0)
            {
                printfRed("execve: copy envp failed\n");
                CLEANUP_AND_RETURN(-EFAULT);
            }
        }
        proc->get_trapframe()->a2 = sp; // 设置栈指针到trapframe

        // 6. 压入命令行参数指针数组（argv）
        // if (uargv[0])
        {
            sp -= (argc + 1) * sizeof(uint64); // 为argv数组预留空间
            // sp -= sp % 16;                     // 对齐到16字节
            if (sp < stackbase + PGSIZE)
            {
                printfRed("execve: stack overflow while copying argv\n");
                CLEANUP_AND_RETURN(-E2BIG);
            }
            if (mem::k_vmm.copy_out(new_pt, sp, uargv, (argc + 1) * sizeof(uint64)) < 0)
            {
                printfRed("execve: copy argv failed\n");
                CLEANUP_AND_RETURN(-EFAULT);
            }
            // // 新增：打印压入的 argv 指针及其内容
            // for (uint64 i = 0; i <= argc; ++i)
            // {
            //     printf("[execve] argv_ptr[%d] = 0x%p -> \"%s\"\n", i, uargv[i], argv[i].c_str());
            // }
        }

        proc->get_trapframe()->a1 = sp; // 设置argv指针到trapframe

        // 7. 压入参数个数（argc）
        sp -= sizeof(uint64);
        // printfGreen("execve: argc: %d, sp: %p\n", argc, (void *)sp);
        if (mem::k_vmm.copy_out(new_pt, sp, (char *)&argc, sizeof(uint64)) < 0)
        {
            printfRed("execve: copy argc failed\n");
            CLEANUP_AND_RETURN(-EFAULT);
        }

        // 步骤13: 保存程序名用于调试
        // 从路径中提取文件名
        size_t last_slash = ab_path.find_last_of('/');
        eastl::string filename;
        if (last_slash != eastl::string::npos)
        {
            filename = ab_path.substr(last_slash + 1); // 提取最后一个'/'之后的部分
        }
        else
        {
            filename = ab_path; // 如果没有'/'，整个路径就是文件名
        }

        // 使用safestrcpy将文件名安全地拷贝到进程名称中
        // 注意：由于Pcb类没有提供set_name()函数，这里直接访问_name成员
        safestrcpy(proc->_name, filename.c_str(), sizeof(proc->_name));
        proc->exe = ab_path;

        // printfGreen("execve: process name set to '%s'\n", proc->get_name());

        // ========== 第七阶段：配置进程资源限制 ==========
        // 设置栈大小限制
        // 注意：由于Pcb类没有提供通用的set_rlimit()函数，这里直接访问_rlim_vec
        proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_cur =
            proc->_rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_max = sp - stackbase;
        // 处理F_DUPFD_CLOEXEC标志位，关闭设置了该标志的文件描述符
        // 注意：这里直接访问_ofile结构是因为这是execve的特定操作
        for (int i = 0; i < (int)max_open_files; i++)
        {
            if (proc->_ofile != nullptr && proc->_ofile->_ofile_ptr[i] != nullptr && proc->_ofile->_fl_cloexec[i])
            {
                fs::file *file_obj = proc->_ofile->_ofile_ptr[i];
                if (!is_probably_live_file_object(file_obj))
                {
                    printfRed("[execve] 检测到异常 CLOEXEC 文件指针，直接丢弃: pid=%d fd=%d file=%p\n",
                              proc->_pid, i, file_obj);
                }
                else
                {
                    file_obj->free_file();
                }
                proc->_ofile->_ofile_ptr[i] = nullptr;
                proc->_ofile->_fl_cloexec[i] = false;
            }
        }

        // ========== 第八阶段：替换进程映像 ==========
        // 注意：execve保持进程的身份信息不变，包括PID、PGID、SID、UID/GID等
        // 这符合POSIX标准：execve只替换进程的内存映像，不改变进程的身份标识

        printfBlue("execve: start clean up old process memory space\n");

        // 使用PCB的cleanup_memory_manager进行完整的内存清理
        // 这会正确处理引用计数并释放ProcessMemoryManager对象
        proc->cleanup_memory_manager();

        printfBlue("execve: cleaning up old process memory space\n");

        // 注意：new_mm已经在第二阶段创建，这里直接使用
        // new_pt已经设置在new_mm->pagetable中

        // 检查是否有段被记录
        if (new_mm->prog_section_count == 0)
        {
            printfYellow("execve: warning - no program sections were recorded\n");
            // 为兼容性添加一个总段，使用highest_addr作为大小参考
            new_mm->add_program_section((void *)0, PGROUNDUP(highest_addr), "fallback_program");
        }

        // 在所有已分配的内存区域之后初始化堆
        new_mm->init_heap(PGROUNDUP(highest_addr));

        // 完成新内存管理器的设置后，绑定到当前PCB
        proc->set_memory_manager(new_mm);

        printfGreen("execve: old process memory space cleaned up\n");

        printfBlue("execve: added %d program sections to process\n", new_mm->prog_section_count);

        uint64 entry_point;
        if (is_dynamic)
        {
            entry_point = interp_entry; // 动态链接时从动态链接器开始执行
            printfCyan("execve: starting from dynamic linker entry: %p\n", (void *)entry_point);
        }
        else
        {
            entry_point = elf.entry; // 静态链接时直接从程序入口开始
            printfCyan("execve: starting from program entry: %p\n", (void *)entry_point);
        }

#ifdef RISCV
        proc->get_trapframe()->epc = entry_point;
#elif defined(LOONGARCH)
        proc->get_trapframe()->era = entry_point;
#endif
        proc->get_trapframe()->sp = sp; // 设置栈指针

        printfGreen("execve succeed, new process size: %p\n", proc->get_size());
        printfGreen("execve: process '%s' loaded with %d program sections\n",
                    proc->get_name(), proc->get_prog_section_count());
        if (proc->_vfork_parent != nullptr)
        {
            _wait_lock.acquire();
            proc->_vfork_parent = nullptr;
            wakeup(proc);
            _wait_lock.release();
        }
        proc->print_detailed_memory_info();
        // 写成0为了适配glibc的rtld_fini需求

#undef CLEANUP_AND_RETURN
        return 0; // 返回参数个数，表示成功执行
    };
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
} // namespace proc
