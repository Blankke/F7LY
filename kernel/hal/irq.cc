#include "hal/irq.hh"

#include "hal/irq_backend.hh"
#include "printer.hh"

namespace hal::irq
{
namespace
{
    // PCI INTx 可能让多个设备共享一个 hwirq。固定四槽覆盖当前块设备和网卡，
    // 同时保持中断路径无堆分配、无链表和确定的遍历上界。
    constexpr uint32 k_handlers_per_source = 4;

    struct HandlerSlot
    {
        Handler handler;
        void *context;
        const char *name;
    };

    // 以下对象均依靠 BSS 清零，不要求链接脚本执行 .init_array。
    HandlerSlot g_handlers[k_max_sources][k_handlers_per_source];
    uint64 g_registered_sources;
    uint64 g_unhandled_reported;
    uint8 g_registry_lock;
    bool g_global_initialized;
    bool g_invalid_claim_reported;

    void lock_registry()
    {
        while (__atomic_test_and_set(&g_registry_lock, __ATOMIC_ACQUIRE))
        {
            asm volatile("" ::: "memory");
        }
    }

    void unlock_registry()
    {
        __atomic_clear(&g_registry_lock, __ATOMIC_RELEASE);
    }

    uint32 first_set_source(uint64 sources)
    {
        // freestanding 链接不保证提供 libgcc 的 __ctzdi2。公共入口只在
        // sources!=0 时调用，最多检查 64 位，确定性循环也更容易跨架构审计。
        uint32 source = 0;
        while ((sources & 1ULL) == 0)
        {
            sources >>= 1;
            ++source;
        }
        return source;
    }
} // namespace

bool register_handler(Source source, Handler handler, void *context,
                      const char *name)
{
    if (source >= k_max_sources || handler == nullptr ||
        !backend::supports_source(source))
    {
        return false;
    }

    bool first_handler = false;
    bool controller_ready = false;
    lock_registry();

    HandlerSlot *free_slot = nullptr;
    for (uint32 index = 0; index < k_handlers_per_source; ++index)
    {
        HandlerSlot &slot = g_handlers[source][index];
        Handler installed = __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (installed == handler && slot.context == context)
        {
            unlock_registry();
            return true;
        }
        if (installed == nullptr && free_slot == nullptr)
        {
            free_slot = &slot;
        }
    }

    if (free_slot == nullptr)
    {
        unlock_registry();
        return false;
    }

    first_handler = (g_registered_sources & (1ULL << source)) == 0;
    free_slot->context = context;
    free_slot->name = name != nullptr ? name : "unnamed";
    // handler 最后以 release 语义发布，dispatch 看到它时 context/name 已就绪。
    __atomic_store_n(&free_slot->handler, handler, __ATOMIC_RELEASE);
    g_registered_sources |= 1ULL << source;
    controller_ready = __atomic_load_n(&g_global_initialized, __ATOMIC_ACQUIRE);
    if (first_handler && controller_ready)
    {
        backend::enable_source(source);
    }
    unlock_registry();

    if (first_handler && controller_ready)
    {
        platformDiagnosticInfo("[irq] source=%u enabled handler=%s\n",
                               source, free_slot->name);
    }
    return true;
}

void initialize_global()
{
    lock_registry();
    if (__atomic_load_n(&g_global_initialized, __ATOMIC_RELAXED))
    {
        unlock_registry();
        panic("irq global controller initialized twice");
    }

    backend::initialize_global();
    uint64 sources = g_registered_sources;
    while (sources != 0)
    {
        const Source source = first_set_source(sources);
        backend::enable_source(source);
        sources &= sources - 1;
    }
    __atomic_store_n(&g_global_initialized, true, __ATOMIC_RELEASE);
    const uint64 registered = g_registered_sources;
    unlock_registry();

    platformDiagnosticInfo("[irq] controller ready sources=0x%lx\n", registered);
}

void initialize_current_cpu()
{
    lock_registry();
    if (!__atomic_load_n(&g_global_initialized, __ATOMIC_ACQUIRE))
    {
        unlock_registry();
        panic("irq CPU context initialized before global controller");
    }
    backend::initialize_current_cpu(g_registered_sources);
    unlock_registry();
}

void dispatch()
{
    const ClaimToken token = backend::claim();
    uint64 pending = token.pending_sources;

    if (pending == 0 && token.controller_token != 0)
    {
        if (!__atomic_exchange_n(&g_invalid_claim_reported, true, __ATOMIC_ACQ_REL))
        {
            platformDiagnosticWarn("[irq] controller returned unsupported token=0x%lx\n",
                                   token.controller_token);
        }
    }

    while (pending != 0)
    {
        const Source source = first_set_source(pending);
        bool has_handler = false;
        for (uint32 index = 0; index < k_handlers_per_source; ++index)
        {
            HandlerSlot &slot = g_handlers[source][index];
            Handler handler = __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
            if (handler == nullptr)
            {
                continue;
            }
            has_handler = true;
            handler(slot.context);
        }

        if (!has_handler)
        {
            const uint64 bit = 1ULL << source;
            const uint64 reported = __atomic_fetch_or(
                &g_unhandled_reported, bit, __ATOMIC_ACQ_REL);
            if ((reported & bit) == 0)
            {
                platformDiagnosticWarn("[irq] unhandled source=%u pending=0x%lx\n",
                                       source, token.pending_sources);
            }
        }
        pending &= pending - 1;
    }

    // 即使 token 不在公共 0..63 范围内，也必须归还原始控制器令牌，
    // 否则一个异常 source 会把整个外部中断入口永久堵住。
    if (token.controller_token != 0)
    {
        backend::complete(token);
    }
}

uint64 registered_sources()
{
    lock_registry();
    const uint64 sources = g_registered_sources;
    unlock_registry();
    return sources;
}
} // namespace hal::irq
