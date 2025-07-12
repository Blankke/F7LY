#pragma once

#include "libs/clist.h"
#include "fs2/vfs/inodehash.hh"
#include "semaphore.hh"
#include "sleeplock.hh"
#include "param.h"
#include "fs2/stat.hh"

#include "fs2/lwext4/ext4.hh"

struct superblock;
struct inode;

struct super_operations {

};

/*
 *
 *超级块
 */
struct superblock {
    uint8 s_dev; //块设备标识符
    uint32 s_blocksize; //数据块大小，字节单位

    uint32 s_magic; //文件系统的魔数
    uint32 s_maxbytes; //最大文件大小
    struct inode *root; //指根目录

    struct super_operations *s_op;

    SpinLock dirty_lock;
    struct list_head s_dirty_inodes; //脏inode表
};

struct inode_operations {
    void (*unlockput)(struct inode *self);
    void (*unlock)(struct inode *self);
    void (*put)(struct inode *self);
    void (*lock)(struct inode *self);
    void (*update)(struct inode *self);

    ssize_t (*read)(struct inode *self, int user_dst, uint64 dst, uint off, uint n);
    int (*write)(struct inode *self, int user_src, uint64 src, uint off, uint n);


    int (*isdir)(struct inode *self); // 是否是directory

    struct inode *(*dup)(struct inode *self);

    //For directory
    struct inode *(*dirlookup)(struct inode *self, const char *name, uint *poff);
    ///TODO: delete是c语言关键词，这里改个名，后续遇到了记得处理
    int (*deletei)(struct inode *self, struct inode *ip);            
    int (*dir_empty)(struct inode *self);
    //返回时要持有新ip的锁
    struct inode *(*create)(struct inode *self, const char *name, uchar type, short major, short minor);

    void (*stat)(struct inode *self, struct stat *st);
};

extern struct inode_operations inode_ops;

#define EXT4_PATH_LONG_MAX 512

struct vfs_ext4_inode_info {
    char fname[EXT4_PATH_LONG_MAX];
};

/*
 *索引节点
 */
struct inode {
    uint8 i_dev;
    uint16 i_mode; //类型 & 访问权限
    uint16 i_type;
    uint32 i_ino; //编号
    uint32 i_valid; // 是否可用 0 -> 未使用

    uint16 i_count; //引用计数
    uint16 i_nlink; //硬链接数
    uint i_uid; //拥有者编号
    uint i_gid; //拥有者组编号
    uint64 i_rdev; //当文件代表设备的时候，rdev代表这个设备的实际编号
    uint64 i_size; //文件大小

    long i_atime; //文件最后一次访问时间
    long i_mtime; //文件最后一次修改时间
    long i_ctime; //inode最后一次修改时间

    uint64 i_blocks; //文件有多少块
    uint64 i_blksize; //块大小 bytes
    // struct semaphore i_sem; //同步信号量
    SpinLock lock; //测试完成后再换成信号量
    struct inode_operations *i_op; //inode操作函数

    struct superblock *i_sb;

    struct vfs_ext4_inode_info i_info; //EXT4 inode结构
};


void inodeinit();
struct inode *get_inode();
void free_inode(struct inode *inode);





