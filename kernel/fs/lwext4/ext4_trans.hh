


/**
 * @file  ext4_trans.hh
 * @brief Transaction handle functions
 */

#ifndef EXT4_TRANS_H
#define EXT4_TRANS_H


#include <fs/lwext4/ext4_config.hh>
#include <fs/lwext4/ext4_types.hh>


/**@brief   Mark a buffer dirty and add it to the current transaction.
 * @param   buf buffer
 * @return  standard error code*/
int ext4_trans_set_block_dirty(struct ext4_buf *buf);

/**@brief   Block get function (through cache, don't read).
 *          jbd_trans_get_access would be called in order to
 *          get write access to the buffer.
 * @param   bdev block device descriptor
 * @param   b block descriptor
 * @param   lba logical block address
 * @return  standard error code*/
int ext4_trans_block_get_noread(struct ext4_blockdev *bdev, struct ext4_block *b, uint64_t lba);

/**@brief   Block get function (through cache).
 *          jbd_trans_get_access would be called in order to
 *          get write access to the buffer.
 * @param   bdev block device descriptor
 * @param   b block descriptor
 * @param   lba logical block address
 * @return  standard error code*/
int ext4_trans_block_get(struct ext4_blockdev *bdev, struct ext4_block *b, uint64_t lba);

/**@brief  Try to add block to be revoked to the current transaction.
 * @param  bdev block device descriptor
 * @param  lba logical block address
 * @return standard error code*/
int ext4_trans_try_revoke_block(struct ext4_blockdev *bdev, uint64_t lba);


#endif /* EXT4_TRANS_H */

/**
 * @}
 */
