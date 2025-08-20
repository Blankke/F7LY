#pragma once
#include "types.hh"
#define T_DIR     4     // Directory
#define T_FILE    8   // File
#define T_DEVICE  6   // Device
#define T_CHR     2   // 字符设备
#define T_BLK     6   // 块设备
#define T_FIFO    1   // FIFO/命名管道
#define T_SOCK    12   // Socket
#define T_UNKNOWN 14

// struct stat {
//   int dev;     // File system's disk device
//   uint ino;    // Inode number
//   short type;  // Type of file
//   short nlink; // Number of links to file
//   uint64 size; // Size of file in bytes
// };

//文件状态
struct Kstat {
  uint64 st_dev;
  uint64 st_ino;
  uint32 st_mode;
  uint32 st_nlink;
  uint32 st_uid;
  uint32 st_gid;
  uint64 st_rdev;
  uint64 __pad;
  uint64 st_size;
  uint32 st_blksize;
  uint32 __pad2;
  uint64 st_blocks;
  uint64 st_atime_sec;
  uint64 st_atime_nsec;
  uint64 st_mtime_sec;
  uint64 st_mtime_nsec;
  uint64 st_ctime_sec;
  uint64 st_ctime_nsec;
  // unsigned __unused[2];
};

struct statx
{
  uint32 stx_mask;
  uint32 stx_blksize;
  uint64 stx_attributes;
  uint32 stx_nlink;
  uint32 stx_uid;
  uint32 stx_gid;
  uint16 stx_mode;
  uint16 pad1;
  uint64 stx_ino;
  uint64 stx_size;
  uint64 stx_blocks;
  uint64 stx_attributes_mask;
  struct
  {
    int64 tv_sec;
    uint32 tv_nsec;
    int pad;
  } stx_atime, stx_btime, stx_ctime, stx_mtime;
  uint32 stx_rdev_major;
  uint32 stx_rdev_minor;
  uint32 stx_dev_major;
  uint32 stx_dev_minor;
  uint64 spare[14];
};

// typedef struct {
//   int val[2];
// } __kernel_fsid_t;
// typedef __kernel_fsid_t fsid_t;

// struct statfs {
//   uint64 f_type; /* type of file system (see below) */
//   uint64 f_bsize; /* optimal transfer block size */
//   uint64 f_blocks; /* total data blocks in file system */
//   uint64 f_bfree; /* free blocks in fs */
//   uint64 f_bavail; /* free blocks available to
//                           unprivileged user */
//   uint64 f_files; /* total file nodes in file system */
//   uint64 f_ffree; /* free file nodes in fs */
//   fsid_t f_fsid; /* file system ID */
//   uint64 f_namelen; /* maximum length of filenames */
//   uint64 f_frsize; /* fragment size (since Linux 2.6) */
//   uint64 f_flags; /* mount flags of filesystem (since Linux 2.6.36) */
//   uint64 f_spare[4]; /* padding for future expansion */
// };

//For writev readv
// struct iovec {
//   void *iov_base;
//   int iov_len;
// };

#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)