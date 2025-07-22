#include "ipc_param.hh"
#include <EASTL/unordered_map.h>

namespace shm
{
    struct shm_segment
    {
        int shmid;         // 共享内存段ID
        size_t size;       // 共享内存段大小
        int perm;          // 权限
        void *addr;        // 映射地址
        time_t atime;      // 最后访问时间
        time_t dtime;      // 最后分离时间
        time_t ctime;      // 创建时间
        pid_t creator_pid; // 创建者进程ID
    };
    {
        /* data */
    };

    class ShmManager
    {
    private:
        // 管理共享内存段的容器
        // 使用unordered_map来存储共享内存段信息，key为shmid
        eastl::unordered_map<int, shm_segment> segments;
        int next_shmid; // 下一个可用的shmid,分配后更新这个值
        uint64 shm_base;
    public:
        void init();
        // 创建共享内存段
        int create_seg(key_t key, size_t size, int perm);

        // 删除共享内存段
        int delete_seg(int shmid);

        // 映射共享内存段到进程地址空间
        void *attach_seg(int shmid, void *addr = nullptr);

        // 解除映射共享内存段
        int detach_seg(void *addr);

        // 获取共享内存段信息
        shm_segment get_seg_info(int shmid);

        int set_seg_info(int shmid, const shm_segment &seg_info);

        key_t ftok(const char *__pathname, int __proj_id);
    };
}