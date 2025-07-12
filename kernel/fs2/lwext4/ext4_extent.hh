


/**
 * @file  ext4_extent.h
 * @brief More complex filesystem functions.
 */
#ifndef EXT4_EXTENT_H_
#define EXT4_EXTENT_H_


#include <fs2/lwext4/ext4_config.hh>
#include <fs2/lwext4/ext4_inode.hh>
#include <fs2/lwext4/ext4_types.hh>

void ext4_extent_tree_init(struct ext4_inode_ref *inode_ref);


int ext4_extent_get_blocks(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock, uint32_t max_blocks,
                           ext4_fsblk_t *result, bool create, uint32_t *blocks_count);


/**@brief Release all data blocks starting from specified logical block.
 * @param inode_ref   I-node to release blocks from
 * @param iblock_from First logical block to release
 * @return Error code */
int ext4_extent_remove_space(struct ext4_inode_ref *inode_ref, ext4_lblk_t from, ext4_lblk_t to);


#endif /* EXT4_EXTENT_H_ */
/**
 * @}
 */
