#pragma once

#include "types.hh"
#include <EASTL/map.h>
#include <EASTL/string.h>
#include "devs/spinlock.hh"

namespace intr_stats
{
    // 中断统计管理器
    class InterruptStatsManager
    {
    private:
        SpinLock _lock;
        eastl::map<int, uint64> _interrupt_counts; // 中断号 -> 计数
        
    public:
        /// @brief 初始化中断统计管理器
        void init();
        
        /// @brief 记录一次中断
        /// @param irq_num 中断号
        void record_interrupt(int irq_num);
        
        /// @brief 获取中断计数
        /// @param irq_num 中断号
        /// @return 该中断的计数
        uint64 get_interrupt_count(int irq_num);
        
        /// @brief 获取所有中断的统计信息
        /// @return 包含所有中断统计的字符串
        eastl::string get_interrupts_info();
        
        /// @brief 清除所有统计信息（用于调试）
        void clear_stats();
    };
    
    // 全局中断统计管理器实例
    extern InterruptStatsManager k_intr_stats;
}
