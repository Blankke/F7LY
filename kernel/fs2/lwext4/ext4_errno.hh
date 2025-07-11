

/**
 * @file  ext4_errno.hh
 * @brief Error codes.
 */
#ifndef EXT4_ERRNO_H_
#define EXT4_ERRNO_H_


#include <lwext4/ext4_config.hh>

#define EPERM 1 /* Operation not permitted */
#define ENOENT 2 /* No such file or directory */
#define EIO 5 /* I/O error */
#define ENXIO 6 /* No such device or address */
#define E2BIG 7 /* Argument list too long */
#define ENOMEM 12 /* Out of memory */
#define EACCES 13 /* Permission denied */
#define EFAULT 14 /* Bad address */
#define EEXIST 17 /* File exists */
#define ENODEV 19 /* No such device */
#define ENOTDIR 20 /* Not a directory */
#define EISDIR 21 /* Is a directory */
#define EINVAL 22 /* Invalid argument */
#define EFBIG 27 /* File too large */
#define ENOSPC 28 /* No space left on device */
#define EROFS 30 /* Read-only file system */
#define EMLINK 31 /* Too many links */
#define ERANGE 34 /* Math result not representable */
#define ENOTEMPTY 39 /* Directory not empty */
#define ENODATA 61 /* No data available */
#define ENOTSUP 95 /* Not supported */

#ifndef ENODATA
#ifdef ENOATTR
#define ENODATA ENOATTR
#else
#define ENODATA 61
#endif
#endif

#ifndef ENOTSUP
#define ENOTSUP 95
#endif

#ifndef EOK
#define EOK 0
#endif


#endif /* EXT4_ERRNO_H_ */

/**
 * @}
 */
