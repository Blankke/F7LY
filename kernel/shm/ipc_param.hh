#include "types.hh"
typedef unsigned long shmatt_t;
/* Shared memory attach boundary - typically page size */
#define SHMLBA		PGSIZE		/* segment attach address boundary */

/* Permission flag for shmget.  */
#define SHM_R		0400		/* or S_IRUGO from <linux/stat.h> */
#define SHM_W		0200		/* or S_IWUGO from <linux/stat.h> */

/* Flags for `shmat'.  */
#define SHM_RDONLY	010000		/* attach read-only else read-write */
#define SHM_RND		020000		/* round attach address to SHMLBA */
#define SHM_REMAP	040000		/* take-over region on attach */
#define SHM_EXEC	0100000		/* execution access */

/* Commands for `shmctl'.  */
#define SHM_LOCK	11		/* lock segment (root only) */
#define SHM_UNLOCK	12		/* unlock segment (root only) */

/* ipcs ctl commands */
# define SHM_STAT 	13
# define SHM_INFO 	14
# define SHM_STAT_ANY	15

/* shm_mode upper byte flags */
# define SHM_DEST	01000	/* segment will be destroyed on last detach */
# define SHM_LOCKED	02000   /* segment will not be swapped */
# define SHM_HUGETLB	04000	/* segment is mapped via hugetlb */
# define SHM_NORESERVE	010000	/* don't check for reservations */

struct	shminfo
  {
    __syscall_ulong_t shmmax;
    __syscall_ulong_t shmmin;
    __syscall_ulong_t shmmni;
    __syscall_ulong_t shmseg;
    __syscall_ulong_t shmall;
    __syscall_ulong_t __glibc_reserved1;
    __syscall_ulong_t __glibc_reserved2;
    __syscall_ulong_t __glibc_reserved3;
    __syscall_ulong_t __glibc_reserved4;
  };

struct shm_info
  {
    int used_ids;
    __syscall_ulong_t shm_tot;	/* total allocated shm */
    __syscall_ulong_t shm_rss;	/* total resident shm */
    __syscall_ulong_t shm_swp;	/* total swapped shm */
    __syscall_ulong_t swap_attempts;
    __syscall_ulong_t swap_successes;
  };

  /* Mode bits for `msgget', `semget', and `shmget'.  */
#define IPC_CREAT	01000		/* Create key if key does not exist. */
#define IPC_EXCL	02000		/* Fail if key exists.  */
#define IPC_NOWAIT	04000		/* Return error on wait.  */

/* Control commands for `msgctl', `semctl', and `shmctl'.  */
#define IPC_RMID	0		/* Remove identifier.  */
#define IPC_SET		1		/* Set `ipc_perm' options.  */
#define IPC_STAT	2		/* Get `ipc_perm' options.  */

# define IPC_INFO	3		/* See ipcs.  */

struct ipc_perm
{
  __key_t __key;				/* Key.  */
  __uid_t uid;					/* Owner's user ID.  */
  __gid_t gid;					/* Owner's group ID.  */
  __uid_t cuid;					/* Creator's user ID.  */
  __gid_t cgid;					/* Creator's group ID.  */
  __mode_t mode;				/* Read/write permission.  */
  unsigned short int __seq;			/* Sequence number.  */
  unsigned short int __pad2;
  __syscall_ulong_t __glibc_reserved1;
  __syscall_ulong_t __glibc_reserved2;
};

struct shmid_ds
  {
    struct ipc_perm shm_perm;		/* operation permission struct */
    size_t shm_segsz;			/* size of segment in bytes */

    __time_t shm_atime;			/* time of last shmat() */
    __time_t shm_dtime;			/* time of last shmdt() */
    __time_t shm_ctime;			/* time of last change by shmctl() */

    __pid_t shm_cpid;			/* pid of creator */
    __pid_t shm_lpid;			/* pid of last shmop */
    shmatt_t shm_nattch;		/* number of current attaches */
    __syscall_ulong_t __glibc_reserved5;
    __syscall_ulong_t __glibc_reserved6;
  };
