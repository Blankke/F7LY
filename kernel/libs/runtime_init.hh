#pragma once

namespace runtime
{
// RISC-V 裸加载入口在使用任何 BSS 全局状态前调用。多 hart 可以同时进入，
// 但只有一个负责清零，其余等待；启动栈必须位于清零区间之外。
void initialize_zero_storage_once();

// 所有 CPU 都可在进入 C++ 主流程前调用。第一个到达者执行构造，其余 CPU
// 等待完成；状态放在非零初始化的 .data 中，不依赖尚未构造的 C++ atomic。
// 此阶段 HMM 尚未建立，全局构造函数只能初始化 POD、锁或空容器，禁止分配堆。
void initialize_global_objects_once();
bool global_constructors_ready();
} // namespace runtime
