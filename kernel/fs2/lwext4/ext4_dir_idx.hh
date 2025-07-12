


/**
 * @file  ext4_dir_idx.h
 * @brief Directory indexing procedures.
 */

#ifndef EXT4_DIR_IDX_H_
#define EXT4_DIR_IDX_H_


#include <fs2/lwext4/ext4_config.hh>
#include <fs2/lwext4/ext4_types.hh>

#include <fs2/lwext4/ext4_dir.hh>
#include <fs2/lwext4/ext4_fs.hh>

#include "types.hh"

struct ext4_dir_idx_block {
    struct ext4_block b;
    struct ext4_dir_idx_entry *entries;
    struct ext4_dir_idx_entry *position;
};

#define EXT4_DIR_DX_INIT_BCNT 2


/**@brief Initialize index structure of new directory.
 * @param dir Pointer to directory i-node
 * @param parent Pointer to parent directory i-node
 * @return Error code
 */
int ext4_dir_dx_init(struct ext4_inode_ref *dir, struct ext4_inode_ref *parent);

/**@brief Try to find directory entry using directory index.
 * @param result    Output value - if entry will be found,
 *                  than will be passed through this parameter
 * @param inode_ref Directory i-node
 * @param name_len  Length of name to be found
 * @param name      Name to be found
 * @return Error code
 */
int ext4_dir_dx_find_entry(struct ext4_dir_search_result *result, struct ext4_inode_ref *inode_ref, size_t name_len,
                           const char *name);

/**@brief Add new entry to indexed directory
 * @param parent Directory i-node
 * @param child  I-node to be referenced from directory entry
 * @param name   Name of new directory entry
 * @return Error code
 */
int ext4_dir_dx_add_entry(struct ext4_inode_ref *parent, struct ext4_inode_ref *child, const char *name,
                          uint32_t name_len);

/**@brief Add new entry to indexed directory
 * @param dir           Directory i-node
 * @param parent_inode  parent inode index
 * @return Error code
 */
int ext4_dir_dx_reset_parent_inode(struct ext4_inode_ref *dir, uint32_t parent_inode);


#endif /* EXT4_DIR_IDX_H_ */

/**
 * @}
 */
