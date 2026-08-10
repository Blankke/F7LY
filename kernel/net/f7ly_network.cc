//
// F7LY Network Stack Integration
// Selects the target-specific Ethernet adapter and integrates it with ONPS.
//

#include "f7ly_network.hh"
#ifdef VISIONFIVE2
#include "drivers/vf2/vf2_gmac_adapter.hh"
#else
#include "drivers/virtio_net_adapter.hh"
#include "drivers/virtio_net.hh"
#endif
#include "libs/printer.hh"
#include "onps.hh"

namespace net
{
    // 整个网络栈只需要初始化一次。loopback socket 不依赖这个标志；
    // 只有外部 IPv4 TCP/UDP/ICMP 需要 ONPS + VirtIO 时才要求它为 true。
    static bool network_initialized = false;
    
    // Initialize the complete network stack
    bool init_network_stack()
    {
        // 多个进程第一次创建 AF_INET socket 时都可能走到这里；已经完成就直接返回。
        if (network_initialized) {
            printf("[f7ly_network] Network stack already initialized\n");
            return true;
        }
        
        printf("[f7ly_network] Initializing F7LY network stack with %s\n",
#ifdef VISIONFIVE2
               "VF2 GMAC"
#else
               "VirtIO Net"
#endif
        );
        
        // Step 1: Initialize ONPS network stack core
        // ONPS 是内核内的 TCP/IP 协议栈，负责 ARP/IP/ICMP/UDP/TCP 等协议逻辑。
        EN_ONPSERR onps_error;
        if (!open_npstack_load(&onps_error)) {
            printf("[f7ly_network] Failed to initialize ONPS stack: %d\n", onps_error);
            return false;
        }
        
        printf("[f7ly_network] ONPS core initialized successfully\n");
        
        // Step 2: Initialize the target-specific adapter and register it with ONPS.
#ifdef VISIONFIVE2
        if (!net::vf2_adapter_init()) {
            printf("[f7ly_network] Failed to initialize VF2 GMAC adapter\n");
#else
        if (!net::adapter_init()) {
            printf("[f7ly_network] Failed to initialize VirtIO Net adapter\n");
#endif
            // ONPS 已经初始化成功，如果网卡适配层失败，需要回滚协议栈核心。
            open_npstack_unload();
            return false;
        }
        
        printf("[f7ly_network] Target network adapter initialized successfully\n");
        
        network_initialized = true;
        
        // Print initial status
#ifdef RISCV
        // print_network_status();
#endif
        
        return true;
    }

    bool is_network_stack_ready()
    {
        // socket_file 用这个判断是否可以把非 loopback IPv4 流量交给 ONPS。
        return network_initialized;
    }
    
    // Cleanup the network stack
    void cleanup_network_stack()
    {
        if (!network_initialized) {
            return;
        }
        
        printf("[f7ly_network] Cleaning up network stack\n");
        
        // Cleanup in reverse order
        // 先停网卡适配层，再卸载协议栈，顺序和初始化相反。
#ifdef VISIONFIVE2
        net::vf2_adapter_cleanup();
#else
        net::adapter_cleanup();
#endif
        open_npstack_unload();
        
        network_initialized = false;
    }
    
    // Print network interface status
    void print_network_status()
    {
        // 调试辅助函数，只打印当前网卡与 VirtIO 状态，不改变网络栈行为。
        if (!network_initialized) {
            printf("[f7ly_network] Network stack not initialized\n");
            return;
        }
        
        printf("[f7ly_network] ========== Network Status ==========\n");
        
        // Get and print MAC address
        uint8 mac[6];
#ifdef VISIONFIVE2
        vf2_get_mac_address(mac);
#else
        get_mac_address(mac);
#endif
        printf("[f7ly_network] MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
#ifdef VISIONFIVE2
        vf2_adapter_debug_status();
#else
        virtio_net_debug_status();
#endif
        
        printf("[f7ly_network] ===================================\n");
    }
}
