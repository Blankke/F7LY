#include "libs/param.h"
#include "types.hh"
// #include "riscv.h"
#include "devs/spinlock.hh"
#include "proc/sleeplock.hh"
#include "fs/buf.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "mem/virtual_memory_manager.hh"
#include "fs/stat.hh"
#include <fcntl.h>
#include "fs/fat32/fat32.hh"

#define NELEM(x) (sizeof(x)/sizeof((x)[0]))
#define myproc() proc::k_pm.get_cur_pcb()

static int either_copyin(void *dst, int user, uint64 src, uint64 len) {
    if (user) {
        auto p = myproc();
        return mem::k_vmm.copy_in(*p->get_pagetable(), dst, src, len);
    } else {
        memmove(dst, (void *)src, len);
        return 0;
    }
}

static int either_copyout(int user, uint64 dst, const void *src, uint64 len) {
    if (user) {
        auto p = myproc();
        return mem::k_vmm.copy_out(*p->get_pagetable(), dst, src, len);
    } else {
        memmove((void *)dst, src, len);
        return 0;
    }
}

#include "libs/string.hh"
#include "libs/klib.hh"
#include "libs/printer.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/inode.hh"

/* 以下以"_"开头的字段是FAT32规范中定义但本实现暂不使用的字段 */

// FAT32短文件名目录项结构体（32字节）
// __attribute__((packed, aligned(4)))：取消内存对齐，保证和磁盘格式一致
typedef struct short_name_entry {
    char        name[CHAR_SHORT_NAME];      // 8+3短文件名（8位主名+3位扩展名）
    uint8       attr;                       // 文件属性（目录/只读/隐藏等）
    uint8       _nt_res;                    // 保留字段（未使用）
    uint8       _crt_time_tenth;            // 创建时间的1/100秒（未使用）
    uint16      _crt_time;                  // 创建时间（未使用）
    uint16      _crt_date;                  // 创建日期（未使用）
    uint16      _lst_acce_date;             // 最后访问日期（未使用）
    uint16      fst_clus_hi;                // 起始簇号高16位
    uint16      _lst_wrt_time;              // 最后写入时间（未使用）
    uint16      _lst_wrt_date;              // 最后写入日期（未使用）
    uint16      fst_clus_lo;                // 起始簇号低16位
    uint32      file_size;                  // 文件大小（字节）
} __attribute__((packed, aligned(4))) short_name_entry_t;

// FAT32长文件名目录项结构体（32字节）
typedef struct long_name_entry {
    uint8       order;                      // 长文件名项的顺序（最高位标记最后一项）
    wchar       name1[5];                   // 长文件名部分1（5个宽字符）
    uint8       attr;                       // 属性（固定为ATTR_LONG_NAME）
    uint8       _type;                      // 类型（未使用）
    uint8       checksum;                   // 短文件名校验和（用于关联短/长文件名）
    wchar       name2[6];                   // 长文件名部分2（6个宽字符）
    uint16      _fst_clus_lo;               // 起始簇号低16位（未使用）
    wchar       name3[2];                   // 长文件名部分3（2个宽字符）
} __attribute__((packed, aligned(4))) long_name_entry_t;

// 目录项联合体：兼容短/长文件名目录项（共用32字节内存）
union dentry {
    short_name_entry_t  sne;    // 短文件名目录项
    long_name_entry_t   lne;    // 长文件名目录项
};

// FAT32文件系统核心参数（全局）
static struct {
    uint32  dev;                 // 设备号
    uint32  first_data_sec;      // 第一个数据扇区的编号
    uint32  data_sec_cnt;        // 数据区总扇区数
    uint32  data_clus_cnt;       // 数据区总簇数
    uint32  byts_per_clus;       // 每个簇的字节数（簇大小=扇区大小×每簇扇区数）

    // BIOS参数块（BPB）：存储FAT32分区的核心配置
    struct {
        uint16  byts_per_sec;    // 每个扇区的字节数（固定为BSIZE=512）
        uint8   sec_per_clus;    // 每个簇包含的扇区数
        uint16  rsvd_sec_cnt;    // 保留扇区数（包含引导扇区）
        uint8   fat_cnt;         // FAT表的数量（通常为2）
        uint32  hidd_sec;        // 隐藏扇区数（分区之前的扇区数）
        uint32  tot_sec;         // 分区总扇区数
        uint32  fat_sz;          // 每个FAT表的扇区数
        uint32  root_clus;       // 根目录的起始簇号
    } bpb;

} fat;

// 目录项缓存：加速目录项访问，减少磁盘IO
static struct entry_cache {
    SpinLock lock;                // 保护缓存的自旋锁
    struct fat32_entry entries[ENTRY_CACHE_NUM]; // 缓存的目录项数组
} ecache;

// 根目录项（全局）
static struct fat32_entry root;

// 挂载的文件系统设备列表（最多8个）
struct mntfs devs[8];
int idx = 0;

/**
 * 读取FAT32的BIOS参数块（BPB）并初始化文件系统
 * @return  0       成功
 *          -1      失败
 */
int fat32_init_internal(uint32 dev)
{
    fat.dev = dev;
    #ifdef DEBUG
    printf("[fat32_init] enter!\n");
    #endif
    // 读取引导扇区（第0个设备的第0个扇区）
    struct buf *b = bread(fat.dev, 0);
    // 校验FAT32标识（引导扇区偏移82处应为"FAT32"）
    if (strncmp((char const*)(b->data + 82), "FAT32", 5))
        panic("not FAT32 volume");
    
    // 从引导扇区读取BPB参数（memmove避免K210的非对齐内存访问错误）
    memmove(&fat.bpb.byts_per_sec, b->data + 11, 2);            
    fat.bpb.sec_per_clus = *(b->data + 13);
    fat.bpb.rsvd_sec_cnt = *(uint16 *)(b->data + 14);
    fat.bpb.fat_cnt = *(b->data + 16);
    fat.bpb.hidd_sec = *(uint32 *)(b->data + 28);
    fat.bpb.tot_sec = *(uint32 *)(b->data + 32);
    fat.bpb.fat_sz = *(uint32 *)(b->data + 36);
    fat.bpb.root_clus = *(uint32 *)(b->data + 44);
    
    // 计算FAT32核心参数
    fat.first_data_sec = fat.bpb.rsvd_sec_cnt + fat.bpb.fat_cnt * fat.bpb.fat_sz;
    fat.data_sec_cnt = fat.bpb.tot_sec - fat.first_data_sec;
    fat.data_clus_cnt = fat.data_sec_cnt / fat.bpb.sec_per_clus;
    fat.byts_per_clus = fat.bpb.sec_per_clus * fat.bpb.byts_per_sec;
    brelse(b); // 释放缓冲区

    #ifdef DEBUG
    printf("[FAT32 init]byts_per_sec: %d\n", fat.bpb.byts_per_sec);
    printf("[FAT32 init]root_clus: %d\n", fat.bpb.root_clus);
    printf("[FAT32 init]sec_per_clus: %d\n", fat.bpb.sec_per_clus);
    printf("[FAT32 init]fat_cnt: %d\n", fat.bpb.fat_cnt);
    printf("[FAT32 init]fat_sz: %d\n", fat.bpb.fat_sz);
    printf("[FAT32 init]first_data_sec: %d\n", fat.first_data_sec);
    #endif

    // 校验扇区大小是否与系统定义的BSIZE一致（必须相等）
    if (BSIZE != fat.bpb.byts_per_sec)
        panic("byts_per_sec != BSIZE");
    
    // 初始化目录项缓存锁
    ecache.lock.init("ecache");
    // 初始化根目录项
    memset(&root, 0, sizeof(root));
    root.lock.init("entry", "entry");          // 初始化根目录的睡眠锁
    root.attribute = (ATTR_DIRECTORY | ATTR_SYSTEM); // 标记为目录+系统属性
    root.first_clus = root.cur_clus = fat.bpb.root_clus; // 根目录起始簇号
    root.valid = 1;                              // 有效标记
    root.prev = &root;                           // 双向链表自环（缓存链表）
    root.next = &root;
    
    // 初始化目录项缓存：将所有缓存项加入根目录的双向链表
    for(struct fat32_entry *de = ecache.entries; de < ecache.entries + ENTRY_CACHE_NUM; de++) {
        de->dev = 0;
        de->valid = 0;
        de->ref = 0;
        de->dirty = 0;
        de->parent = 0;
        de->next = root.next;
        de->prev = &root;
        de->lock.init("entry", "entry");
        root.next->prev = de;
        root.next = de;
    }
    return 0;
}

/**
 * 计算指定簇对应的第一个扇区编号
 * @param   cluster   簇号（从2开始，0和1为保留值）
 */
static inline uint32 first_sec_of_clus(uint32 cluster)
{
    return ((cluster - 2) * fat.bpb.sec_per_clus) + fat.first_data_sec;
}

/**
 * 计算指定簇在FAT表中对应的扇区编号
 * @param   cluster     簇号
 * @param   fat_num     FAT表编号（从1开始，不超过fat.bpb.fat_cnt）
 */
static inline uint32 fat_sec_of_clus(uint32 cluster, uint8 fat_num)
{
    return fat.bpb.rsvd_sec_cnt + (cluster << 2) / fat.bpb.byts_per_sec + fat.bpb.fat_sz * (fat_num - 1);
}

/**
 * 计算指定簇在FAT表对应扇区中的偏移量
 * @param   cluster   簇号
 */
static inline uint32 fat_offset_of_clus(uint32 cluster)
{
    return (cluster << 2) % fat.bpb.byts_per_sec;
}

/**
 * 读取FAT表中指定簇对应的下一个簇号
 * @param   cluster     要查询的簇号
 * @return              下一个簇号（FAT32_EOC表示簇链结束）
 */
static uint32 read_fat(uint32 cluster)
{
    if (cluster >= FAT32_EOC) {          // 已到簇链末尾
        return cluster;
    }
    if (cluster > fat.data_clus_cnt + 1) { // 簇号超出范围（簇从2开始）
        return 0;
    }
    // 计算该簇在FAT表中的扇区
    uint32 fat_sec = fat_sec_of_clus(cluster, 1);
    // 注：此处应实现FAT表缓存，本版本未实现
    struct buf *b = bread(fat.dev, fat_sec);
    // 读取FAT表中该簇对应的下一个簇号
    uint32 next_clus = *(uint32 *)(b->data + fat_offset_of_clus(cluster));
    brelse(b);
    return next_clus;
}

/**
 * 向FAT表中写入指定簇的下一个簇号
 * @param   cluster     要写入的簇号
 * @param   content     下一个簇号（或FAT32_EOC表示簇链结束）
 * @return              0成功，-1失败
 */
static int write_fat(uint32 cluster, uint32 content)
{
    if (cluster > fat.data_clus_cnt + 1) { // 簇号超出范围
        return -1;
    }
    uint32 fat_sec = fat_sec_of_clus(cluster, 1);
    struct buf *b = bread(fat.dev, fat_sec);
    uint off = fat_offset_of_clus(cluster);
    // 写入下一个簇号
    *(uint32 *)(b->data + off) = content;
    bwrite(b); // 写回磁盘
    brelse(b);
    return 0;
}

/**
 * 将指定簇的所有扇区清零
 * @param   cluster     要清零的簇号
 */
static void zero_clus(uint32 cluster)
{
    uint32 sec = first_sec_of_clus(cluster);
    struct buf *b;
    // 遍历簇中的每个扇区
    for (int i = 0; i < fat.bpb.sec_per_clus; i++) {
        b = bread(fat.dev, sec++);
        memset(b->data, 0, BSIZE); // 扇区数据清零
        bwrite(b);
        brelse(b);
    }
}

/**
 * 分配一个空闲簇
 * @param   dev         设备号
 * @return              分配到的簇号（失败触发panic）
 */
static uint32 alloc_clus(uint8 dev)
{
    // 优化点：应维护空闲簇列表，而非每次遍历FAT表
    struct buf *b;
    uint32 sec = fat.bpb.rsvd_sec_cnt; // FAT表起始扇区
    uint32 const ent_per_sec = fat.bpb.byts_per_sec / sizeof(uint32); // 每个扇区的FAT项数
    // 遍历FAT表的所有扇区
    for (uint32 i = 0; i < fat.bpb.fat_sz; i++, sec++) {
        b = bread(dev, sec);
        // 遍历扇区中的每个FAT项
        for (uint32 j = 0; j < ent_per_sec; j++) {
            if (((uint32 *)(b->data))[j] == 0) { // 找到空闲簇（FAT项为0）
                ((uint32 *)(b->data))[j] = FAT32_EOC + 7; // 标记为已占用
                bwrite(b);
                brelse(b);
                uint32 clus = i * ent_per_sec + j; // 计算簇号
                zero_clus(clus); // 簇清零
                return clus;
            }
        }
        brelse(b);
    }
    panic("no clusters"); // 无空闲簇
}

/**
 * 释放指定簇（标记为空闲）
 * @param   cluster     要释放的簇号
 */
static void free_clus(uint32 cluster)
{
    write_fat(cluster, 0); // FAT项置0表示空闲
}

/**
 * 读写指定簇中的数据（核心底层函数）
 * @param   cluster     簇号
 * @param   write       1=写，0=读
 * @param   user        1=用户态地址，0=内核态地址
 * @param   data        数据缓冲区地址（读：目标地址；写：源地址）
 * @param   off         簇内偏移量
 * @param   n           要读写的字节数
 * @return              实际读写的字节数
 */
static uint rw_clus(uint32 cluster, int write, int user, uint64 data, uint off, uint n)
{
    // 校验偏移+长度是否超出簇大小
    if (off + n > fat.byts_per_clus)
        panic("offset out of range");
    uint tot, m;
    struct buf *bp;
    // 计算起始扇区（簇内偏移转扇区）
    uint sec = first_sec_of_clus(cluster) + off / fat.bpb.byts_per_sec;
    off = off % fat.bpb.byts_per_sec; // 扇区内偏移

    int bad = 0;
    // 循环读写数据（跨扇区时拆分）
    for (tot = 0; tot < n; tot += m, off += m, data += m, sec++) {
        bp = bread(fat.dev, sec);
        // 本次读写的字节数（不超过扇区剩余空间）
        m = BSIZE - off % BSIZE;
        if (n - tot < m) {
            m = n - tot;
        }
        if (write) {
            // 写操作：从用户/内核地址拷贝到缓冲区
            if ((bad = either_copyin(bp->data + (off % BSIZE), user, data, m)) != -1) {
                bwrite(bp); // 写回磁盘
            }
        } else {
            // 读操作：从缓冲区拷贝到用户/内核地址
            bad = either_copyout(user, data, bp->data + (off % BSIZE), m);
        }
        brelse(bp);
        if (bad == -1) { // 拷贝失败，终止
            break;
        }
    }
    return tot;
}

/**
 * 根据文件偏移量定位到对应的簇（更新entry->cur_clus）
 * @param   entry       目录项
 * @param   off         文件偏移量
 * @param   alloc       1=簇链结束时分配新簇，0=不分配（返回-1）
 * @return              在当前簇内的偏移量（失败返回-1）
 */
static int reloc_clus(struct fat32_entry *entry, uint off, int alloc)
{
    // 计算偏移量对应的簇号（相对于文件起始）
    uint clus_num = off / fat.byts_per_clus;
    
    // 簇号大于当前已遍历的簇数：向后遍历簇链
    while (clus_num > entry->clus_cnt) {
        int clus = read_fat(entry->cur_clus); // 读取下一个簇号
        if (clus >= FAT32_EOC) { // 簇链结束
            if (alloc) { // 分配新簇
                clus = alloc_clus(entry->dev);
                write_fat(entry->cur_clus, clus); // 链接新簇
            } else { // 不分配，重置位置
                entry->cur_clus = entry->first_clus;
                entry->clus_cnt = 0;
                return -1;
            }
        }
        entry->cur_clus = clus;
        entry->clus_cnt++;
    }
    
    // 簇号小于当前已遍历的簇数：重置到文件起始，重新遍历
    if (clus_num < entry->clus_cnt) {
        entry->cur_clus = entry->first_clus;
        entry->clus_cnt = 0;
        while (entry->clus_cnt < clus_num) {
            entry->cur_clus = read_fat(entry->cur_clus);
            if (entry->cur_clus >= FAT32_EOC) {
                panic("reloc_clus");
            }
            entry->clus_cnt++;
        }
    }
    // 返回簇内偏移量
    return off % fat.byts_per_clus;
}

/* 读取文件数据（对应eread，命名避免和read冲突）
 * 调用者必须持有entry->lock
 * @param   entry       文件目录项
 * @param   user_dst    1=用户态目标地址，0=内核态
 * @param   dst         目标地址
 * @param   off         文件偏移量
 * @param   n           读取字节数
 * @return              实际读取的字节数
 */
int eread(struct fat32_entry *entry, int user_dst, uint64 dst, uint off, uint n)
{
    // 校验：偏移超出文件大小/溢出/是目录文件 → 返回0
    if (off > entry->file_size || off + n < off || (entry->attribute & ATTR_DIRECTORY)) {
        return 0;
    }
    // 修正读取长度：不超过文件剩余数据
    if (off + n > entry->file_size) {
        n = entry->file_size - off;
    }

    uint tot, m;
    // 遍历文件的簇链，读取数据
    for (tot = 0; entry->cur_clus < FAT32_EOC && tot < n; tot += m, off += m, dst += m) {
        reloc_clus(entry, off, 0); // 定位到偏移对应的簇（不分配新簇）
        // 本次读取的字节数（不超过簇剩余空间）
        m = fat.byts_per_clus - off % fat.byts_per_clus;
        if (n - tot < m) {
            m = n - tot;
        }
        // 读取簇内数据
        if (rw_clus(entry->cur_clus, 0, user_dst, dst, off % fat.byts_per_clus, m) != m) {
            break;
        }
    }
    return tot;
}

/* 写入文件数据（对应ewrite，命名避免和write冲突）
 * 调用者必须持有entry->lock
 * @param   entry       文件目录项
 * @param   user_src    1=用户态源地址，0=内核态
 * @param   src         源地址
 * @param   off         文件偏移量
 * @param   n           写入字节数
 * @return              实际写入的字节数（失败返回-1）
 */
int ewrite(struct fat32_entry *entry, int user_src, uint64 src, uint off, uint n)
{
    // 校验：偏移超出文件大小/溢出/超过32位大小限制/文件只读 → 返回-1
    if (off > entry->file_size || off + n < off || (uint64)off + n > 0xffffffff
        || (entry->attribute & ATTR_READ_ONLY)) {
        return -1;
    }
    // 文件首次写入：分配第一个簇
    if (entry->first_clus == 0) {   
        entry->cur_clus = entry->first_clus = alloc_clus(entry->dev);
        entry->clus_cnt = 0;
        entry->dirty = 1; // 标记为脏（需要更新磁盘目录项）
    }
    uint tot, m;
    // 遍历簇链，写入数据
    for (tot = 0; tot < n; tot += m, off += m, src += m) {
        reloc_clus(entry, off, 1); // 定位到偏移对应的簇（簇链结束则分配新簇）
        // 本次写入的字节数（不超过簇剩余空间）
        m = fat.byts_per_clus - off % fat.byts_per_clus;
        if (n - tot < m) {
            m = n - tot;
        }
        // 写入簇内数据
        if (rw_clus(entry->cur_clus, 1, user_src, src, off % fat.byts_per_clus, m) != m) {
            break;
        }
    }
    // 更新文件大小（如果写入超出原有大小）
    if(n > 0) {
        if(off > entry->file_size) {
            entry->file_size = off;
            entry->dirty = 1; // 标记为脏
        }
    }
    return tot;
}

/**
 * 获取目录项（从缓存/分配新项）
 * @param   parent      父目录项
 * @param   name        文件名（NULL表示仅分配）
 * @return              目录项指针（失败触发panic）
 */
static struct fat32_entry *eget(struct fat32_entry *parent, char *name)
{
    struct fat32_entry *ep;
    ecache.lock.acquire();
    // 如果指定了文件名，先查缓存（LRU算法）
    if (name) {
        for (ep = root.next; ep != &root; ep = ep->next) {          
            // 缓存项有效 + 父目录匹配 + 文件名匹配
            if (ep->valid == 1 && ep->parent == parent
                && strncmp(ep->filename, name, FAT32_MAX_FILENAME) == 0) {
                // 引用计数+1（0→1时父目录引用也+1）
                if (ep->ref++ == 0) {
                    ep->parent->ref++;
                }
                ecache.lock.release();
                return ep;
            }
        }
    }
    // 缓存未命中，分配新项（从链表尾部找引用计数为0的项，LRU）
    for (ep = root.prev; ep != &root; ep = ep->prev) {              
        if (ep->ref == 0) {
            ep->ref = 1;          // 引用计数置1
            ep->dev = parent->dev;
            ep->off = 0;
            ep->valid = 0;        // 标记为无效（需后续初始化）
            ep->dirty = 0;
            ecache.lock.release();
            return ep;
        }
    }
    panic("eget: insufficient ecache"); // 缓存满
    return 0;
}

/**
 * 格式化文件名（去除首尾空格/前置点，检查非法字符）
 * @param   name        原始文件名
 * @return              格式化后的文件名（非法返回NULL）
 */
char *formatname(char *name)
{
    static char illegal[] = { '\"', '*', '/', ':', '<', '>', '?', '\\', '|', 0 };
    char *p;
    // 跳过开头的空格和点
    while (*name == ' ' || *name == '.') { name++; }
    // 检查非法字符
    for (p = name; *p; p++) {
        char c = *p;
        if (c < 0x20 || strchr(illegal, c)) {
            return 0;
        }
    }
    // 去除尾部空格
    while (p-- > name) {
        if (*p != ' ') {
            p[1] = '\0';
            break;
        }
    }
    return name;
}

/**
 * 生成短文件名（兼容8.3格式）
 * @param   shortname   输出短文件名
 * @param   name        长文件名
 */
static void generate_shortname(char *shortname, char *name)
{
    static char illegal[] = { '+', ',', ';', '=', '[', ']', 0 };   // 短文件名非法字符
    int i = 0;
    char c, *p = name;
    // 找到最后一个点（分离主名和扩展名）
    for (int j = strlen(name) - 1; j >= 0; j--) {
        if (name[j] == '.') {
            p = name + j;
            break;
        }
    }
    // 生成8.3格式短文件名
    while (i < CHAR_SHORT_NAME && (c = *name++)) {
        if (i == 8 && p) { // 主名达到8位
            if (p + 1 < name) { break; }            // 无扩展名
            else {
                name = p + 1, p = 0; // 开始处理扩展名
                continue;
            }
        }
        if (c == ' ') { continue; } // 跳过空格
        if (c == '.') { // 处理点
            if (name > p) {                    // 最后一个点
                memset(shortname + i, ' ', 8 - i); // 主名补空格到8位
                i = 8, p = 0;
            }
            continue;
        }
        // 小写转大写
        if (c >= 'a' && c <= 'z') {
            c += 'A' - 'a';
        } else {
            // 非法字符替换为下划线
            if (strchr(illegal, c) != NULL) {
                c = '_';
            }
        }
        shortname[i++] = c;
    }
    // 补空格到11位（8+3）
    while (i < CHAR_SHORT_NAME) {
        shortname[i++] = ' ';
    }
}

/**
 * 计算短文件名的校验和（用于关联长/短文件名）
 * @param   shortname   短文件名
 * @return              校验和
 */
uint8 cal_checksum(uchar* shortname)
{
    uint8 sum = 0;
    for (int i = CHAR_SHORT_NAME; i != 0; i--) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *shortname++;
    }
    return sum;
}

/**
 * 生成磁盘格式的目录项并写入磁盘
 * 调用者必须持有dp->lock
 * @param   dp          父目录项
 * @param   ep          要写入的目录项
 * @param   off         在父目录中的偏移量（需提前通过dirlookup计算）
 */
void emake(struct fat32_entry *dp, struct fat32_entry *ep, uint off)
{
    if (!(dp->attribute & ATTR_DIRECTORY))
        panic("emake: not dir");
    if (off % sizeof(union dentry))
        panic("emake: not aligned");

    union dentry de;
    memset(&de, 0, sizeof(de));
    // 处理目录的"./"和"../"项（偏移0和32）
    if (off <= 32) {
        if (off == 0) {
            strncpy(de.sne.name, ".          ", sizeof(de.sne.name)); // "."项
        } else {
            strncpy(de.sne.name, "..         ", sizeof(de.sne.name)); // ".."项
        }
        de.sne.attr = ATTR_DIRECTORY; // 标记为目录
        de.sne.fst_clus_hi = (uint16)(ep->first_clus >> 16); // 起始簇号高16位
        de.sne.fst_clus_lo = (uint16)(ep->first_clus & 0xffff); // 起始簇号低16位
        de.sne.file_size = 0; // 目录大小为0（后续更新）
        // 定位到父目录的对应偏移，写入目录项
        off = reloc_clus(dp, off, 1);
        rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    } else {
        // 处理普通文件/目录（长文件名+短文件名）
        // 计算需要的长文件名项数
        int entcnt = (strlen(ep->filename) + CHAR_LONG_NAME - 1) / CHAR_LONG_NAME;   
        char shortname[CHAR_SHORT_NAME + 1];
        memset(shortname, 0, sizeof(shortname));
        generate_shortname(shortname, ep->filename); // 生成短文件名
        de.lne.checksum = cal_checksum((uchar *)shortname); // 计算校验和
        de.lne.attr = ATTR_LONG_NAME; // 标记为长文件名项

        // 写入所有长文件名项（倒序）
        for (int i = entcnt; i > 0; i--) {
            if ((de.lne.order = i) == entcnt) {
                de.lne.order |= LAST_LONG_ENTRY; // 标记最后一个长文件名项
            }
            char *p = ep->filename + (i - 1) * CHAR_LONG_NAME;
            uint8 *w = (uint8 *)de.lne.name1;
            int end = 0;
            // 填充长文件名的宽字符
            for (int j = 1; j <= CHAR_LONG_NAME; j++) {
                if (end) {
                    *w++ = 0xff; // 未使用的宽字符填0xffff
                    *w++ = 0xff;
                } else { 
                    if ((*w++ = *p++) == 0) {
                        end = 1;
                    }
                    *w++ = 0;
                }
                // 切换到不同的名字段
                switch (j) {
                    case 5:     w = (uint8 *)de.lne.name2; break;
                    case 11:    w = (uint8 *)de.lne.name3; break;
                }
            }
            // 写入长文件名项
            uint off2 = reloc_clus(dp, off, 1);
            rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off2, sizeof(de));
            off += sizeof(de);
        }
        // 写入短文件名项
        memset(&de, 0, sizeof(de));
        strncpy(de.sne.name, shortname, sizeof(de.sne.name));
        de.sne.attr = ep->attribute; // 文件属性
        de.sne.fst_clus_hi = (uint16)(ep->first_clus >> 16);
        de.sne.fst_clus_lo = (uint16)(ep->first_clus & 0xffff);
        de.sne.file_size = ep->file_size; // 文件大小
        off = reloc_clus(dp, off, 1);
        rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    }
}

/**
 * 在磁盘上分配一个新目录项
 * 调用者必须持有dp->lock
 * @param   dp          父目录项
 * @param   name        文件名
 * @param   attr        文件属性
 * @return              新目录项指针（失败返回NULL）
 */
struct fat32_entry *ealloc(struct fat32_entry *dp, char *name, int attr)
{
    if (!(dp->attribute & ATTR_DIRECTORY)) {
        panic("ealloc not dir");
    }
    // 校验目录项有效 + 格式化文件名（非法字符检查）
    if (dp->valid != 1 || !(name = formatname(name))) {        
        return NULL;
    }
    struct fat32_entry *ep;
    uint off = 0;
    // 检查文件是否已存在
    if ((ep = dirlookup(dp, name, &off)) != 0) {      
        return ep;
    }
    // 从缓存分配新目录项
    ep = eget(dp, name);
    elock(ep);
    // 初始化目录项属性
    ep->attribute = attr;
    ep->file_size = 0;
    ep->first_clus = 0;
    ep->parent = edup(dp); // 引用父目录
    ep->off = off;         // 在父目录中的偏移
    ep->clus_cnt = 0;
    ep->cur_clus = 0;
    ep->dirty = 0;
    strncpy(ep->filename, name, FAT32_MAX_FILENAME); // 保存文件名
    ep->filename[FAT32_MAX_FILENAME] = '\0';
    
    // 如果是目录：创建"./"和"../"项
    if (attr == ATTR_DIRECTORY) {    
        ep->attribute |= ATTR_DIRECTORY;
        ep->cur_clus = ep->first_clus = alloc_clus(dp->dev); // 分配目录的第一个簇
        emake(ep, ep, 0);  // 创建"./"项
        emake(ep, dp, 32); // 创建"../"项
    } else {
        ep->attribute |= ATTR_ARCHIVE; // 文件标记为归档属性
    }
    // 写入目录项到父目录
    emake(dp, ep, off);
    ep->valid = 1; // 标记为有效
    eunlock(ep);
    return ep;
}

/**
 * 增加目录项的引用计数
 * @param   entry       目录项
 * @return              目录项指针（方便链式调用）
 */
struct fat32_entry *edup(struct fat32_entry *entry)
{
    if (entry != 0) {
        ecache.lock.acquire();
        entry->ref++;
        ecache.lock.release();
    }
    return entry;
}

/**
 * 更新磁盘上的目录项（仅更新文件大小和起始簇号）
 * 调用者必须持有entry->parent->lock
 * @param   entry       要更新的目录项
 */
void eupdate(struct fat32_entry *entry)
{
    if (!entry->dirty || entry->valid != 1) { return; }
    uint entcnt = 0;
    // 定位到目录项的第一个长文件名项，读取项数
    uint32 off = reloc_clus(entry->parent, entry->off, 0);
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64) &entcnt, off, 1);
    entcnt &= ~LAST_LONG_ENTRY; // 清除最后一项标记
    // 定位到短文件名项
    off = reloc_clus(entry->parent, entry->off + (entcnt << 5), 0);
    union dentry de;
    // 读取短文件名项
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64)&de, off, sizeof(de));
    // 更新起始簇号和文件大小
    de.sne.fst_clus_hi = (uint16)(entry->first_clus >> 16);
    de.sne.fst_clus_lo = (uint16)(entry->first_clus & 0xffff);
    de.sne.file_size = entry->file_size;
    // 写回磁盘
    rw_clus(entry->parent->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    entry->dirty = 0; // 清除脏标记
}

/**
 * 从父目录中删除目录项
 * 调用者必须持有entry->lock和entry->parent->lock
 * @param   entry       要删除的目录项
 */
void eremove(struct fat32_entry *entry)
{
    if (entry->valid != 1) { return; }
    uint entcnt = 0;
    uint32 off = entry->off;
    // 定位到目录项的第一个长文件名项
    uint32 off2 = reloc_clus(entry->parent, off, 0);
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64) &entcnt, off2, 1);
    entcnt &= ~LAST_LONG_ENTRY;
    uint8 flag = EMPTY_ENTRY; // 空目录项标记（0xe5）
    // 将所有关联的目录项（长+短）标记为空
    for (uint i = 0; i <= entcnt; i++) {
        rw_clus(entry->parent->cur_clus, 1, 0, (uint64) &flag, off2, 1);
        off += 32;
        off2 = reloc_clus(entry->parent, off, 0);
    }
    entry->valid = -1; // 标记为已删除
}

/**
 * 截断文件（释放所有簇，文件大小置0）
 * 调用者必须持有entry->lock
 * @param   entry       要截断的文件目录项
 */
void etrunc(struct fat32_entry *entry)
{
    // 遍历文件的簇链，释放所有簇
    for (uint32 clus = entry->first_clus; clus >= 2 && clus < FAT32_EOC; ) {
        uint32 next = read_fat(clus);
        free_clus(clus);
        clus = next;
    }
    // 重置文件属性
    entry->file_size = 0;
    entry->first_clus = 0;
    entry->dirty = 1; // 标记为脏
}

/**
 * 加锁目录项（睡眠锁）
 * @param   entry       目录项
 */
void elock(struct fat32_entry *entry)
{
    if (entry == 0 || entry->ref < 1)
        panic("elock");
    entry->lock.acquire();
}

/**
 * 解锁目录项
 * @param   entry       目录项
 */
void eunlock(struct fat32_entry *entry)
{
    if (entry == 0 || !entry->lock.is_holding() || entry->ref < 1)
        panic("eunlock");
    entry->lock.release();
}

/**
 * 释放目录项的引用（核心函数）
 * 引用计数为0时：更新磁盘目录项/释放簇，并重排缓存链表（LRU）
 * @param   entry       目录项
 */
void eput(struct fat32_entry *entry)
{
    ecache.lock.acquire();
    // 非根目录 + 有效 + 引用计数为1（最后一个引用）
    if (entry != &root && entry->valid != 0 && entry->ref == 1) {
        // 加锁（引用计数1，不会阻塞）
        entry->lock.acquire();
        // 将目录项移到缓存链表头部（LRU：最近使用）
        entry->next->prev = entry->prev;
        entry->prev->next = entry->next;
        entry->next = root.next;
        entry->prev = &root;
        root.next->prev = entry;
        root.next = entry;
        ecache.lock.release();
        
        // 处理已删除的目录项：截断文件（释放簇）
        if (entry->valid == -1) {       
            etrunc(entry);
        } else {
            // 有效目录项：更新磁盘目录项
            elock(entry->parent);
            eupdate(entry);
            eunlock(entry->parent);
        }
        entry->lock.release();

        // 释放父目录引用
        struct fat32_entry *eparent = entry->parent;
        ecache.lock.acquire();
        entry->ref--;
        ecache.lock.release();
        if (entry->ref == 0) {
            eput(eparent);
        }
        return;
    }
    // 引用计数-1
    entry->ref--;
    ecache.lock.release();
}

/**
 * 获取目录项的元数据（填充struct stat）
 * @param   de          目录项
 * @param   st          stat结构体指针
 */
void estat(struct fat32_entry *de, struct stat *st)
{
    st->st_mode = (de->attribute & ATTR_DIRECTORY) ? S_IFDIR : S_IFREG;
    st->st_dev = de->dev;
    st->st_size = de->file_size;
    st->st_ino = 0;
    st->st_nlink = 1;
}

/**
 * 获取目录项的扩展元数据（填充struct kstat）
 * @param   de          目录项
 * @param   st          kstat结构体指针
 */
void ekstat(struct fat32_entry *de, struct Kstat *st)
{
  st->st_dev = de->dev;
  st->st_size = de->file_size;
  st->st_blksize = 0;
  st->st_blocks = (st->st_size + st->st_blksize - 1) / st->st_blksize;
  st->st_atime_nsec = 0;
  st->st_atime_sec = 0;
  st->st_ctime_nsec = 0;
  st->st_ctime_sec = 0;
  st->st_mtime_nsec = 0;
  st->st_mtime_sec = 0;
  st->st_dev = 0;
  st->st_rdev = de->dev;
  st->st_nlink = 1;
  st->st_ino = 0;
  st->st_mode = de->attribute;
}

/**
 * 从目录项中读取文件名
 * @param   buffer      输出文件名的缓冲区
 * @param   d           磁盘目录项（短/长）
 */
static void read_entry_name(char *buffer, union dentry *d)
{
    if (d->lne.attr == ATTR_LONG_NAME) {                       // 长文件名分支
        wchar temp[NELEM(d->lne.name1)];
        memmove(temp, d->lne.name1, sizeof(temp));
        snstr(buffer, temp, NELEM(d->lne.name1));
        buffer += NELEM(d->lne.name1);
        snstr(buffer, d->lne.name2, NELEM(d->lne.name2));
        buffer += NELEM(d->lne.name2);
        snstr(buffer, d->lne.name3, NELEM(d->lne.name3));
    } else {
        // 仅处理"./"和"../"短文件名
        memset(buffer, 0, CHAR_SHORT_NAME + 2); 
        int i;
        for (i = 0; d->sne.name[i] != ' ' && i < 8; i++) {
            buffer[i] = d->sne.name[i];
        }
        if (d->sne.name[8] != ' ') {
            buffer[i++] = '.';
        }
        for (int j = 8; j < CHAR_SHORT_NAME; j++, i++) {
            if (d->sne.name[j] == ' ') { break; }
            buffer[i] = d->sne.name[j];
        }
    }
}

/**
 * 从目录项中读取文件信息（起始簇号、大小、属性）
 * @param   entry       输出目录项
 * @param   d           磁盘目录项
 */
static void read_entry_info(struct fat32_entry *entry, union dentry *d)
{
    entry->attribute = d->sne.attr;
    // 合并起始簇号（高16位+低16位）
    entry->first_clus = ((uint32)d->sne.fst_clus_hi << 16) | d->sne.fst_clus_lo;
    entry->file_size = d->sne.file_size;
    entry->cur_clus = entry->first_clus;
    entry->clus_cnt = 0;
}

/**
 * 从目录的指定偏移读取下一个目录项
 * 调用者必须持有dp->lock
 * @param   dp          目录项
 * @param   ep          输出的目录项
 * @param   off         目录偏移量
 * @param   count       输出目录项占用的32字节块数
 * @return  -1          目录遍历结束
 *          0           找到空目录项
 *          1           找到有效文件/目录项
 */
int enext(struct fat32_entry *dp, struct fat32_entry *ep, uint off, int *count)
{
    if (!(dp->attribute & ATTR_DIRECTORY))
        panic("enext not dir");
    if (ep->valid)
        panic("enext ep valid");
    if (off % 32)
        panic("enext not align");
    if (dp->valid != 1) { return -1; }

    union dentry de;
    int cnt = 0;
    memset(ep->filename, 0, FAT32_MAX_FILENAME + 1);
    // 遍历目录的簇链，读取目录项
    for (int off2; (off2 = reloc_clus(dp, off, 0)) != -1; off += 32) {
        // 读取32字节目录项
        if (rw_clus(dp->cur_clus, 0, 0, (uint64)&de, off2, 32) != 32 || de.lne.order == END_OF_ENTRY) {
            return -1;
        }
        // 空目录项：计数+1
        if (de.lne.order == EMPTY_ENTRY) {
            cnt++;
            continue;
        } else if (cnt) { // 找到连续空项
            *count = cnt;
            return 0;
        }
        // 长文件名项：拼接文件名
        if (de.lne.attr == ATTR_LONG_NAME) {
            int lcnt = de.lne.order & ~LAST_LONG_ENTRY;
            if (de.lne.order & LAST_LONG_ENTRY) {
                *count = lcnt + 1; // 长文件名项数 + 短文件名项
                count = 0;
            }
            read_entry_name(ep->filename + (lcnt - 1) * CHAR_LONG_NAME, &de);
        } else { // 短文件名项：读取文件信息
            if (count) {
                *count = 1;
                read_entry_name(ep->filename, &de);
            }
            read_entry_info(ep, &de);
            return 1;
        }
    }
    return -1;
}

/**
 * 在目录中查找指定文件名的目录项
 * 同时记录可用于创建新文件的空目录项偏移
 * 调用者必须持有entry->lock
 * @param   dp          目录项
 * @param   filename    目标文件名
 * @param   poff        输出空目录项偏移（NULL则不输出）
 * @return              找到的目录项（NULL表示未找到）
 */
struct fat32_entry *dirlookup(struct fat32_entry *dp, char *filename, uint *poff)
{
    if (!(dp->attribute & ATTR_DIRECTORY))
        panic("dirlookup not DIR");
    // 处理"./"和"../"
    if (strncmp(filename, ".", FAT32_MAX_FILENAME) == 0) {
        return edup(dp);
    } else if (strncmp(filename, "..", FAT32_MAX_FILENAME) == 0) {
        if (dp == &root) {
            return edup(&root);
        }
        return edup(dp->parent);
    }
    if (dp->valid != 1) {
        return NULL;
    }
    // 先查缓存
    struct fat32_entry *ep = eget(dp, filename);
    if (ep->valid == 1) { return ep; }                              

    // 计算需要的目录项数（长文件名+短文件名）
    int len = strlen(filename);
    int entcnt = (len + CHAR_LONG_NAME - 1) / CHAR_LONG_NAME + 1;   
    int count = 0;
    int type;
    uint off = 0;
    reloc_clus(dp, 0, 0); // 定位到目录起始
    // 遍历目录项
    while ((type = enext(dp, ep, off, &count) != -1)) {
        if (type == 0) { // 找到空目录项
            if (poff && count >= entcnt) { // 空项数足够
                *poff = off;
                poff = 0;
            }
        } else if (strncmp(filename, ep->filename, FAT32_MAX_FILENAME) == 0) { // 找到目标文件
            ep->parent = edup(dp);
            ep->off = off;
            ep->valid = 1;
            return ep;
        }
        off += count << 5; // 偏移 += count * 32
    }
    // 未找到文件，记录空项偏移
    if (poff) {
        *poff = off;
    }
    eput(ep);
    return NULL;
}

/**
 * 解析路径中的下一个元素（分割路径）
 * @param   path        输入路径
 * @param   name        输出路径元素
 * @return              剩余路径（NULL表示解析完成）
 */
static char *skipelem(char *path, char *name)
{
    // 跳过开头的'/'
    while (*path == '/') {
        path++;
    }
    if (*path == 0) { return NULL; }
    char *s = path;
    // 找到下一个'/'或结束符
    while (*path != '/' && *path != 0) {
        path++;
    }
    int len = path - s;
    if (len > FAT32_MAX_FILENAME) {
        len = FAT32_MAX_FILENAME;
    }
    name[len] = 0;
    memmove(name, s, len); // 拷贝路径元素
    // 跳过后续的'/'
    while (*path == '/') {
        path++;
    }
    return path;
}

// FAT32版本的路径查找函数（对应xv6原始文件系统的namex）
static struct fat32_entry *lookup_path(char *path, int parent, char *name)
{
    struct fat32_entry *entry, *next;
    // 初始化起始目录：绝对路径→根目录，相对路径→当前工作目录
    if (*path == '/') {
        entry = edup(&root);
    } else if (*path != '\0') {
        // entry = edup(myproc()->cwd);
        panic("Relative path not supported in internal fat32 driver");
        return NULL;
    } else {
        return NULL;
    }
    // 逐段解析路径
    while ((path = skipelem(path, name)) != 0) {
        elock(entry);
        // 当前项不是目录 → 失败
        if (!(entry->attribute & ATTR_DIRECTORY)) {
            eunlock(entry);
            eput(entry);
            return NULL;
        }
        // parent=1且路径解析完成 → 返回父目录
        if (parent && *path == '\0') {
            eunlock(entry);
            return entry;
        }
        // 查找下一个路径元素
        if ((next = dirlookup(entry, name, 0)) == 0) {
            eunlock(entry);
            eput(entry);
            return NULL;
        }
        // 释放当前目录，切换到下一个
        eunlock(entry);
        eput(entry);
        entry = next;
    }
    // parent=1 → 释放并返回NULL
    if (parent) {
        eput(entry);
        return NULL;
    }
    return entry;
}

/**
 * 根据路径查找文件/目录
 * @param   path        文件路径
 * @return              目录项指针（NULL表示未找到）
 */
struct fat32_entry *ename(char *path)
{
    char name[FAT32_MAX_FILENAME + 1];
    return lookup_path(path, 0, name);
}

/**
 * 根据路径查找父目录，并输出最后一个路径元素
 * @param   path        文件路径
 * @param   name        输出最后一个路径元素
 * @return              父目录项指针（NULL表示未找到）
 */
struct fat32_entry *enameparent(char *path, char *name)
{
    return lookup_path(path, 1, name);
}

/**
 * 挂载FAT32文件系统
 * @param   dev         设备目录项
 * @param   mnt         挂载点目录项
 * @return              0成功
 */
int mount(struct fat32_entry *dev, struct fat32_entry *mnt)
{
    // 找到空闲的挂载项
    while (devs[idx].vaild != 0){
        idx++;
        idx = idx % 8;
    }

    // 读取设备的引导扇区，校验FAT32
    struct buf *b = bread(dev->dev, 0);
    if (strncmp((char const *)(b->data + 82), "FAT32", 5))
        panic("not FAT32 volume");
    // 读取BPB参数
    memmove(&devs[idx].bpb.byts_per_sec, b->data + 11, 2); 
    devs[idx].bpb.sec_per_clus = *(b->data + 13);
    devs[idx].bpb.rsvd_sec_cnt = *(uint16 *)(b->data + 14);
    devs[idx].bpb.fat_cnt = *(b->data + 16);
    devs[idx].bpb.hidd_sec = *(uint32 *)(b->data + 28);
    devs[idx].bpb.tot_sec = *(uint32 *)(b->data + 32);
    devs[idx].bpb.fat_sz = *(uint32 *)(b->data + 36);
    devs[idx].bpb.root_clus = *(uint32 *)(b->data + 44);
    // 计算核心参数
    devs[idx].first_data_sec = fat.bpb.rsvd_sec_cnt + fat.bpb.fat_cnt * fat.bpb.fat_sz;
    devs[idx].data_sec_cnt = fat.bpb.tot_sec - fat.first_data_sec;
    devs[idx].data_clus_cnt = fat.data_sec_cnt / fat.bpb.sec_per_clus;
    devs[idx].byts_per_clus = fat.bpb.sec_per_clus * fat.bpb.byts_per_sec;
    brelse(b);

    // 校验扇区大小
    if (BSIZE != devs[idx].bpb.byts_per_sec)
        panic("byts_per_sec != BSIZE");
    ecache.lock.init("ecache");
    // 初始化挂载设备的根目录
    memset(&devs[idx].root, 0, sizeof(devs[idx].root));
    root.lock.init("entry", "entry");
    devs[idx].root.attribute = (ATTR_DIRECTORY | ATTR_SYSTEM);
    devs[idx].root.first_clus = devs[idx].root.cur_clus = devs[idx].bpb.root_clus;
    devs[idx].root.valid = 1;
    devs[idx].root.prev = &devs[idx].root;
    devs[idx].root.next = &devs[idx].root;
    devs[idx].root.filename[0] = '/';
    devs[idx].root.filename[1] = '\0';
    // 标记为已挂载
    devs[idx].mount_mode = 1;
    mnt->mount_flag = 1;
    mnt->dev = idx;
    return 0;
}

/**
 * 卸载FAT32文件系统
 * @param   mnt         挂载点目录项
 * @return              0成功
 */
int umount2(struct fat32_entry *mnt)
{
    mnt->mount_flag = 0;
    memset(&devs[mnt->dev], 0, sizeof(devs[mnt->dev]));
    mnt->dev = 0;
    return 0;
}
// VFS Operations Wrapper

int fat32_fs_mount(filesystem_t *fs, unsigned long rwflag, void *data) {
    return fat32_init_internal(fs->dev);
}

int fat32_fs_umount(filesystem_t *fs) {
    return 0;
}

int fat32_fs_statfs(filesystem_t *fs, struct statfs *buf) {
    return 0;
}

filesystem_op_t fat32_fs_op = {
    .mount = fat32_fs_mount,
    .umount = fat32_fs_umount,
    .statfs = fat32_fs_statfs
};
