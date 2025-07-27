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
 * - 内存资源的统一释放接口
 * 
 * 设计目标：
 * - 将散落在各处的内存管理逻辑集中管理
 * - 提供清晰的内存管理接口
 * - 支持内存调试和一致性检查
 * - 便于内存泄漏检测和问题定位
 */

#pragma once

#include "types.hh"
#include "proc.hh"
#include "mem.hh"  // 为MAP_SHARED、PROT_WRITE等常量
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif

namespace proc {

/**
 * @brief 进程内存管理器
 * 
 * 负责统一管理进程的内存资源，提供内存分配、释放和管理的完整接口。
 * 这个类封装了程序段、堆内存和VMA的管理逻辑，避免内存管理代码散落在各处。
 */
class ProcessMemoryManager {
public:
    /**
     * @brief 构造函数
     * @param pcb 关联的进程控制块指针
     */
    explicit ProcessMemoryManager(Pcb* pcb);

    /**
     * @brief 析构函数，确保资源正确释放
     */
    ~ProcessMemoryManager();

    /****************************************************************************************
     * 程序段管理接口
     ****************************************************************************************/
    
    /**
     * @brief 释放所有程序段占用的内存
     * 
     * 遍历进程的所有程序段，释放其占用的物理内存和虚拟地址映射。
     * 这个函数会调用vmunmap来取消页表映射，并释放物理页面。
     */
    void free_all_program_sections();

    /**
     * @brief 释放指定程序段的内存
     * @param section_index 程序段索引
     * @return true 释放成功，false 失败（索引无效等）
     */
    bool free_program_section(int section_index);

    /**
     * @brief 验证程序段内存的一致性
     * @return true 一致，false 不一致
     */
    bool verify_program_sections_consistency() const;

    /****************************************************************************************
     * 堆内存管理接口
     ****************************************************************************************/
    
    /**
     * @brief 释放堆内存
     * 
     * 释放进程堆内存占用的所有页面，将堆大小重置为0。
     * 这个函数会调用vmunmap来取消页表映射。
     */
    void free_heap_memory();

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
     * @brief 清理堆内存到指定大小
     * @param new_size 新的堆大小，如果为0则完全清理
     * @return true 成功，false 失败
     */
    bool cleanup_heap_to_size(uint64 new_size);

    /****************************************************************************************
     * VMA管理接口
     ****************************************************************************************/
    
    /**
     * @brief 释放所有VMA资源
     * 
     * 释放进程的所有虚拟内存区域，包括：
     * - 文件映射的写回操作（对于MAP_SHARED且可写的映射）
     * - 释放文件引用
     * - 取消虚拟地址映射
     * - 清理VMA结构体
     */
    void free_all_vma();

    /**
     * @brief 释放指定的VMA
     * @param vma_index VMA索引
     * @return true 释放成功，false 失败（索引无效等）
     */
    bool free_vma(int vma_index);

    /**
     * @brief 处理VMA的写回操作
     * @param vma_index VMA索引
     * @return true 成功，false 失败
     */
    bool writeback_vma(int vma_index);

    /**
     * @brief 减少VMA引用计数，如果计数为0则释放
     * @return true 已释放VMA，false VMA仍被引用
     */
    bool decrease_vma_refcount_and_free();

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
     * @brief 释放进程页表
     * 
     * 释放进程的页表结构，包括：
     * - 取消trampoline、trapframe等特殊页面的映射
     * - 释放页表目录
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
     ****************************************************************************************/
    
    /**
     * @brief 释放进程的所有内存资源
     * 
     * 这是进程退出时的主要内存清理函数，按顺序释放：
     * 1. VMA资源（包括文件映射的写回）
     * 2. 程序段内存
     * 3. 堆内存
     * 4. 页表结构
     * 5. trapframe等特殊页面
     */
    void free_all_memory();

    /**
     * @brief 紧急内存清理（用于错误恢复）
     * 
     * 在出现错误时进行的快速内存清理，不进行写回操作。
     */
    void emergency_cleanup();

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
     */
    bool verify_all_memory_consistency() const;

    /**
     * @brief 获取进程总内存使用量（不包括VMA）
     * @return 内存使用量（字节）
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

private:
    Pcb* _pcb;  ///< 关联的进程控制块
    
    /****************************************************************************************
     * 内部辅助函数
     ****************************************************************************************/
    
    /**
     * @brief 检查页面是否已映射
     * @param va 虚拟地址
     * @return true 已映射，false 未映射
     */
    bool is_page_mapped(uint64 va) const;

    /**
     * @brief 安全地取消单个页面的映射
     * @param va 虚拟地址
     * @return true 成功，false 失败
     */
    bool safe_unmap_page(uint64 va);

    /**
     * @brief 写回文件映射的数据
     * @param vma_entry VMA条目
     * @return true 成功，false 失败
     */
    bool writeback_file_mapping(const vma& vma_entry);

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

} // namespace proc
