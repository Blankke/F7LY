#pragma once
/**
 * @file  ext4_journal.h
 * @brief Journal handle functions
 */

#ifndef EXT4_JOURNAL_H_
#define EXT4_JOURNAL_H_


#include <fs/lwext4/ext4_config.hh>
#include <fs/lwext4/ext4_types.hh>
#include <fs/lwext4/misc/queue.hh>
#include <fs/lwext4/misc/tree.hh>

// Forward declarations for global RB tree types
RB_HEAD(jbd_revoke_tree_global, jbd_revoke_rec);
RB_HEAD(jbd_block_tree_global, jbd_block_rec);
RB_HEAD(jbd_revoke_tree_recover, revoke_entry);

// Forward declarations for global TAILQ types  
TAILQ_HEAD(jbd_buf_dirty_global, jbd_buf);
TAILQ_HEAD(jbd_trans_buf_global, jbd_buf);
TAILQ_HEAD(jbd_cp_queue_global, jbd_trans);

// Forward declarations for comparison functions
int jbd_revoke_rec_cmp(struct jbd_revoke_rec *a, struct jbd_revoke_rec *b);
int jbd_block_rec_cmp(struct jbd_block_rec *a, struct jbd_block_rec *b);
int jbd_revoke_entry_cmp(struct revoke_entry *a, struct revoke_entry *b);

// RB tree function prototypes
RB_PROTOTYPE(jbd_revoke_tree_global, jbd_revoke_rec, revoke_node, jbd_revoke_rec_cmp)
RB_PROTOTYPE(jbd_block_tree_global, jbd_block_rec, block_rec_node, jbd_block_rec_cmp)
RB_PROTOTYPE(jbd_revoke_tree_recover, revoke_entry, revoke_node, jbd_revoke_entry_cmp)

#include "fs/lwext4/ext4_fs.hh"

struct jbd_fs {
    struct ext4_blockdev *bdev;
    struct ext4_inode_ref inode_ref;
    struct jbd_sb sb;

    bool dirty;
};

struct jbd_buf {
    uint32_t jbd_lba;
    struct ext4_block block;
    struct jbd_trans *trans;
    struct jbd_block_rec *block_rec;
    TAILQ_ENTRY(jbd_buf) buf_node;
    TAILQ_ENTRY(jbd_buf) dirty_buf_node;
};

struct jbd_revoke_rec {
    ext4_fsblk_t lba;
    RB_ENTRY(jbd_revoke_rec) revoke_node;
};

struct jbd_block_rec {
    ext4_fsblk_t lba;
    struct jbd_trans *trans;
    RB_ENTRY(jbd_block_rec) block_rec_node;
    LIST_ENTRY(jbd_block_rec) tbrec_node;
    struct jbd_buf_dirty_global dirty_buf_queue;
};

struct jbd_trans {
    uint32_t trans_id;

    uint32_t start_iblock;
    int alloc_blocks;
    int data_cnt;
    uint32_t data_csum;
    int written_cnt;
    int error;

    struct jbd_journal *journal;

    struct jbd_trans_buf_global buf_queue;
    struct jbd_revoke_tree_global revoke_root;
    LIST_HEAD(jbd_trans_block_rec, jbd_block_rec) tbrec_list;
    TAILQ_ENTRY(jbd_trans) trans_node;
};

struct jbd_journal {
    uint32_t first;
    uint32_t start;
    uint32_t last;
    uint32_t trans_id;
    uint32_t alloc_trans_id;

    uint32_t block_size;

    struct jbd_cp_queue_global cp_queue;
    struct jbd_block_tree_global block_rec_root;

    struct jbd_fs *jbd_fs;
};

int jbd_get_fs(struct ext4_fs *fs, struct jbd_fs *jbd_fs);
int jbd_put_fs(struct jbd_fs *jbd_fs);
int jbd_inode_bmap(struct jbd_fs *jbd_fs, ext4_lblk_t iblock, ext4_fsblk_t *fblock);
int jbd_recover(struct jbd_fs *jbd_fs);
int jbd_journal_start(struct jbd_fs *jbd_fs, struct jbd_journal *journal);
int jbd_journal_stop(struct jbd_journal *journal);
struct jbd_trans *jbd_journal_new_trans(struct jbd_journal *journal);
int jbd_trans_set_block_dirty(struct jbd_trans *trans, struct ext4_block *block);
int jbd_trans_revoke_block(struct jbd_trans *trans, ext4_fsblk_t lba);
int jbd_trans_try_revoke_block(struct jbd_trans *trans, ext4_fsblk_t lba);
void jbd_journal_free_trans(struct jbd_journal *journal, struct jbd_trans *trans, bool abort);
int jbd_journal_commit_trans(struct jbd_journal *journal, struct jbd_trans *trans);
void jbd_journal_purge_cp_trans(struct jbd_journal *journal, bool flush, bool once);


#endif /* EXT4_JOURNAL_H_ */

/**
 * @}
 */
