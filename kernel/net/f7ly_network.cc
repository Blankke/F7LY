// F7LY 网络栈集成：通过板级网卡门面连接 ONPS。

#include "f7ly_network.hh"
#include "drivers/onps_adapter.hh"
#include "drivers/platform_net_device.hh"
#include "libs/printer.hh"
#include "onps.hh"
#include "scheduler.hh"
#include <EASTL/atomic.h>

namespace net
{
    // 三阶段状态使失败可以在原阶段重试。尤其要保证网卡探测失败时尚未启动
    // ONPS 工作线程，不能再走“先启动线程、失败后立即释放其同步对象”的路径。
    static bool device_initialized = false;
    static bool core_initialized = false;
    constexpr uint32 k_network_idle = 0;
    constexpr uint32 k_network_initializing = 1;
    constexpr uint32 k_network_ready = 2;
    static eastl::atomic<uint32> network_state{k_network_idle};
    
    // Initialize the complete network stack
    bool init_network_stack()
    {
        // 多个进程可能同时创建第一个 AF_INET socket。只有 CAS 成功者执行
        // 硬件/协议栈初始化，其余进程让出 CPU 并观察最终结果。
        for (;;)
        {
            const uint32 state = network_state.load(eastl::memory_order_acquire);
            if (state == k_network_ready)
            {
                return true;
            }
            if (state == k_network_idle)
            {
                uint32 expected = k_network_idle;
                if (network_state.compare_exchange_strong(
                        expected, k_network_initializing, eastl::memory_order_acq_rel))
                {
                    break;
                }
                continue;
            }
            proc::k_scheduler.yield();
        }

        auto finish = [](bool success) {
            network_state.store(success ? k_network_ready : k_network_idle,
                                eastl::memory_order_release);
            return success;
        };
        
        printf("[f7ly_network] Initializing F7LY network stack\n");

        // 先确认硬件可用。这样 VirtIO/GMAC 探测失败不会留下 ONPS 后台线程。
        if (!device_initialized) {
            if (!platform_device::initialize()) {
                printf("[f7ly_network] Platform network driver init failed\n");
                return finish(false);
            }
            device_initialized = true;
        }
        
        // Step 1: Initialize ONPS network stack core
        // ONPS 是内核内的 TCP/IP 协议栈，负责 ARP/IP/ICMP/UDP/TCP 等协议逻辑。
        if (!core_initialized) {
            EN_ONPSERR onps_error;
            if (!open_npstack_load(&onps_error)) {
                printf("[f7ly_network] Failed to initialize ONPS stack: %d\n", onps_error);
                return finish(false);
            }
            core_initialized = true;
            printf("[f7ly_network] ONPS core initialized successfully\n");
        }
        
        // Step 2: 初始化板级网卡适配器并注册到 ONPS。
        if (!net::adapter_init()) {
            printf("[f7ly_network] Failed to initialize platform Net adapter\n");
            // ONPS 的历史 unload 路径没有先等待全部工作线程退出。这里保留已经
            // 成功初始化的 core，下次只重试接口注册，避免释放活线程仍在使用的锁。
            return finish(false);
        }
        
        printf("[f7ly_network] Platform Net adapter initialized successfully\n");
        
        finish(true);

        // Print initial status
#ifdef RISCV
        print_network_status();
#endif
        
        return true;
    }

    bool is_network_stack_ready()
    {
        // socket_file 用这个判断是否可以把非 loopback IPv4 流量交给 ONPS。
        return network_state.load(eastl::memory_order_acquire) == k_network_ready;
    }
    
    // Print network interface status
    void print_network_status()
    {
        // 调试辅助函数只打印当前板级网卡状态，不改变网络栈行为。
        if (!is_network_stack_ready()) {
            printf("[f7ly_network] Network stack not initialized\n");
            return;
        }
        
        printf("[f7ly_network] ========== Network Status ==========\n");
        
        // Get and print MAC address
        uint8 mac[6];
        get_mac_address(mac);
        printf("[f7ly_network] MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        // 输出当前板级网卡状态。
        platform_device::debug_status();
        
        printf("[f7ly_network] ===================================\n");
    }
}
