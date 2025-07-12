
/**
 * @file  ext4_bitmap.hh
 * @brief Block/inode bitmap allocator.
 */

#ifndef EXT4_BITMAP_H_
#define EXT4_BITMAP_H_


#include <fs/lwext4/ext4_config.hh>

#include "types.hh"

/**@brief   Set bitmap bit.
 * @param   bmap bitmap
 * @param   bit bit to set*/
static inline void ext4_bmap_bit_set(uint8_t *bmap, uint32_t bit) { *(bmap + (bit >> 3)) |= (1 << (bit & 7)); }

/**@brief   Clear bitmap bit.
 * @param   bmap bitmap buffer
 * @param   bit bit to clear*/
static inline void ext4_bmap_bit_clr(uint8_t *bmap, uint32_t bit) { *(bmap + (bit >> 3)) &= ~(1 << (bit & 7)); }

/**@brief   Check if the bitmap bit is set.
 * @param   bmap bitmap buffer
 * @param   bit bit to check*/
static inline bool ext4_bmap_is_bit_set(uint8_t *bmap, uint32_t bit) {
    return (*(bmap + (bit >> 3)) & (1 << (bit & 7)));
}

/**@brief   Check if the bitmap bit is clear.
 * @param   bmap bitmap buffer
 * @param   bit bit to check*/
static inline bool ext4_bmap_is_bit_clr(uint8_t *bmap, uint32_t bit) { return !ext4_bmap_is_bit_set(bmap, bit); }

/**@brief   Free range of bits in bitmap.
 * @param   bmap bitmap buffer
 * @param   sbit start bit
 * @param   bcnt bit count*/
void ext4_bmap_bits_free(uint8_t *bmap, uint32_t sbit, uint32_t bcnt);

/**@brief   Find first clear bit in bitmap.
 * @param   sbit start bit of search
 * @param   ebit end bit of search
 * @param   bit_id output parameter (first free bit)
 * @return  standard error code*/
int ext4_bmap_bit_find_clr(uint8_t *bmap, uint32_t sbit, uint32_t ebit, uint32_t *bit_id);


#endif /* EXT4_BITMAP_H_ */

/**
 * @}
 */
