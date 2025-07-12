

/**
 * @file  ext4_hash.h
 * @brief Directory indexing hash functions.
 */

#ifndef EXT4_HASH_H_
#define EXT4_HASH_H_


#include <fs2/lwext4/ext4_config.hh>

#include "types.hh"

struct ext4_hash_info {
    uint32_t hash;
    uint32_t minor_hash;
    uint32_t hash_version;
    const uint32_t *seed;
};


/**@brief   Directory entry name hash function.
 * @param   name entry name
 * @param   len entry name length
 * @param   hash_seed (from superblock)
 * @param   hash_version version (from superblock)
 * @param   hash_minor output value
 * @param   hash_major output value
 * @return  standard error code*/
int ext2_htree_hash(const char *name, int len, const uint32_t *hash_seed, int hash_version, uint32_t *hash_major,
                    uint32_t *hash_minor);


#endif /* EXT4_HASH_H_ */

/**
 * @}
 */
