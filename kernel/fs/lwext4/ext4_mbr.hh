


#pragma once
/**
 * @file  ext4_mbr.h
 * @brief Master boot record parser
 */

#ifndef EXT4_MBR_H_
#define EXT4_MBR_H_


#include <fs/lwext4/ext4_blockdev.hh>
#include <fs/lwext4/ext4_config.hh>

/**@brief Master boot record block devices descriptor*/
struct ext4_mbr_bdevs {
    struct ext4_blockdev partitions[4];
};

int ext4_mbr_scan(struct ext4_blockdev *parent, struct ext4_mbr_bdevs *bdevs);

/**@brief Master boot record partitions*/
struct ext4_mbr_parts {

    /**@brief Percentage division tab:
     *  - {50, 20, 10, 20}
     * Sum of all 4 elements must be <= 100*/
    uint8_t division[4];
};

int ext4_mbr_write(struct ext4_blockdev *parent, struct ext4_mbr_parts *parts, uint32_t disk_id);


#endif /* EXT4_MBR_H_ */

/**
 * @}
 */
