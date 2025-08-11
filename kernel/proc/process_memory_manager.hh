/**
 * @file process_memory_manager.hh
 * @brief 进程内存管理器
 *
 * 该文件定义了进程内存管理器类，用于统一管理进程的内存资源，
 * 包括程序段、堆内存、VMA(虚拟内存区域)的分配、释放和管理。
 *
 * 主要功能：
 * - 程序段内存管理（代码段、数据段等）
 * - 堆内存的分配与释放
 * - VMA(虚拟内存区域)的创建、映射和清理
 * - 内存一致性检查和调试信息输出
 * - 统一的内存资源释放接口（通过free_all_memory()）
 */

#pragma once

#include "types.hh"
#include "proc.hh"
#include "mem.hh" // 为MAP_SHARED、PROT_WRITE等常量
#include "devs/spinlock.hh"
#include <EASTL/atomic.h>
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif

// 结构体定义
namespace proc
{
    class Pcb;
    constexpr int max_program_section_num = 16; // 确保常量可用

    struct program_section_desc
    {
        void *_sec_start = nullptr; // virtual address
        ulong _sec_size = 0;
        const char *_debug_name = nullptr;
    };

    constexpr int NVMA = 30; // 每个进程最多的虚拟内存区域数量

    // VMA结构体定义（从proc.hh移动过来）
    struct VMA
    {
        vma _vm[NVMA]; // 虚拟内存区域数组，类似Linux的vm_area_struct
        // int _ref_cnt;  // VMA引用计数，用于copy-on-write机制
    };
}

namespace proc
{

    /**
     * @brief 进程内存管理器
     *
     * 负责统一管理进程的内存资源，提供内存分配、释放和管理的完整接口。
     * 这个类封装了程序段、堆内存和VMA的管理逻辑，避免内存管理代码散落在各处。
     *
     * 阶段0重构：增加了原子引用计数支持，为线程共享内存做准备。
     */
    class ProcessMemoryManager
    {
    public:
        /****************************************************************************************
         * 内存字段
         ****************************************************************************************/
        // 程序段管理
        program_section_desc prog_sections[max_program_section_num];
        int prog_section_count;

        // 堆内存管理
        uint64 heap_start;
        uint64 heap_end;

        // 页表管理（移除分散的引用计数）
        mem::PageTable pagetable;

        // VMA管理（移除分散的引用计数）
        VMA vma_data;

        // 共享标志
        bool shared_vm;

    private:
        // 内存大小
        uint64 total_memory_size;

    public:
        /**
         * @brief 增加引用计数
         *
         * 线程安全的原子操作，用于共享内存时增加引用计数。
         */
        void get();

        /**
         * @brief 减少引用计数
         * @return true 如果引用计数降至0需要释放，false 仍有其他引用
         *
         * 线程安全的原子操作，当引用计数降至0时返回true，调用者应当释放资源。
         */
        bool put();

        /**
         * @brief 构造函数
         */
        ProcessMemoryManager();

        /**
         * @brief 析构函数，确保资源正确释放
         */
        ~ProcessMemoryManager();

        /**
         * @brief 获取当前引用计数（仅用于调试）
         * @return 当前引用计数值
         */
        int get_ref_count() const;

        /**
         * @brief 为线程创建共享内存（增加引用计数）
         * @return 返回当前对象指针，引用计数+1
         */
        ProcessMemoryManager *share_for_thread();

        /**
         * @brief 为进程创建完全复制的内存管理器
         * @return 新的ProcessMemoryManager实例，内容为深拷贝
         */
        ProcessMemoryManager *clone_for_fork();

        /****************************************************************************************
         * 程序段管理接口
         ****************************************************************************************/

        /**
         * @brief 添加程序段
         * @param start 程序段起始地址
         * @param size 程序段大小
         * @param name 程序段名称（调试用）
         * @return 程序段索引，失败返回-1
         */
        int add_program_section(void *start, ulong size, const char *name = nullptr);

        /**
         * @brief 移除指定索引的程序段
         * @param index 程序段索引
         */
        void remove_program_section(int index);

        /**
         * @brief 清空所有程序段
         */
        void clear_all_program_sections_data();

        /**
         * @brief 重置所有内存管理信息
         */
        void reset_memory_sections();

        /**
         * @brief 从另一个ProcessMemoryManager复制程序段信息
         * @param src 源ProcessMemoryManager
         */
        void copy_program_sections(const ProcessMemoryManager &src);

        /**
         * @brief 释放所有程序段占用的内存
         *
         * 遍历进程的所有程序段，释放其占用的物理内存和虚拟地址映射。
         * 这个函数会调用vmunmap来取消页表映射，并释放物理页面。
         *
         * 注意：此函数仅在free_all_memory()中内部调用，不建议单独使用
         */
        void free_all_program_sections();

        /****************************************************************************************
         * 堆内存管理接口
         ****************************************************************************************/

        /**
         * @brief 初始化堆
         * @param start_addr 堆的起始地址
         */
        void init_heap(uint64 start_addr);

        /**
         * @brief 扩展堆内存
         * @param new_end 新的堆结束地址
         * @return 实际的堆结束地址，如果分配失败返回原来的地址
         */
        uint64 grow_heap(uint64 new_end);

        /**
         * @brief 收缩堆内存
         * @param new_end 新的堆结束地址
         * @return 实际的堆结束地址
         */
        uint64 shrink_heap(uint64 new_end);

        /**
         * @brief 释放堆内存
         *
         * 释放进程堆内存占用的所有页面，将堆大小重置为0。
         *
         * 注意：此函数内部调用cleanup_heap_to_size(0)，仅在free_all_memory()中内部调用
         */
        void free_heap_memory();
        /**
         * @brief 清理堆内存到指定大小
         * @param new_size 新的堆大小，如果为0则完全释放堆内存
         * @return true 成功，false 失败
         */
        bool cleanup_heap_to_size(uint64 new_size);

        /****************************************************************************************
         * VMA管理接口
         *
         * 说明：VMA管理现在统一通过free_all_memory()进行，不建议单独调用
         ****************************************************************************************/

        /**
         * @brief 释放单个VMA资源
         * @param vma_index VMA索引
         *
         * 释放指定索引的VMA，包括：
         * - 文件映射的写回操作（对于MAP_SHARED且可写的映射）
         * - 释放文件引用
         * - 取消虚拟地址映射
         * - 清理VMA结构体
         */
        void free_single_vma(int vma_index);

        /**
         * @brief 释放所有VMA资源
         *
         * 释放进程的所有虚拟内存区域，包括：
         * - 文件映射的写回操作（对于MAP_SHARED且可写的映射）
         * - 释放文件引用
         * - 取消虚拟地址映射
         * - 清理VMA结构体
         *
         * 注意：此函数仅在free_all_memory()中内部调用，不建议单独使用
         */
        void free_all_vma();

        /**
         * @brief 取消指定地址范围的内存映射（支持munmap系统调用）
         * @param addr 起始地址
         * @param length 长度
         * @return 0 成功，-1 失败
         */
        int unmap_memory_range(void *addr, size_t length);

        /**
         * @brief 查找覆盖指定地址范围的VMA
         * @param start_addr 起始地址
         * @param end_addr 结束地址
         * @param overlapping_vmas 输出参数：重叠的VMA索引列表
         * @return 找到的重叠VMA数量
         */
        int find_overlapping_vmas(uint64 start_addr, uint64 end_addr, int overlapping_vmas[], int max_count);

        /**
         * @brief 部分取消VMA映射
         * @param vma_index VMA索引
         * @param unmap_start 取消映射的起始地址
         * @param unmap_end 取消映射的结束地址
         * @return true 成功，false 失败
         */
        bool partial_unmap_vma(int vma_index, uint64 unmap_start, uint64 unmap_end);

        /****************************************************************************************
         * 页表管理接口
         ****************************************************************************************/

        /**
         * @brief 为进程创建页表
         * @param pcb 进程控制块指针
         * @return 创建成功返回true，失败返回false
         *
         * 创建包含trampoline、trapframe等基础映射的页表
         */
        bool create_pagetable();

        /**
         * @brief 释放进程页表
         *
         * 释放进程的页表结构，包括：
         * - 取消trampoline、trapframe等特殊页面的映射
         * - 释放页表目录
         *
         * 注意：此函数仅在free_all_memory()中内部调用，不建议单独使用
         */
        void free_pagetable();

        /**
         * @brief 安全的虚拟内存取消映射
         * @param va_start 起始虚拟地址
         * @param va_end 结束虚拟地址
         * @param check_validity 是否检查页面有效性
         */
        void safe_vmunmap(uint64 va_start, uint64 va_end, bool check_validity = true);

        /****************************************************************************************
         * 统一内存释放接口
         *
         * 说明：
         * - free_all_memory(): 完整的内存清理流程（正常进程退出使用）
         * - emergency_cleanup(): 快速错误恢复清理（不进行写回等耗时操作）
         * - cleanup_execve_pagetable(): 静态方法，专门用于execve失败时的页表清理
         ****************************************************************************************/

        /**
         * @brief 释放进程的所有内存资源
         *
         * 这是进程退出时的主要内存清理函数，按顺序释放：
         * 1. VMA资源（包括文件映射的写回）
         * 2. 程序段内存
         * 3. 堆内存
         * 4. 页表结构
         */
        void free_all_memory();

        /**
         * @brief 紧急内存清理（用于错误恢复）
         *
         * 在出现错误时进行的快速内存清理，不进行写回操作。
         */
        void emergency_cleanup();

        /**
         * @brief 专门用于execve失败时的页表清理
         *
         * 在execve加载过程中出现错误时，安全地清理已分配的页表和内存。
         * 这个函数不依赖PCB状态，直接操作页表进行清理。
         *
         * @param pagetable 要清理的页表
         * @param section_descs 已分配的程序段描述数组
         * @param section_count 程序段数量
         */
        static void cleanup_execve_pagetable(mem::PageTable &pagetable,
                                             const program_section_desc *section_descs,
                                             int section_count);

        /****************************************************************************************
         * 内存调试和监控接口
         ****************************************************************************************/

        /**
         * @brief 打印详细的内存使用信息
         */
        void print_memory_usage() const;

        /**
         * @brief 验证所有内存区域的一致性
         * @return true 一致，false 存在问题
         *
         * 调用verify_program_sections_consistency()和其他一致性检查
         */
        bool verify_all_memory_consistency() const;

        /**
         * @brief 获取进程总内存使用量（不包括VMA）
         * @return 内存使用量（字节）
         *
         * 直接返回total_memory_size字段值，等价于calculate_total_memory_size()
         */
        uint64 get_total_memory_usage() const;

        /**
         * @brief 获取VMA总内存使用量
         * @return VMA内存使用量（字节）
         */
        uint64 get_vma_memory_usage() const;

        /**
         * @brief 检查是否存在内存泄漏
         * @return true 存在泄漏，false 无泄漏
         */
        bool check_memory_leaks() const;

        /****************************************************************************************
         * 内存大小计算和一致性检查接口
         *
         * 说明：
         * - calculate_total_memory_size(): 实时计算程序段+堆大小
         * - get_total_memory_usage(): 返回缓存的total_memory_size字段值
         * - get_total_program_memory(): 计算程序段总大小（在calculate_total_memory_size中包含）
         * - get_vma_memory_usage(): 计算VMA总大小（独立统计）
         ****************************************************************************************/

        /**
         * @brief 更新总内存大小
         */
        void update_total_memory_size();

        /**
         * @brief 计算总内存大小（包括程序段和堆，不包括VMA）
         * @return 总内存大小（字节）
         */
        uint64 calculate_total_memory_size() const;

        /**
         * @brief 获取程序段总内存大小
         * @return 程序段总内存大小（字节）
         *
         * 注意：此函数功能已包含在calculate_total_memory_size()中，为保持API兼容性而保留
         */
        uint64 get_total_program_memory() const;

        /**
         * @brief 验证内存一致性
         * @return true 一致，false 不一致
         *
         * 验证total_memory_size与实际计算值是否一致
         */
        bool verify_memory_consistency();

        /**
         * @brief 验证程序段内存的一致性
         * @return true 一致，false 不一致
         */
        bool verify_program_sections_consistency() const;

    private:
        /****************************************************************************************
         * 引用计数和线程支持
         ****************************************************************************************/
        eastl::atomic<int> ref_count; // 原子引用计数，用于线程间安全共享
        SpinLock memory_lock;         // 内存操作锁，保护并发访问

    private:
        /****************************************************************************************
         * 内部辅助函数
         ****************************************************************************************/

        /**
         * @brief 检查页面是否已映射
         * @param va 虚拟地址
         * @return true 已映射，false 未映射
         */
        bool is_page_mapped(uint64 va);
        
        /**
         * @brief 写回文件映射的数据
         * @param vma_entry VMA条目
         * @return true 成功，false 失败
         */
        bool writeback_file_mapping(const vma &vma_entry);

        /**
         * @brief 检查VMA条目是否有效
         * @param vma_index VMA索引
         * @return true 有效，false 无效
         */
        bool is_vma_valid(int vma_index) const;

        /**
         * @brief 计算地址范围的页面数量
         * @param start_addr 起始地址
         * @param size 大小
         * @return 页面数量
         */
        uint64 calculate_page_count(uint64 start_addr, uint64 size) const;

        /**
         * @brief 对齐地址到页边界
         * @param addr 地址
         * @param round_up 是否向上对齐
         * @return 对齐后的地址
         */
        uint64 align_to_page(uint64 addr, bool round_up = true) const;
    };

    // 类型别名，便于后续重命名为MemoryDescriptor
    using MemoryDescriptor = ProcessMemoryManager;

} // namespace proc
