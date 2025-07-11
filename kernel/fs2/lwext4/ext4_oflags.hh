


/**
 * @file  ext4_oflags.h
 * @brief File opening & seeking flags.
 */
#ifndef EXT4_OFLAGS_H_
#define EXT4_OFLAGS_H_


#ifndef O_RDONLY
#define O_RDONLY 00
#endif

#ifndef O_WRONLY
#define O_WRONLY 01
#endif

#ifndef O_RDWR
#define O_RDWR 02
#endif

#ifndef O_CREAT
#define O_CREAT 0100
#endif

#ifndef O_EXCL
#define O_EXCL 0200
#endif

#ifndef O_TRUNC
#define O_TRUNC 01000
#endif

#ifndef O_APPEND
#define O_APPEND 02000

/********************************FILE SEEK FLAGS*****************************/

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif

#ifndef SEEK_END
#define SEEK_END 2
#endif
#endif


#endif /* EXT4_OFLAGS_H_ */

/**
 * @}
 */
