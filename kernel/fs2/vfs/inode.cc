#include "types.hh"
#include "param.h"

#include "spinlock.hh"
#include "fs2/vfs/inode.hh"
#include "vfs_ext4_ext.hh"

struct {
    SpinLock lock;
    struct inode inode[NINODE];
} itable;

void inodeinit()
{
    int i = 0;
    // printf("--- %d ---\n", sizeof(struct inode));
    itable.lock.init( "itable");
    for(i = 0; i < NINODE; i++) {
        itable.inode[i].lock.init("inode");
        itable.inode[i].i_op = get_ext4_inode_op();
    }
}

struct inode *get_inode() {
    int i;
    itable.lock.acquire();
    for(i = 0; i < NINODE; i++) {
        if (itable.inode[i].i_valid == 0) {
            itable.inode[i].i_valid = 1;
            break;
        }
    }
    itable.lock.release();
    if (i == NINODE) {
        return NULL;
    }
    return &itable.inode[i];
}
struct inode *free_inode(struct inode *inode) {
    itable.lock.acquire();
    inode->i_valid = 0;
    itable.lock.release();
}
































