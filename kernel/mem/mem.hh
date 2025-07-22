#pragma once

// Protection flags for mmap
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define PROT_GROWSDOWN 0X01000000
#define PROT_GROWSUP 0X02000000

// Mapping flags for mmap
#define MAP_FILE 0
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10          /* Interpret addr exactly */
#define MAP_ANONYMOUS 0x20      /* Don't use a file */
#define MAP_ANON MAP_ANONYMOUS  /* Synonym for MAP_ANONYMOUS */
#define MAP_GROWSDOWN 0x0100    /* Stack mapping grows downward */
#define MAP_DENYWRITE 0x0800    /* Ignored for compatibility */
#define MAP_EXECUTABLE 0x1000   /* Ignored for compatibility */
#define MAP_LOCKED 0x2000       /* Pages are locked in memory */
#define MAP_NORESERVE 0x4000    /* Don't reserve swap space */
#define MAP_POPULATE 0x8000     /* Prefault pages */
#define MAP_NONBLOCK 0x10000    /* Don't block on page faults */
#define MAP_STACK 0x20000       /* Allocate at address suitable for stack */
#define MAP_HUGETLB 0x40000     /* Use huge pages */
#define MAP_SYNC 0x80000        /* Persistent memory synchronization */
#define MAP_FIXED_NOREPLACE 0x100000 /* MAP_FIXED but don't replace existing mapping */
#define MAP_SHARED_VALIDATE 0x03 /* Share this mapping, validate flags */
#define MAP_UNINITIALIZED 0x4000000 /* Don't clear anonymous pages */

// Error codes for mmap
#define MAP_FAILED ((void *)-1)