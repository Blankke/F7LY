


/**
 * @file  ext4_xattr.h
 * @brief Extended Attribute manipulation.
 */

#ifndef EXT4_XATTR_H_
#define EXT4_XATTR_H_


#include <fs/lwext4/ext4_config.hh>
#include <fs/lwext4/ext4_inode.hh>
#include <fs/lwext4/ext4_types.hh>

struct ext4_xattr_info {
    uint8_t name_index;
    const char *name;
    size_t name_len;
    const void *value;
    size_t value_len;
};

struct ext4_xattr_list_entry {
    uint8_t name_index;
    char *name;
    size_t name_len;
    struct ext4_xattr_list_entry *next;
};

struct ext4_xattr_search {
    /* The first entry in the buffer */
    struct ext4_xattr_entry *first;

    /* The address of the buffer */
    void *base;

    /* The first inaccessible address */
    void *end;

    /* The current entry pointer */
    struct ext4_xattr_entry *here;

    /* Entry not found */
    bool not_found;
};

const char *ext4_extract_xattr_name(const char *full_name, size_t full_name_len, uint8_t *name_index, size_t *name_len,
                                    bool *found);

const char *ext4_get_xattr_name_prefix(uint8_t name_index, size_t *ret_prefix_len);

int ext4_xattr_list(struct ext4_inode_ref *inode_ref, struct ext4_xattr_list_entry *list, size_t *list_len);

int ext4_xattr_get(struct ext4_inode_ref *inode_ref, uint8_t name_index, const char *name, size_t name_len, void *buf,
                   size_t buf_len, size_t *data_len);

int ext4_xattr_remove(struct ext4_inode_ref *inode_ref, uint8_t name_index, const char *name, size_t name_len);

int ext4_xattr_set(struct ext4_inode_ref *inode_ref, uint8_t name_index, const char *name, size_t name_len,
                   const void *value, size_t value_len);


#endif
/**
 * @}
 */
