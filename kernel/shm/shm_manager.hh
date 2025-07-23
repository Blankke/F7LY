#include "ipc_param.hh"
#include <EASTL/unordered_map.h>
#include <EASTL/vector.h>
#include "proc.hh"
namespace shm
{
    struct shm_segment
    {
        int shmid;         // 共享内存段ID
        key_t key;         // 共享内存段键值
        size_t size;       // 用户请求的原始大小（POSIX标准要求）
        size_t real_size;  // 实际分配的页对齐大小（用于内存管理）
		union
		{
			u16 shmflg;
			struct
			{
				u16 o_exec : 1;  // 其他执行权限
				u16 o_write : 1; // 其他写权限
				u16 o_read : 1;  // 其他读权限
				u16 g_exec : 1;  // 组执行权限
				u16 g_write : 1; // 组写权限
				u16 g_read : 1;  // 组读权限
				u16 u_exec : 1;  // 用户执行权限
				u16 u_write : 1; // 用户写权限
				u16 u_read : 1;  // 用户读权限
				u16 _rsv : 7;
			}__attribute__((__packed__));
		}__attribute__((__packed__));
        eastl::vector<void*> attached_addrs;  // 所有附加的虚拟地址列表
        uint64 phy_addrs;  // 物理地址
        
        // 时间信息（POSIX标准：自Unix纪元以来的秒数）
        time_t atime;      // 最后访问时间 (shmat) - 使用timer_manager获取REALTIME
        time_t dtime;      // 最后分离时间 (shmdt) - 使用timer_manager获取REALTIME
        time_t ctime;      // 创建/最后修改时间 (shmget/shmctl) - 使用timer_manager获取REALTIME
        
        // 进程信息
        pid_t creator_pid; // 创建者进程ID (shm_cpid)
        pid_t last_pid;    // 最后操作的进程ID (shm_lpid)
        
        // 权限和状态信息
        uid_t owner_uid;   // 所有者用户ID
        gid_t owner_gid;   // 所有者组ID
        uid_t creator_uid; // 创建者用户ID
        gid_t creator_gid; // 创建者组ID
        mode_t mode;       // 访问权限模式
        
        // 附加计数
        int nattch;        // 当前附加的进程数量
        
        // 序列号
        unsigned short seq; // 序列号
    };


    // 空闲内存块结构
    struct free_block
    {
        uint64 addr;   // 空闲块起始地址
        size_t size;   // 空闲块大小
        
        // 用于排序，按地址排序便于合并相邻块
        bool operator<(const free_block& other) const {
            return addr < other.addr;
        }
    };

    class ShmManager
    {
    private:
        // 管理共享内存段的容器
        // 使用unordered_map来存储共享内存段信息，key为shmid
        eastl::unordered_map<int, shm_segment>* segments;
        int next_shmid; // 下一个可用的shmid,分配后更新这个值
        uint64 shm_base;
        uint64 shm_size;
        
        // 空闲内存块管理 - 使用vector来存储空闲块，保持按地址排序
        eastl::vector<free_block> free_blocks;
        
        // 私有内存管理方法
        uint64 allocate_memory(size_t size);  // 从空闲块中分配内存
        void deallocate_memory(uint64 addr, size_t size);  // 回收内存到空闲块
        void merge_adjacent_blocks();  // 合并相邻的空闲块
        
        // 私有辅助方法
        int create_new_segment(key_t key, size_t size, int shmflg);  // 创建新的共享内存段
        eastl::unordered_map<int, shm_segment>::iterator find_segment_by_key(key_t key);  // 根据key查找段
        bool check_segment_permission(const shm_segment& seg, uid_t uid, gid_t gid, mode_t requested_mode);  // 检查权限
        bool check_segment_read_permission(const shm_segment& seg, uid_t uid, gid_t gid);  // 检查读权限
        bool check_segment_attach_permission(const shm_segment& seg, uid_t uid, gid_t gid, bool need_write);  // 检查附加权限
        uint64 find_available_address(proc::Pcb* proc, size_t size);  // 查找可用地址
        bool is_valid_attach_address(uint64 addr, size_t size, bool rounded);  // 验证地址合法性
        bool has_address_conflict(proc::Pcb* proc, uint64 addr, size_t size);  // 检查地址冲突
        
    public:
        void init(uint64 base, uint64 size) ;

        // 创建共享内存段
        int create_seg(key_t key, size_t size, int shmflg);

        // 删除共享内存段
        int delete_seg(int shmid);

        // 映射共享内存段到进程地址空间 (标准shmat接口)
        // 用法示例：
        //   char* shmaddr = (char*)attach_seg(shmid, nullptr, 0);         // 内核自动选择地址
        //   char* shmaddr = (char*)attach_seg(shmid, (void*)0x1000, 0);  // 指定地址
        //   char* shmaddr = (char*)attach_seg(shmid, (void*)0x1234, SHM_RND); // 地址向下对齐
        //   char* shmaddr = (char*)attach_seg(shmid, nullptr, SHM_RDONLY);    // 只读映射
        void *attach_seg(int shmid, void *shmaddr = nullptr, int shmflg = 0);

        // 解除映射共享内存段
        int detach_seg(void *addr);

        // 控制共享内存段 (标准shmctl接口)
        // cmd可以是: IPC_STAT, IPC_SET, IPC_RMID
        // 用法示例:
        //   struct shmid_ds shm_info;
        //   shmctl(shmid, IPC_STAT, &shm_info);  // 获取信息
        //   shmctl(shmid, IPC_SET, &shm_info);   // 设置信息  
        //   shmctl(shmid, IPC_RMID, nullptr);    // 删除段
        int shmctl(int shmid, int cmd, struct shmid_ds *buf,uint64 buf_addr = 0);

        // 获取共享内存段信息
        shm_segment get_seg_info(int shmid);

        int set_seg_info(int shmid, const shm_segment &seg_info);

        key_t ftok(const char *__pathname, int __proj_id);
        
        // 调试和监控方法
        void print_memory_status() const;  // 打印内存使用状况
        size_t get_total_free_memory() const;  // 获取总空闲内存
        size_t get_largest_free_block() const;  // 获取最大空闲块大小
    };

    extern ShmManager k_smm; // 全局共享内存管理器实例
}