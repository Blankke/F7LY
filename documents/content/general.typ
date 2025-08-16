#import "../components/figure.typ": algorithm-figure, code-figure

= 概述

本文档展示了F7LY-OS操作系统内核的设计与实现。本章节提供了一个完整的模板示例，包括文本格式、图表、代码块等常用元素的使用方法。

== F7LY-OS内核整体设计

F7LY-OS是一个面向教学的微内核操作系统，采用模块化设计，具有以下特点：

- *微内核架构*：核心功能最小化，其他服务作为用户态进程运行
- *模块化设计*：各功能模块相对独立，便于理解和维护  
- *跨平台支持*：支持多种硬件架构
- *教学友好*：代码结构清晰，注释详细

=== 系统架构图

@fig-architecture 展示了F7LY-OS的整体架构。系统采用分层设计，从底层硬件抽象到上层应用接口。

#figure(
  rect(
    width: 100%,
    height: 200pt,
    fill: rgb("#f0f0f0"),
    stroke: 1pt,
    [
      #set align(center + horizon)
      #text(size: 14pt)[
        *F7LY-OS 系统架构*
        
        #v(10pt)
        
        应用层 → 系统调用接口 → 微内核 → 硬件抽象层 → 硬件
        
        #v(10pt)
        
        （此处应放置实际的架构图）
      ]
    ]
  ),
  caption: [F7LY-OS系统架构图],
) <fig-architecture>

*注意*：由于这是示例，这里使用了一个占位图片。在实际使用中，您应该替换为真实的架构图。

=== 内核模块组成

F7LY-OS内核主要包含以下核心模块：

#figure(
  table(
    columns: 3,
    [*模块名称*], [*功能描述*], [*实现状态*],
    [内存管理], [负责物理内存和虚拟内存管理], [✓ 已完成],
    [进程调度], [实现进程创建、切换和销毁], [✓ 已完成],
    [中断处理], [处理硬件中断和系统调用], [✓ 已完成],
    [文件系统], [提供文件操作接口], [🔄 开发中],
    [网络栈], [TCP/IP协议栈实现], [📋 计划中],
    [设备驱动], [各种硬件设备驱动], [🔄 开发中],
  ),
  caption: [内核模块组成表],
) <tab-modules>

== 代码示例

=== 内核初始化代码

以下是F7LY-OS内核的初始化代码示例：

#code-figure(
```c
#include "kernel.h"
#include "memory.h"
#include "scheduler.h"
#include "interrupt.h"

/**
 * 内核主入口函数
 * @param boot_info 引导信息结构体
 */
void kernel_main(boot_info_t* boot_info) {
    // 初始化早期控制台输出
    console_early_init();
    printk("F7LY-OS Kernel Starting...\n");
    
    // 初始化内存管理
    memory_init(boot_info->memory_map);
    printk("Memory management initialized\n");
    
    // 初始化中断系统
    interrupt_init();
    printk("Interrupt system initialized\n");
    
    // 初始化进程调度器
    scheduler_init();
    printk("Process scheduler initialized\n");
    
    // 创建初始用户进程
    process_t* init_process = create_process("init", PRIORITY_HIGH);
    if (init_process == NULL) {
        panic("Failed to create init process");
    }
    
    // 启用中断并开始调度
    enable_interrupts();
    printk("Kernel initialization complete\n");
    
    // 进入空闲循环
    while (1) {
        cpu_idle();
    }
}
```,
caption: [内核初始化主函数],
label-name: "code-kernel-init"
)

=== 内存分配算法

下面展示了一个简单的内存分配算法实现：

#code-figure(
```c
/**
 * 简单的内存分配器实现
 * 使用首次适应算法
 */
void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    // 对齐到最小分配单元
    size = ALIGN_UP(size, MIN_ALLOC_SIZE);
    
    memory_block_t* current = free_list_head;
    memory_block_t* prev = NULL;
    
    // 遍历空闲链表，寻找合适大小的块
    while (current != NULL) {
        if (current->size >= size) {
            // 找到合适的块
            if (current->size > size + sizeof(memory_block_t)) {
                // 分割块
                memory_block_t* new_block = 
                    (memory_block_t*)((char*)current + size + sizeof(memory_block_t));
                new_block->size = current->size - size - sizeof(memory_block_t);
                new_block->next = current->next;
                current->size = size;
                current->next = new_block;
            }
            
            // 从空闲链表中移除
            if (prev == NULL) {
                free_list_head = current->next;
            } else {
                prev->next = current->next;
            }
            
            current->magic = ALLOC_MAGIC;
            return (char*)current + sizeof(memory_block_t);
        }
        
        prev = current;
        current = current->next;
    }
    
    return NULL; // 分配失败
}
```,
caption: [内存分配算法实现],
label-name: "code-kmalloc"
)

=== 算法伪代码示例

使用算法图表来描述进程调度算法：

#algorithm-figure(
[
  *输入*：就绪队列 $Q$，当前时间片 $t$ \
  *输出*：下一个执行的进程 $P$

  1. *while* $Q$ 不为空 *do*
     1. $P <- Q$.dequeue()
     2. *if* $P$.priority $>$ current_priority *then*
        1. current_process $<- P$
        2. *return* $P$
     3. *else*
        1. $Q$.enqueue($P$)
  2. *if* 没有找到合适进程 *then*
     1. *return* idle_process
],
caption: [优先级调度算法],
label-name: "algo-priority-scheduler"
)

== 性能分析

=== 基准测试结果

下表显示了F7LY-OS在不同负载下的性能表现：

#figure(
  table(
    columns: 4,
    [*测试项目*], [*轻负载*], [*中负载*], [*重负载*],
    [进程切换 (μs)], [12.5], [15.8], [23.1],
    [内存分配 (μs)], [8.2], [11.4], [18.7],
    [系统调用 (μs)], [3.1], [4.2], [6.8],
    [中断响应 (μs)], [2.8], [3.5], [5.2],
  ),
  caption: [系统性能基准测试结果],
) <tab-performance>

从 @tab-performance 可以看出，F7LY-OS在各种负载条件下都保持了良好的性能表现。

=== Shell脚本示例

以下是用于编译和运行F7LY-OS的脚本：

#code-figure(
```bash
#!/bin/bash

# F7LY-OS 构建脚本
# 作者：F7LY团队

set -e  # 遇到错误立即退出

echo "开始构建F7LY-OS..."

# 清理之前的构建文件
echo "清理构建目录..."
rm -rf build/
mkdir -p build/

# 设置编译选项
export CC=gcc
export CFLAGS="-std=c11 -O2 -Wall -Wextra"
export LDFLAGS="-nostdlib -static"

# 编译内核
echo "编译内核..."
cd src/kernel
make clean
make all

# 编译用户程序
echo "编译用户程序..."
cd ../userland
make clean  
make all

# 创建磁盘映像
echo "创建磁盘映像..."
cd ../../tools
./create_image.sh

echo "构建完成！"
echo "运行方式：qemu-system-x86_64 -kernel build/kernel.bin"
```,
caption: [F7LY-OS构建脚本],
label-name: "code-build-script"
)

== 总结

本文档展示了Typst文档编写的各种元素使用方法，包括：

1. *文本格式*：粗体、斜体、等宽字体
2. *图表*：使用 `figure()` 函数插入图片和表格
3. *代码块*：使用 `code-figure()` 函数显示带标题的代码
4. *算法*：使用 `algorithm-figure()` 显示算法伪代码
5. *交叉引用*：使用 `@` 符号引用图表和代码
6. *数学公式*：行内公式 $x = y + z$ 和行间公式

更多高级功能请参考Typst官方文档。
