/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_bcache.c
 * @brief Block cache allocator.
 */

#include <fs/lwext4/ext4_bcache.hh>
#include <fs/lwext4/ext4_blockdev.hh>
#include <fs/lwext4/ext4_config.hh>

#include <fs/lwext4/ext4_errno.hh>
#include <fs/lwext4/ext4_types.hh>

#include <stdlib.h>
#include "libs/string.hh"
#include "devs/spinlock.hh"
#include "mem/physical_memory_manager.hh"

namespace
{
    /*
     * ext4_buf 是固定大小的小对象，但 lwext4 的 ext4_calloc/ext4_free 在
     * F7LY 中按整张 4 KiB 页分配。较深块缓存若继续“一描述符一页”，会让
     * 8192 个缓存项额外浪费约 31 MiB，并在回收时反复争用 buddy 锁。
     *
     * 数据页仍由原有 ext4_malloc 管理；描述符页切成等长槽，释放后回收到
     * 池内。池只保留达到过的高水位，不改变 ext4_buf 的所有权或 bcache LRU。
    */
    SpinLock g_ext4_buf_pool_lock;
    eastl::atomic<uint32> g_ext4_buf_pool_state{0};
    ext4_buf *g_ext4_buf_free_list = nullptr;

    void ext4_buf_pool_init()
    {
        constexpr uint32 k_uninitialized = 0;
        constexpr uint32 k_initializing = 1;
        constexpr uint32 k_ready = 2;

        uint32 state = g_ext4_buf_pool_state.load(eastl::memory_order_acquire);
        if (state == k_ready)
        {
            return;
        }

        uint32 expected = k_uninitialized;
        if (g_ext4_buf_pool_state.compare_exchange_strong(
                expected, k_initializing, eastl::memory_order_acq_rel))
        {
            g_ext4_buf_pool_lock.init("ext4_buf_pool");
            g_ext4_buf_pool_state.store(k_ready, eastl::memory_order_release);
            return;
        }

        // 首次挂载通常只有一个调用者；仍把初始化协议做成 SMP 安全，避免
        // 未来并发挂载/mkfs 在锁对象尚未就绪时进入描述符分配。
        while (g_ext4_buf_pool_state.load(eastl::memory_order_acquire) != k_ready)
        {
            asm volatile("nop");
        }
    }

    ext4_buf *ext4_buf_pool_alloc()
    {
        ext4_buf_pool_init();
        g_ext4_buf_pool_lock.acquire();
        if (g_ext4_buf_free_list == nullptr)
        {
            void *page = mem::k_pmm.try_alloc_page_uninitialized();
            if (page == nullptr)
            {
                g_ext4_buf_pool_lock.release();
                return nullptr;
            }

            constexpr size_t slots_per_page = PGSIZE / sizeof(ext4_buf);
            static_assert(slots_per_page > 0);
            static_assert(PGSIZE % alignof(ext4_buf) == 0);
            auto *slots = static_cast<ext4_buf *>(page);
            for (size_t index = 0; index < slots_per_page; ++index)
            {
                // 空闲槽复用 data 保存链指针；取出后会整体清零。
                slots[index].data =
                    reinterpret_cast<uint8_t *>(g_ext4_buf_free_list);
                g_ext4_buf_free_list = &slots[index];
            }
        }

        ext4_buf *buf = g_ext4_buf_free_list;
        g_ext4_buf_free_list = reinterpret_cast<ext4_buf *>(buf->data);
        g_ext4_buf_pool_lock.release();
        memset(buf, 0, sizeof(*buf));
        return buf;
    }

    void ext4_buf_pool_free(ext4_buf *buf)
    {
        if (buf == nullptr)
        {
            return;
        }
        g_ext4_buf_pool_lock.acquire();
        buf->data = reinterpret_cast<uint8_t *>(g_ext4_buf_free_list);
        g_ext4_buf_free_list = buf;
        g_ext4_buf_pool_lock.release();
    }
}

int ext4_bcache_lba_compare(struct ext4_buf *a, struct ext4_buf *b) {
    if (a->lba > b->lba)
        return 1;
    else if (a->lba < b->lba)
        return -1;
    return 0;
}

//原本是INTERNAL的函数，因为c语言作用域问题，此处改成了全局的。
RB_GENERATE(ext4_buf_lba_tree, ext4_buf, lba_node, ext4_bcache_lba_compare)

int ext4_bcache_init_dynamic(struct ext4_bcache *bc, uint32_t cnt, uint32_t itemsize) {
    ext4_assert(bc && cnt && itemsize);

    memset(bc, 0, sizeof(struct ext4_bcache));
    RB_INIT(&bc->lba_root);
    TAILQ_INIT(&bc->lru_list);
    TAILQ_INIT(&bc->dirty_list);

    bc->cnt = cnt;
    bc->itemsize = itemsize;
    bc->ref_blocks = 0;
    bc->max_ref_blocks = 0;

    return EOK;
}

int ext4_bcache_cleanup(struct ext4_bcache *bc) {
    struct ext4_buf *buf, *tmp;
    RB_FOREACH_SAFE(buf, ext4_buf_lba_tree, &bc->lba_root, tmp) {
        int r = ext4_block_flush_buf(bc->bdev, buf);
        if (r != EOK)
            return r;
        ext4_bcache_drop_buf(bc, buf);
    }
    return EOK;
}

int ext4_bcache_fini_dynamic(struct ext4_bcache *bc) {
    memset(bc, 0, sizeof(struct ext4_bcache));
    return EOK;
}

/**@brief:
 *
 *  This is ext4_bcache, the module handling basic buffer-cache stuff.
 *
 *  Buffers in a bcache are sorted by their LBA and stored in a
 *  RB-Tree(lba_root).
 *
 *  Bcache 还维护一条未引用缓冲区的 LRU 队列：
 *  - 队首是最久未使用的块；
 *  - 队尾是最近刚释放引用的块。
 *
 *  A singly-linked list is used to track those dirty buffers which are
 *  ready to be flushed. (Those buffers which are dirty but also referenced
 *  are not considered ready to be flushed.)
 *
 *  When a buffer is not referenced, it will be stored in both lba_root
 *  and lru_list, while it will only be stored in lba_root when it is
 *  referenced.
 */

static struct ext4_buf *ext4_buf_alloc(struct ext4_bcache *bc, uint64_t lba) {
    void *data;
    struct ext4_buf *buf;
    data = ext4_malloc(bc->itemsize);
    if (!data)
        return NULL;

    buf = ext4_buf_pool_alloc();
    if (!buf) {
        ext4_free(data, bc->itemsize);
        return NULL;
    }

    buf->lba = lba;
    buf->data = (uint8_t *)data;
    buf->bc = bc;
    buf->on_dirty_list = false;
    buf->on_lba_tree = false;
    buf->on_lru_list = false;
    return buf;
}

static void ext4_buf_free(struct ext4_buf *buf) {
    ext4_free(buf->data, buf->bc->itemsize);
    ext4_buf_pool_free(buf);
}

static uint32_t ext4_bcache_hot_slot(uint64_t lba)
{
    // inode table、extent 与目录块常按 4KiB LBA 连续分布；混入高位可减少
    // 相隔固定 stride 的元数据块在小型直映表中互相覆盖。
    return static_cast<uint32_t>((lba ^ (lba >> 12)) &
                                 (EXT4_BCACHE_HOT_SLOT_COUNT - 1));
}

static struct ext4_buf *ext4_buf_lookup(struct ext4_bcache *bc, uint64_t lba) {
    const uint32_t hot_slot = ext4_bcache_hot_slot(lba);
    struct ext4_buf *buf = bc->hot_slots[hot_slot];
    if (buf != nullptr && buf->lba == lba)
        return buf;

    struct ext4_buf tmp = {.lba = lba};
    buf = RB_FIND(ext4_buf_lba_tree, &bc->lba_root, &tmp);
    if (buf != nullptr)
        bc->hot_slots[hot_slot] = buf;

    return buf;
}

struct ext4_buf *ext4_buf_lowest_lru(struct ext4_bcache *bc) { return TAILQ_FIRST(&bc->lru_list); }

void ext4_bcache_insert_dirty_node(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    ext4_assert(bc && buf && buf->bc == bc);
    ext4_assert(buf->refctr == 0);
    ext4_assert(ext4_bcache_test_flag(buf, BC_DIRTY));
    ext4_assert(ext4_bcache_test_flag(buf, BC_UPTODATE));
    ext4_assert(!buf->on_dirty_list);

    TAILQ_INSERT_TAIL(&bc->dirty_list, buf, dirty_node);
    buf->on_dirty_list = true;
}

void ext4_bcache_remove_dirty_node(struct ext4_bcache *bc, struct ext4_buf *buf)
{
    ext4_assert(bc && buf && buf->bc == bc);
    ext4_assert(buf->on_dirty_list);
    ext4_assert(buf->dirty_node.tqe_prev != nullptr);
    ext4_assert(*buf->dirty_node.tqe_prev == buf);
    if (buf->dirty_node.tqe_next != nullptr)
    {
        ext4_assert(buf->dirty_node.tqe_next->dirty_node.tqe_prev ==
                    &buf->dirty_node.tqe_next);
    }

    TAILQ_REMOVE(&bc->dirty_list, buf, dirty_node);
    buf->dirty_node.tqe_next = nullptr;
    buf->dirty_node.tqe_prev = nullptr;
    buf->on_dirty_list = false;
}

void ext4_bcache_drop_buf(struct ext4_bcache *bc, struct ext4_buf *buf) {
    ext4_assert(bc && buf && buf->bc == bc);
    ext4_assert(buf->refctr == 0);
    ext4_assert(buf->on_lba_tree);
    ext4_assert(bc->ref_blocks > 0);

    if (buf->on_lru_list) {
        ext4_assert(buf->lru_link.tqe_prev != nullptr);
        ext4_assert(*buf->lru_link.tqe_prev == buf);
        TAILQ_REMOVE(&bc->lru_list, buf, lru_link);
        buf->lru_link.tqe_next = nullptr;
        buf->lru_link.tqe_prev = nullptr;
        buf->on_lru_list = false;
    }

    if (buf->on_lba_tree) {
        RB_REMOVE(ext4_buf_lba_tree, &bc->lba_root, buf);
        buf->on_lba_tree = false;
    }
    const uint32_t hot_slot = ext4_bcache_hot_slot(buf->lba);
    if (bc->hot_slots[hot_slot] == buf)
        bc->hot_slots[hot_slot] = nullptr;

    /*Forcibly drop dirty buffer.*/
    if (buf->on_dirty_list)
        ext4_bcache_remove_dirty_node(bc, buf);

    ext4_buf_free(buf);
    bc->ref_blocks--;
}

void ext4_bcache_invalidate_buf(struct ext4_bcache *bc, struct ext4_buf *buf) {
    buf->end_write = NULL;
    buf->end_write_arg = NULL;

    /* Clear both dirty and up-to-date flags. */
    if (buf->on_dirty_list)
        ext4_bcache_remove_dirty_node(bc, buf);

    ext4_bcache_clear_dirty(buf);
}

void ext4_bcache_invalidate_lba(struct ext4_bcache *bc, uint64_t from, uint32_t cnt) {
    if (cnt == 0)
        return;
    uint64_t end = from + cnt - 1;
    if (end < from)
        end = UINT64_MAX;
    struct ext4_buf key = {.lba = from};
    struct ext4_buf *tmp = RB_NFIND(ext4_buf_lba_tree, &bc->lba_root, &key), *buf;
    RB_FOREACH_FROM(buf, ext4_buf_lba_tree, tmp) {
        if (buf->lba > end)
            break;

        ext4_bcache_invalidate_buf(bc, buf);
    }
}

struct ext4_buf *ext4_bcache_find_get(struct ext4_bcache *bc, struct ext4_block *b, uint64_t lba) {
    struct ext4_buf *buf = ext4_buf_lookup(bc, lba);
    if (buf) {
        ext4_assert(buf->bc == bc);
        ext4_assert(buf->on_lba_tree);
        /* If buffer is not referenced. */
        if (!buf->refctr) {
            ext4_assert(buf->on_lru_list);
            if (buf->on_lru_list) {
                ext4_assert(buf->lru_link.tqe_prev != nullptr);
                ext4_assert(*buf->lru_link.tqe_prev == buf);
                TAILQ_REMOVE(&bc->lru_list, buf, lru_link);
                buf->lru_link.tqe_next = nullptr;
                buf->lru_link.tqe_prev = nullptr;
                buf->on_lru_list = false;
            }
            if (buf->on_dirty_list)
                ext4_bcache_remove_dirty_node(bc, buf);
        } else {
            ext4_assert(!buf->on_lru_list);
            ext4_assert(!buf->on_dirty_list);
        }

        ext4_bcache_inc_ref(buf);

        b->lb_id = lba;
        b->buf = buf;
        b->data = buf->data;
    }
    return buf;
}

int ext4_bcache_alloc(struct ext4_bcache *bc, struct ext4_block *b, bool *is_new) {
    /* Try to search the buffer with exaxt LBA. */
    struct ext4_buf *buf = ext4_bcache_find_get(bc, b, b->lb_id);
    if (buf) {
        *is_new = false;
        return EOK;
    }

    /* We need to allocate one buffer.*/
    buf = ext4_buf_alloc(bc, b->lb_id);
    if (!buf)
        return ENOMEM;

    RB_INSERT(ext4_buf_lba_tree, &bc->lba_root, buf);
    buf->on_lba_tree = true;
    bc->hot_slots[ext4_bcache_hot_slot(buf->lba)] = buf;
    /* One more buffer in bcache now. :-) */
    bc->ref_blocks++;

    /*Calc ref blocks max depth*/
    if (bc->max_ref_blocks < bc->ref_blocks)
        bc->max_ref_blocks = bc->ref_blocks;


    ext4_bcache_inc_ref(buf);

    b->buf = buf;
    b->data = buf->data;

    *is_new = true;
    return EOK;
}

int ext4_bcache_free(struct ext4_bcache *bc, struct ext4_block *b) {
    struct ext4_buf *buf = b->buf;

    ext4_assert(bc && b);

    /*Check if valid.*/
    // ext4_assert(b->lb_id);

    if (!(b->lb_id)) {
        return EOK;
    }

    /*Block should have a valid pointer to ext4_buf.*/
    ext4_assert(buf);
    ext4_assert(buf->bc == bc);
    ext4_assert(buf->lba == b->lb_id);

    // 重复释放会直接破坏引用计数；必须在第一现场失败，不能静默吞掉。
    ext4_assert(buf->refctr > 0);

    /*Just decrease reference counter*/
    ext4_bcache_dec_ref(buf);

    /* We are the last one touching this buffer, do the cleanups. */
    if (!buf->refctr) {
        /*
         * 释放到 0 引用时再挂回 LRU 队尾，语义上更贴近“最近一次真实使用”
         * 的完成时刻，也避免维护第二棵红黑树带来的复杂性。
         */
        if (!buf->on_lru_list) {
            TAILQ_INSERT_TAIL(&bc->lru_list, buf, lru_link);
            buf->on_lru_list = true;
        }
        /* This buffer is ready to be flushed. */
        if (ext4_bcache_test_flag(buf, BC_DIRTY) && ext4_bcache_test_flag(buf, BC_UPTODATE)) {
            if (bc->bdev->cache_write_back && !ext4_bcache_test_flag(buf, BC_FLUSH) &&
                !ext4_bcache_test_flag(buf, BC_TMP))
                ext4_bcache_insert_dirty_node(bc, buf);
            else {
                ext4_block_flush_buf(bc->bdev, buf);
                ext4_bcache_clear_flag(buf, BC_FLUSH);
            }
        }

        /* The buffer is invalidated...drop it. */
        if (!ext4_bcache_test_flag(buf, BC_UPTODATE) || ext4_bcache_test_flag(buf, BC_TMP))
            ext4_bcache_drop_buf(bc, buf);
    }

    b->lb_id = 0;
    b->buf = nullptr;
    b->data = nullptr;

    return EOK;
}

bool ext4_bcache_is_full(struct ext4_bcache *bc) { return (bc->cnt <= bc->ref_blocks); }


/**
 * @}
 */
