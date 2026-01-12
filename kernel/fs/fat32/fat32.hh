#ifndef __FAT32_H
#define __FAT32_H

#include "proc/sleeplock.hh"
#include "fs/stat.hh"
#include "types.hh"

#define ATTR_READ_ONLY      0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_VOLUME_ID      0x08
#define ATTR_DIRECTORY      0x10
#define ATTR_ARCHIVE        0x20
#define ATTR_LONG_NAME      0x0F

#define LAST_LONG_ENTRY     0x40
#define FAT32_EOC           0x0ffffff8
#define EMPTY_ENTRY         0xe5
#define END_OF_ENTRY        0x00
#define CHAR_LONG_NAME      13
#define CHAR_SHORT_NAME     11

#define FAT32_MAX_FILENAME  255
#define FAT32_MAX_PATH      260
#define ENTRY_CACHE_NUM     50

struct fat32_entry {
    char  filename[FAT32_MAX_FILENAME + 1];
    uint8   attribute;
    // uint8   create_time_tenth;
    // uint16  create_time;
    // uint16  create_date;
    // uint16  last_access_date;
    uint32  first_clus;
    // uint16  last_write_time;
    // uint16  last_write_date;
    uint32  file_size;

    uint32  cur_clus;
    uint    clus_cnt;

    /* for OS */
    uint8   dev;
    uint8   dirty;
    short   valid;
    int     ref;
    uint32  off;            // offset in the parent dir entry, for writing convenience
    struct fat32_entry *parent;  // because FAT32 doesn't have such thing like inum, use this for cache trick
    struct fat32_entry *next;
    struct fat32_entry *prev;
    proc::SleepLock    lock;
    uint8 mount_flag;
};

struct mntfs
{
    uint32 first_data_sec;
    uint32 data_sec_cnt;
    uint32 data_clus_cnt;
    uint32 byts_per_clus;

    struct
    {
        uint16 byts_per_sec;
        uint8 sec_per_clus;
        uint16 rsvd_sec_cnt;
        uint8 fat_cnt;   /* count of FAT regions */
        uint32 hidd_sec; /* count of hidden sectors */
        uint32 tot_sec;  /* total count of sectors including all regions */
        uint32 fat_sz;   /* count of sectors for a FAT region */
        uint32 root_clus;
    } bpb;

    int vaild;
    struct fat32_entry root;
    uint8 mount_mode;
};

#include "fs/vfs/fs.hh"

int             fat32_init_internal(uint32 dev);
struct fat32_entry*  dirlookup(struct fat32_entry *entry, char *filename, uint *poff);
char*           formatname(char *name);
void            emake(struct fat32_entry *dp, struct fat32_entry *ep, uint off);
struct fat32_entry*  ealloc(struct fat32_entry *dp, char *name, int attr);
struct fat32_entry*  edup(struct fat32_entry *entry);
void            eupdate(struct fat32_entry *entry);
void            etrunc(struct fat32_entry *entry);
void            eremove(struct fat32_entry *entry);
void            eput(struct fat32_entry *entry);
void            estat(struct fat32_entry *ep, struct stat *st);
void            ekstat(struct fat32_entry *ep, struct Kstat *st);
void            elock(struct fat32_entry *entry);
void            eunlock(struct fat32_entry *entry);
int             enext(struct fat32_entry *dp, struct fat32_entry *ep, uint off, int *count);
struct fat32_entry*  ename(char *path);
struct fat32_entry*  enameparent(char *path, char *name);
int             eread(struct fat32_entry *entry, int user_dst, uint64 dst, uint off, uint n);
int             ewrite(struct fat32_entry *entry, int user_src, uint64 src, uint off, uint n);
int             mount(struct fat32_entry *dev, struct fat32_entry *mnt);
int             umount2(struct fat32_entry *mnt);

extern filesystem_op_t fat32_fs_op;

#endif