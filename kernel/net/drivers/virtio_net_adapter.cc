//
// 板级网卡到 ONPS 的适配层。文件名保留用于减少本次跨分支改名噪声，
// 实现不再依赖具体 VirtIO 设备。
//

#include "virtio_net_adapter.hh"
#include "platform_net_device.hh"
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
    extern void platform_recv_thread(void *param);
    
    // Static variables for adapter state
    // adapter_initialized 表示 ONPS 和板级网卡之间的桥已经搭好。
    static bool adapter_initialized = false;
    // ONPS 里的网络接口对象，具体设备名由板级驱动提供。
    static PST_NETIF onps_netif = nullptr;
    // 接收线程运行标志，cleanup 时通过它让线程退出。
    static bool recv_thread_running = false;
    
    // 收发路径可能并发执行，不能复用同一块临时帧缓存。
    static uint8 tx_packet_buffer[platform_device::k_max_ethernet_frame];
    static uint8 rx_packet_buffer[platform_device::k_receive_buffer_size];

    // Initialize the adapter
    bool adapter_init()
    {
        // 适配层也只初始化一次，重复调用直接成功。
        if (adapter_initialized) {
            printf("[net_adapter] Already initialized\n");
            return true;
        }
        
        printf("[net_adapter] Initializing %s to ONPS adapter\n", platform_device::name());
        
        // 底层设备由 init_network_stack() 在启动 ONPS 工作线程前完成初始化；
        // 本层只负责把已经可用的设备注册给协议栈。
        // ONPS 注册以太网接口时需要 MAC 地址，由当前板级网卡提供。
        uint8 mac_addr[platform_device::k_mac_address_length];
        platform_device::get_mac(mac_addr);
        
        printf("[net_adapter] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac_addr[0], mac_addr[1], mac_addr[2], 
               mac_addr[3], mac_addr[4], mac_addr[5]);
        
        // Setup IPv4 configuration for the interface
        // ONPS 需要知道接口 IP、子网掩码、网关、DNS 等网络层配置。
        ST_IPV4 ipv4_config;
        memset(&ipv4_config, 0, sizeof(ipv4_config));
        
        // ONPS 当前没有 DHCPv4 客户端，地址配置由板级门面提供；2K1000
        // 可直接通过 make 变量覆盖，不把实验室网段写死在协议适配器中。
        ipv4_config.unAddr = inet_addr(platform_device::ipv4_address());
        ipv4_config.unSubnetMask = inet_addr(platform_device::ipv4_netmask());
        ipv4_config.unGateway = inet_addr(platform_device::ipv4_gateway());
        ipv4_config.unPrimaryDNS = inet_addr(platform_device::ipv4_dns());
        ipv4_config.unBroadcast = inet_addr(platform_device::ipv4_broadcast());

        // Register ethernet interface with onps
        // 这一步是整条真实网络链路的关键：
        // ONPS 之后要发以太网帧时，会调用 platform_emac_send；
        // ONPS 需要收包时，会通过 start_recv_thread_wrapper 启动接收线程。
        EN_ONPSERR error;
        onps_netif = ethernet_add(platform_device::name(), // Interface name
                                  mac_addr,               // MAC address
                                  &ipv4_config,          // IPv4 config
                                  platform_emac_send,    // Send function
                                  start_recv_thread_wrapper, // Receive thread starter
                                  &onps_netif,           // Output netif pointer
                                  &error);               // Error output
        
        if (!onps_netif) {
            printf("[net_adapter] Failed to add ethernet interface to onps: %d\n", error);
            return false;
        }
        
        printf("[net_adapter] Successfully registered interface with onps\n");
        
        adapter_initialized = true;
        return true;
    }
    
    // Implementation of PFUN_EMAC_SEND for onps
    // 该回调接收 ONPS 的链式缓冲并交给当前板级网卡发送。
    int platform_emac_send(short buf_list_head, unsigned char *error)
    {
        // ONPS 发包时给的是自己的 buf_list 链表，不是连续内存。
        if (!adapter_initialized) {
            if (error) *error = 1; // Generic error
            return -1;
        }
        
        // Get total length of the packet
        // 以太网帧最大长度受 ETH_FRAME_LEN 限制，超长直接拒绝。
        UINT total_len = buf_list_get_len(buf_list_head);
        if (total_len <= 0 || total_len > platform_device::k_max_ethernet_frame) {
            printf("[net_adapter] Invalid packet length: %d\n", total_len);
            if (error) *error = 1;
            return -1;
        }
        
        // Merge buffer list into contiguous packet
        // 板级驱动发送接口接收连续帧缓冲，所以先合并 ONPS 链式 buffer。
        buf_list_merge_packet(buf_list_head, tx_packet_buffer);
        
        // 这里进入当前板级驱动的发送路径。
        int result = platform_device::send(tx_packet_buffer, total_len);
        
        if (result != 0) {
            printf("[net_adapter] platform send failed: %d\n", result);
            if (error) *error = 1;
            return -1;
        }
        
        if (error) *error = 0; // Success
        return total_len;
    }
    
    // Background thread for receiving packets
    void platform_recv_thread(void *param)
    {
        // param 是 ethernet_add 传下来的 ONPS 网络接口指针。
        PST_NETIF netif = static_cast<PST_NETIF>(param);
        if (netif == nullptr) {
            printf("[net_adapter] Receive thread got null netif\n");
            return;
        }

        recv_thread_running = true;
        printf("[net_adapter] Receive thread started\n");
        while (recv_thread_running) {
            bool received_any = false;
            for (;;) {
                // 每轮尽量把设备中已经到达的包全部取完。
                uint32 packet_len = sizeof(rx_packet_buffer);
                int result = platform_device::receive(rx_packet_buffer, &packet_len);
                if (result != 0 || packet_len == 0) {
                    break;
                }

                received_any = true;
                // 把完整以太网帧交给 ONPS，之后由 ONPS 解析 ARP/IP/TCP/UDP/ICMP。
                ethernet_ii_recv(netif, rx_packet_buffer, static_cast<INT>(packet_len));
            }

            // 顺手回收已经发送完成的 TX descriptor。
            platform_device::poll();
            if (!received_any) {
                // 没包时短睡，避免接收线程空转占满 CPU。
                os_sleep_ms(1);
            }
        }
        printf("[net_adapter] Receive thread stopped\n");
    }
    
    // Wrapper function for ethernet_add interface
    void start_recv_thread_wrapper(void *param) 
    {
        // ONPS 在 ethernet_add 期间调用这个回调，让驱动启动接收路径。
        printf("[net_adapter] Receive thread wrapper called\n");
        PST_NETIF *netif_slot = static_cast<PST_NETIF *>(param);
        PST_NETIF netif = netif_slot != nullptr ? *netif_slot : nullptr;
        if (netif == nullptr) {
            printf("[net_adapter] Receive thread wrapper got null netif\n");
            return;
        }
        onps_netif = netif;
        if (recv_thread_running) {
            // 防止重复创建接收线程。
            return;
        }

        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr) {
            printf("[net_adapter] No current process for receive thread\n");
            return;
        }

        // 复用内核的 fork/clone 机制创建一个线程：
        // 共享地址空间、fd 表、信号处理器，并标记为同一线程组。
        uint64 flags = syscall::CLONE_VM | syscall::CLONE_FILES |
                       syscall::CLONE_SIGHAND | syscall::CLONE_THREAD;
        proc::Pcb *thread_pcb = proc::k_pm.fork(current_proc, flags, 0, 0, false);
        if (thread_pcb == nullptr) {
            printf("[net_adapter] Failed to fork receive thread\n");
            return;
        }

        // kernel_thread_wrapper 会从 context.s0 取函数指针，从 context.s1 取参数。
        thread_pcb->_context.ra = reinterpret_cast<uint64>(kernel_thread_wrapper);
        thread_pcb->_context.s0 = reinterpret_cast<uint64>(platform_recv_thread);
        thread_pcb->_context.s1 = reinterpret_cast<uint64>(netif);
        strncpy(thread_pcb->_name, "platform-net-rx", sizeof(thread_pcb->_name) - 1);
        thread_pcb->_name[sizeof(thread_pcb->_name) - 1] = '\0';

        // fork 返回的 thread_pcb 此时还持有锁；释放后调度器才能运行这个接收线程。
        recv_thread_running = true;
        thread_pcb->_lock.release();
    }
    
    // Get MAC address for onps registration
    void get_mac_address(unsigned char mac[6])
    {
        platform_device::get_mac(mac);
    }
}
