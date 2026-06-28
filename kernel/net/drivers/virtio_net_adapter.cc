//
// VirtIO Net to ONPS Adapter Implementation
// This adapter bridges virtio_net driver with onps network stack
//

#include "virtio_net_adapter.hh"
#include "virtio_net.hh"
#include "platform.hh"
#include "mem/memlayout.hh"
#include "libs/string.hh"
#include "libs/klib.hh"
#include "libs/printer.hh"
#include "proc_manager.hh"
#include "sys/syscall_defs.hh"

// ONPS network stack includes
#include "onps.hh"
#include "netif/netif.hh"
#include "ethernet/ethernet.hh"
#include "mmu/buf_list.hh"
#include "port/os_adapter.hh"

extern "C" void kernel_thread_wrapper();

namespace net
{
    // Forward declarations
    // ethernet_add 需要一个“启动接收线程”的回调，这里先前向声明。
    static void start_recv_thread_wrapper(void *param);
    // 真正循环收包的内核线程函数。
    extern void virtio_recv_thread(void *param);
    
    // Static variables for adapter state
    // adapter_initialized 表示 ONPS 和 VirtIO 之间的桥已经搭好。
    static bool adapter_initialized = false;
    // ONPS 里的网络接口对象，代表 virtio0。
    static PST_NETIF onps_netif = nullptr;
    // 接收线程运行标志，cleanup 时通过它让线程退出。
    static bool recv_thread_running = false;
    
    // 收发路径可能并发执行，不能复用同一块临时帧缓存。
    static uint8 tx_packet_buffer[ETH_FRAME_LEN];
    static uint8 rx_packet_buffer[ETH_FRAME_LEN];

    // Initialize the adapter
    bool adapter_init()
    {
        // 适配层也只初始化一次，重复调用直接成功。
        if (adapter_initialized) {
            printf("[virtio_net_adapter] Already initialized\n");
            return true;
        }
        
        printf("[virtio_net_adapter] Initializing VirtIO Net to ONPS adapter\n");
        
        // Initialize virtio net driver first
        // 先初始化底层 VirtIO 网卡，后面才能把它注册给 ONPS。
        if (!net::virtio_net_init()) {
            printf("[virtio_net_adapter] VirtIO Net driver init failed\n");
            return false;
        }
        
        // Get MAC address from virtio net
        // ONPS 注册以太网接口时需要 MAC 地址，直接从 VirtIO 配置空间读取。
        uint8 mac_addr[ETH_ALEN];
        net::virtio_net_get_mac(mac_addr);
        
        printf("[virtio_net_adapter] VirtIO MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac_addr[0], mac_addr[1], mac_addr[2], 
               mac_addr[3], mac_addr[4], mac_addr[5]);
        
        // Setup IPv4 configuration for the interface
        // ONPS 需要知道接口 IP、子网掩码、网关、DNS 等网络层配置。
        ST_IPV4 ipv4_config;
        memset(&ipv4_config, 0, sizeof(ipv4_config));
        
        // QEMU user-mode 网络默认拓扑：guest=10.0.2.15，gateway/DNS=10.0.2.2/10.0.2.3。
        ipv4_config.unAddr = inet_addr("10.0.2.15");
        ipv4_config.unSubnetMask = inet_addr("255.255.255.0");
        ipv4_config.unGateway = inet_addr("10.0.2.2");
        ipv4_config.unPrimaryDNS = inet_addr("10.0.2.3");
        ipv4_config.unBroadcast = inet_addr("10.0.2.255");

        // Register ethernet interface with onps
        // 这一步是整条真实网络链路的关键：
        // ONPS 之后要发以太网帧时，会调用 virtio_emac_send；
        // ONPS 需要收包时，会通过 start_recv_thread_wrapper 启动接收线程。
        EN_ONPSERR error;
        onps_netif = ethernet_add("virtio0",              // Interface name
                                  mac_addr,               // MAC address
                                  &ipv4_config,          // IPv4 config
                                  virtio_emac_send,      // Send function
                                  start_recv_thread_wrapper, // Receive thread starter
                                  &onps_netif,           // Output netif pointer
                                  &error);               // Error output
        
        if (!onps_netif) {
            printf("[virtio_net_adapter] Failed to add ethernet interface to onps: %d\n", error);
            return false;
        }
        
        printf("[virtio_net_adapter] Successfully registered interface with onps\n");
        
        adapter_initialized = true;
        return true;
    }
    
    // Cleanup function
    void adapter_cleanup()
    {
        // 清理适配层主要是停接收线程，并忘掉 ONPS 接口指针。
        if (!adapter_initialized) {
            return;
        }
        
        printf("[virtio_net_adapter] Cleaning up adapter\n");
        
        // Stop receive thread
        stop_recv_thread();
        
        // Remove interface from onps (if function exists)
        if (onps_netif) {
            // netif_del_ext(onps_netif); // Uncomment if this function exists
            onps_netif = nullptr;
        }
        
        adapter_initialized = false;
    }
    
    // Implementation of PFUN_EMAC_SEND for onps
    // This function receives data from onps and sends it via virtio net
    int virtio_emac_send(short buf_list_head, unsigned char *error)
    {
        // ONPS 发包时给的是自己的 buf_list 链表，不是连续内存。
        if (!adapter_initialized) {
            if (error) *error = 1; // Generic error
            return -1;
        }
        
        // Get total length of the packet
        // 以太网帧最大长度受 ETH_FRAME_LEN 限制，超长直接拒绝。
        UINT total_len = buf_list_get_len(buf_list_head);
        if (total_len <= 0 || total_len > ETH_FRAME_LEN) {
            printf("[virtio_net_adapter] Invalid packet length: %d\n", total_len);
            if (error) *error = 1;
            return -1;
        }
        
        // Merge buffer list into contiguous packet
        // VirtIO 驱动当前发送接口需要连续帧缓冲，所以先把 ONPS 链式 buffer 合并。
        buf_list_merge_packet(buf_list_head, tx_packet_buffer);
        
        // Send via virtio net
        // 这里进入底层 virtqueue 发送路径。
        int result = net::virtio_net_send(tx_packet_buffer, total_len);
        
        if (result != 0) {
            printf("[virtio_net_adapter] virtio_net_send failed: %d\n", result);
            if (error) *error = 1;
            return -1;
        }
        
        if (error) *error = 0; // Success
        return total_len;
    }
    
    // Background thread for receiving packets
    void virtio_recv_thread(void *param)
    {
        // param 是 ethernet_add 传下来的 ONPS 网络接口指针。
        PST_NETIF netif = static_cast<PST_NETIF>(param);
        if (netif == nullptr) {
            printf("[virtio_net_adapter] Receive thread got null netif\n");
            return;
        }

        recv_thread_running = true;
        printf("[virtio_net_adapter] Receive thread started\n");
        while (recv_thread_running) {
            bool received_any = false;
            for (;;) {
                // 每轮尽量把 VirtIO used ring 里已经到达的包全部取完。
                uint32 packet_len = sizeof(rx_packet_buffer);
                int result = net::virtio_net_recv(rx_packet_buffer, &packet_len);
                if (result != 0 || packet_len == 0) {
                    break;
                }

                received_any = true;
                // 把完整以太网帧交给 ONPS，之后由 ONPS 解析 ARP/IP/TCP/UDP/ICMP。
                ethernet_ii_recv(netif, rx_packet_buffer, static_cast<INT>(packet_len));
            }

            // 顺手回收已经发送完成的 TX descriptor。
            net::virtio_net_poll();
            if (!received_any) {
                // 没包时短睡，避免接收线程空转占满 CPU。
                os_sleep_ms(1);
            }
        }
        printf("[virtio_net_adapter] Receive thread stopped\n");
    }
    
    // Start the receive thread  
    void start_recv_thread()
    {
        // 这个函数只是设置状态；真正创建内核线程的是 start_recv_thread_wrapper。
        if (recv_thread_running) {
            return; // Already running
        }
        
        printf("[virtio_net_adapter] Starting receive thread\n");
        recv_thread_running = true;
        
        // For now, we'll just mark as running
        // The actual thread will be created by the wrapper function
    }
    
    // Wrapper function for ethernet_add interface
    void start_recv_thread_wrapper(void *param) 
    {
        // ONPS 在 ethernet_add 期间调用这个回调，让驱动启动接收路径。
        printf("[virtio_net_adapter] Receive thread wrapper called\n");
        PST_NETIF *netif_slot = static_cast<PST_NETIF *>(param);
        PST_NETIF netif = netif_slot != nullptr ? *netif_slot : nullptr;
        if (netif == nullptr) {
            printf("[virtio_net_adapter] Receive thread wrapper got null netif\n");
            return;
        }
        onps_netif = netif;
        if (recv_thread_running) {
            // 防止重复创建接收线程。
            return;
        }

        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr) {
            printf("[virtio_net_adapter] No current process for receive thread\n");
            return;
        }

        // 复用内核的 fork/clone 机制创建一个线程：
        // 共享地址空间、fd 表、信号处理器，并标记为同一线程组。
        uint64 flags = syscall::CLONE_VM | syscall::CLONE_FILES |
                       syscall::CLONE_SIGHAND | syscall::CLONE_THREAD;
        proc::Pcb *thread_pcb = proc::k_pm.fork(current_proc, flags, 0, 0, false);
        if (thread_pcb == nullptr) {
            printf("[virtio_net_adapter] Failed to fork receive thread\n");
            return;
        }

        // kernel_thread_wrapper 会从 context.s0 取函数指针，从 context.s1 取参数。
        thread_pcb->_context.ra = reinterpret_cast<uint64>(kernel_thread_wrapper);
        thread_pcb->_context.s0 = reinterpret_cast<uint64>(virtio_recv_thread);
        thread_pcb->_context.s1 = reinterpret_cast<uint64>(netif);
        strncpy(thread_pcb->_name, "virtio-net-rx", sizeof(thread_pcb->_name) - 1);
        thread_pcb->_name[sizeof(thread_pcb->_name) - 1] = '\0';

        // fork 返回的 thread_pcb 此时还持有锁；释放后调度器才能运行这个接收线程。
        recv_thread_running = true;
        thread_pcb->_lock.release();
    }
    
    // Stop the receive thread
    void stop_recv_thread()
    {
        // cleanup 时把运行标志清掉，接收线程下一轮循环会退出。
        if (!recv_thread_running) {
            return;
        }
        
        printf("[virtio_net_adapter] Stopping receive thread\n");
        recv_thread_running = false;
        
        // Give the thread time to exit
        // 简单等待线程看到标志并退出。
        os_sleep_ms(100);
        
    }
    
    // Get MAC address for onps registration
    void get_mac_address(unsigned char mac[6])
    {
        // 对外暴露 MAC 地址查询，内部仍从 VirtIO 驱动读取缓存值。
        net::virtio_net_get_mac(mac);
    }
}
