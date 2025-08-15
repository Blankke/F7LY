// #ifndef __FS_DEFS_H__
// #define __FS_DEFS_H__

// #define NOFILE       128                ///< open files per process
// #define NFILE        1024               ///< open files per system
// #define NINODE       1024               ///< maximum number of active i-nodes
// #define NDEV         10                 ///< maximum major device number
// #define ROOTDEV       1                 ///< device number of file system root disk
// #define MAXARG       32                 ///< max exec arguments
// #define MAXOPBLOCKS  1000               ///< max # of blocks any FS op writes
// #define LOGSIZE      (MAXOPBLOCKS * 3)  ///< max data blocks in on-disk log
// #define NBUF         (MAXOPBLOCKS * 3)  ///< size of disk block cache
// #define FSSIZE       1000               ///< size of file system in blocks
// #define MAXPATH      512                ///< maximum file path name
// #define VFS_MAX_FS   4                  ///< VFS 中最多的fs个数
// #define TMPDEV       2                  ///< NOTE 用于挂载的临时设备号
// #define MAXFILENAME  32                 ///< 单个文件最长
// #define min(a, b) ((a) < (b) ? (a) : (b))
// #define ZERO_BYTES   32                 ///< 零字符数组大小

#define R_OK	4		/* Test for read permission.  */
#define W_OK	2		/* Test for write permission.  */
#define X_OK	1		/* Test for execute permission.  */
#define F_OK	0		/* Test for existence.  */  

// #endif ///< __FS_DEFS_H__