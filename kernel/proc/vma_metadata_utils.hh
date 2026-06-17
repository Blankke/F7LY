#pragma once

#include "vm_area.hh"

namespace proc::vma_meta
{
    // 为 rollback / fork 构造一份独立的 VMA 元数据快照。
    bool clone_snapshot(vma &dst, const vma &src);

    // 只释放 VMA 元数据持有的引用与 overlay 页，不碰页表。
    void release_metadata(vma &entry);

    // 拷贝某个页区间对应的 overlay 子集。
    // retain_pages=true 用于 fork/快照，多一份 owner 引用；
    // retain_pages=false 用于同一地址空间内的 split/trim，直接转移 owner。
    VmPrivateOverlayMap *clone_overlay_subset(const vma &src,
                                              uint64 start_page,
                                              uint64 page_count,
                                              bool retain_pages);

    // 释放 overlay 中指定页区间持有的 owner 引用。
    void release_overlay_pages_in_range(const vma &src,
                                        uint64 start_page,
                                        uint64 page_count);

    // 仅销毁 overlay 容器本身，不释放其中页面。
    void discard_overlay_container(vma &entry);
}
