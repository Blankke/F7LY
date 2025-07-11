

/**
 * @file  ext4_config.hh
 * @brief Configuration file.
 */

#ifndef EXT4_CONFIG_H_
#define EXT4_CONFIG_H_

/*****************************************************************************/
#define CONFIG_SUPPORTED_FCOM EXT4_SUPPORTED_FCOM
#define CONFIG_SUPPORTED_FINCOM (EXT4_SUPPORTED_FINCOM | EXT_FINCOM_IGNORED)
#define CONFIG_SUPPORTED_FRO_COM EXT4_SUPPORTED_FRO_COM


#define CONFIG_DIR_INDEX_ENABLE (CONFIG_SUPPORTED_FCOM & EXT4_FCOM_DIR_INDEX)
#define CONFIG_EXTENT_ENABLE (CONFIG_SUPPORTED_FINCOM & EXT4_FINCOM_EXTENTS)
#define CONFIG_META_CSUM_ENABLE (CONFIG_SUPPORTED_FRO_COM & EXT4_FRO_COM_METADATA_CSUM)

/*****************************************************************************/

/**@brief  Enable/disable journaling*/
#ifndef CONFIG_JOURNALING_ENABLE
#define CONFIG_JOURNALING_ENABLE 0
#endif

/**@brief  Enable/disable xattr*/
#ifndef CONFIG_XATTR_ENABLE
#define CONFIG_XATTR_ENABLE 1
#endif

/**@brief  Enable/disable extents*/
#ifndef CONFIG_EXTENTS_ENABLE
#define CONFIG_EXTENTS_ENABLE 1
#endif


/**@brief   Debug printf enable (stdout)*/
#ifndef CONFIG_DEBUG_PRINTF
#define CONFIG_DEBUG_PRINTF 1
#endif

/**@brief   Assert printf enable (stdout)*/
#ifndef CONFIG_DEBUG_ASSERT
#define CONFIG_DEBUG_ASSERT 1
#endif

/**@brief   Include assert codes from ext4_debug or standard library.*/
#ifndef CONFIG_HAVE_OWN_ASSERT
#define CONFIG_HAVE_OWN_ASSERT 1
#endif

/**@brief   Statistics of block device*/
#ifndef CONFIG_BLOCK_DEV_ENABLE_STATS
#define CONFIG_BLOCK_DEV_ENABLE_STATS 1
#endif

/**@brief   Cache size of block device.*/
#ifndef CONFIG_BLOCK_DEV_CACHE_SIZE
#define CONFIG_BLOCK_DEV_CACHE_SIZE 8
#endif


/**@brief   Maximum block device name*/
#ifndef CONFIG_EXT4_MAX_BLOCKDEV_NAME
#define CONFIG_EXT4_MAX_BLOCKDEV_NAME 32
#endif


/**@brief   Maximum block device count*/
#ifndef CONFIG_EXT4_BLOCKDEVS_COUNT
#define CONFIG_EXT4_BLOCKDEVS_COUNT 2
#endif

/**@brief   Maximum mountpoint name*/
#ifndef CONFIG_EXT4_MAX_MP_NAME
#define CONFIG_EXT4_MAX_MP_NAME 32
#endif

/**@brief   Maximum mountpoint count*/
#ifndef CONFIG_EXT4_MOUNTPOINTS_COUNT
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 2
#endif


/**@brief Maximum single truncate size. Transactions must be limited to reduce
 *        number of allocetions for single transaction*/
#ifndef CONFIG_MAX_TRUNCATE_SIZE
#define CONFIG_MAX_TRUNCATE_SIZE (16ul * 1024ul * 1024ul)
#endif


/**@brief Unaligned access switch on/off*/
#ifndef CONFIG_UNALIGNED_ACCESS
#define CONFIG_UNALIGNED_ACCESS 0
#endif


#endif /* EXT4_CONFIG_H_ */
