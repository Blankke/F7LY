#include "runtime_init.hh"

#include "__cxx_abi.hh"
#include "types.hh"

namespace
{
using Constructor = void (*)();

extern "C" Constructor __preinit_array_start[];
extern "C" Constructor __preinit_array_end[];
extern "C" Constructor __init_array_start[];
extern "C" Constructor __init_array_end[];
extern "C" uint64 bss_start[];
extern "C" uint64 bss_end[];

constexpr uint32 k_bss_pending = 0x42535330U; // "BSS0"
constexpr uint32 k_bss_running = 0x42535331U; // "BSS1"
constexpr uint32 k_bss_ready = 0x42535332U;   // "BSS2"

constexpr uint32 k_constructors_pending = 0x43505030U; // "CPP0"
constexpr uint32 k_constructors_running = 0x43505031U; // "CPP1"
constexpr uint32 k_constructors_ready = 0x43505032U;   // "CPP2"

// 必须落入 .data，而不是等待入口清零的 .bss。这样最早到达的 CPU 可以在
// 使用 EASTL atomic、虚表对象或任何其它 C++ 全局对象前完成一次性构造。
uint32 g_bss_state = k_bss_pending;
uint32 g_constructor_state = k_constructors_pending;

void run_range(Constructor *begin, Constructor *end)
{
    for (Constructor *entry = begin; entry < end; ++entry)
    {
        if (*entry != nullptr)
        {
            (*entry)();
        }
    }
}
} // namespace

namespace runtime
{
void initialize_zero_storage_once()
{
    uint32 expected = k_bss_pending;
    if (__atomic_compare_exchange_n(&g_bss_state, &expected, k_bss_running,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        for (uint64 *word = bss_start; word < bss_end; ++word)
        {
            *word = 0;
        }
        __atomic_store_n(&g_bss_state, k_bss_ready, __ATOMIC_RELEASE);
        return;
    }

    while (__atomic_load_n(&g_bss_state, __ATOMIC_ACQUIRE) == k_bss_running)
    {
        asm volatile("" ::: "memory");
    }
    if (__atomic_load_n(&g_bss_state, __ATOMIC_ACQUIRE) != k_bss_ready)
    {
        for (;;)
        {
            asm volatile("" ::: "memory");
        }
    }
}

void initialize_global_objects_once()
{
    uint32 expected = k_constructors_pending;
    if (__atomic_compare_exchange_n(&g_constructor_state, &expected,
                                    k_constructors_running, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        // GCC 生成的全局构造函数可能先调用 __cxa_atexit 登记析构函数。
        // 其固定容量链表同样依赖 BSS，必须由赢得一次性构造权的 CPU 先建立。
        __cxxabiv1::__init_atexit_func_entry();
        run_range(__preinit_array_start, __preinit_array_end);
        run_range(__init_array_start, __init_array_end);
        __atomic_store_n(&g_constructor_state, k_constructors_ready, __ATOMIC_RELEASE);
        return;
    }

    // 次核不能越过这里读取只构造了一半的虚表/容器。若状态不是 running/ready，
    // 说明固件没有按链接脚本装载 .data，继续执行只会产生更隐蔽的内存破坏。
    while (__atomic_load_n(&g_constructor_state, __ATOMIC_ACQUIRE) ==
           k_constructors_running)
    {
        asm volatile("" ::: "memory");
    }
    if (__atomic_load_n(&g_constructor_state, __ATOMIC_ACQUIRE) !=
        k_constructors_ready)
    {
        for (;;)
        {
            asm volatile("" ::: "memory");
        }
    }
}

bool global_constructors_ready()
{
    return __atomic_load_n(&g_constructor_state, __ATOMIC_ACQUIRE) ==
           k_constructors_ready;
}
} // namespace runtime
