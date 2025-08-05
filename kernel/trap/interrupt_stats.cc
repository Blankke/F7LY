#include "interrupt_stats.hh"
#include "libs/klib.hh"
#include "printer.hh"

namespace intr_stats
{
    InterruptStatsManager k_intr_stats;
    
    void InterruptStatsManager::init()
    {
        _lock.init("intr_stats");
        _interrupt_counts.clear();
        printfGreen("[INTR_STATS] Interrupt Statistics Manager initialized\n");
    }
    
    void InterruptStatsManager::record_interrupt(int irq_num)
    {
        _lock.acquire();
        
        // 如果这个中断号还没有记录，初始化为0
        if (_interrupt_counts.find(irq_num) == _interrupt_counts.end())
        {
            _interrupt_counts[irq_num] = 0;
        }
        
        // 增加计数
        // printfGray("[INTR_STATS] Recording interrupt: IRQ %d\n", irq_num);
        _interrupt_counts[irq_num]++;
        
        _lock.release();
    }
    
    uint64 InterruptStatsManager::get_interrupt_count(int irq_num)
    {
        _lock.acquire();
        
        uint64 count = 0;
        auto it = _interrupt_counts.find(irq_num);
        if (it != _interrupt_counts.end())
        {
            count = it->second;
        }
        
        _lock.release();
        return count;
    }
    
    eastl::string InterruptStatsManager::get_interrupts_info()
    {
        _lock.acquire();
        
        eastl::string result;
        
        // 如果没有任何中断记录，返回空字符串
        if (_interrupt_counts.empty())
        {
            _lock.release();
            return result;
        }
        
        // 按中断号排序输出
        for (auto it = _interrupt_counts.begin(); it != _interrupt_counts.end(); ++it)
        {
            int irq_num = it->first;
            uint64 count = it->second;
            
            // 格式：中断号:        计数\n
            // 转换中断号到字符串
            int temp_irq = irq_num;
            char irq_str[16];
            int irq_len = 0;
            if (temp_irq == 0)
            {
                irq_str[irq_len++] = '0';
            }
            else
            {
                char temp_digits[16];
                int temp_len = 0;
                while (temp_irq > 0)
                {
                    temp_digits[temp_len++] = '0' + (temp_irq % 10);
                    temp_irq /= 10;
                }
                // 反转数字
                for (int i = temp_len - 1; i >= 0; i--)
                {
                    irq_str[irq_len++] = temp_digits[i];
                }
            }
            irq_str[irq_len] = '\0';
            
            // 转换计数到字符串
            uint64 temp_count = count;
            char count_str[32];
            int count_len = 0;
            if (temp_count == 0)
            {
                count_str[count_len++] = '0';
            }
            else
            {
                char temp_digits[32];
                int temp_len = 0;
                while (temp_count > 0)
                {
                    temp_digits[temp_len++] = '0' + (temp_count % 10);
                    temp_count /= 10;
                }
                // 反转数字
                for (int i = temp_len - 1; i >= 0; i--)
                {
                    count_str[count_len++] = temp_digits[i];
                }
            }
            count_str[count_len] = '\0';
            
            // 构建行：IRQ:        COUNT\n
            result += irq_str;
            result += ":        ";
            result += count_str;
            result += "\n";
        }
        
        _lock.release();
        return result;
    }
    
    void InterruptStatsManager::clear_stats()
    {
        _lock.acquire();
        _interrupt_counts.clear();
        _lock.release();
    }
}
