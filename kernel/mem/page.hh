#pragma once

// 两种架构当前都采用 4 KiB 基础页。页大小是内存子系统契约，不属于某块
// 开发板，也不应分别复制到每个架构头中。
#define PGSIZE 4096
#define PGSHIFT 12

// 向上取整得到覆盖 [0, size) 所需的页边界；向下取整得到地址所在页的起点。
#define PGROUNDUP(size) (((size) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(address) ((address) & ~(PGSIZE - 1))
