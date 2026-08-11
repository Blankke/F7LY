#include "vma_metadata_utils.hh"

#include "physical_memory_manager.hh"
#include "hal/arch.hh"
#include "vm_object.hh"
#include "fs/vfs/file/file.hh"
#include "shm/shm_manager.hh"

namespace proc::vma_meta
{
    namespace
    {
        inline void *page_pa_to_kernel_ptr(uint64 pa)
        {
#ifdef LOONGARCH
            return reinterpret_cast<void *>(to_vir(pa));
#else
            return reinterpret_cast<void *>(pa);
#endif
        }

        inline uint64 page_count_for_vma(const vma &entry)
        {
            if (!entry.valid_range())
            {
                return 0;
            }
            return PGROUNDUP(static_cast<uint64>(entry.len)) / PGSIZE;
        }

        inline bool page_index_in_range(uint64 page_index, uint64 start_page, uint64 page_count)
        {
            return page_index >= start_page && page_index < (start_page + page_count);
        }
    }

    bool clone_snapshot(vma &dst, const vma &src)
    {
        dst = src;
        dst.private_page_overlay = clone_overlay_subset(src, 0, page_count_for_vma(src), true);

        if (src.private_page_overlay != nullptr &&
            !src.private_page_overlay->empty() &&
            dst.private_page_overlay == nullptr)
        {
            return false;
        }

        if (dst.object != nullptr)
        {
            dst.object->get();
        }
        if (dst.vfile != nullptr)
        {
            dst.vfile->dup();
        }
        return true;
    }

    void release_metadata(vma &entry)
    {
        if (entry.vfile != nullptr)
        {
            entry.vfile->free_file();
            entry.vfile = nullptr;
        }

        if (entry.object != nullptr)
        {
            entry.object->on_area_destroy(entry);
            shm::k_smm.release_shared_file_object_if_unused(entry.object);
            if (entry.object->put())
            {
                delete entry.object;
            }
            entry.object = nullptr;
            return;
        }

        if (entry.private_page_overlay != nullptr)
        {
            for (auto &overlay_entry : *entry.private_page_overlay)
            {
                if (overlay_entry.second != 0)
                {
                    mem::k_pmm.free_page(page_pa_to_kernel_ptr(overlay_entry.second));
                }
            }
            delete entry.private_page_overlay;
            entry.private_page_overlay = nullptr;
        }
    }

    VmPrivateOverlayMap *clone_overlay_subset(const vma &src,
                                              uint64 start_page,
                                              uint64 page_count,
                                              bool retain_pages)
    {
        if (src.private_page_overlay == nullptr || page_count == 0)
        {
            return nullptr;
        }

        VmPrivateOverlayMap *subset = new VmPrivateOverlayMap();
        if (subset == nullptr)
        {
            return nullptr;
        }

        for (const auto &overlay_entry : *src.private_page_overlay)
        {
            if (!page_index_in_range(overlay_entry.first, start_page, page_count))
            {
                continue;
            }

            if (retain_pages && overlay_entry.second != 0)
            {
                if (!mem::k_pmm.retain_page(page_pa_to_kernel_ptr(overlay_entry.second)))
                {
                    for (const auto &retained_entry : *subset)
                    {
                        if (retained_entry.second != 0)
                        {
                            mem::k_pmm.free_page(page_pa_to_kernel_ptr(retained_entry.second));
                        }
                    }
                    delete subset;
                    return nullptr;
                }
            }
            (*subset)[overlay_entry.first - start_page] = overlay_entry.second;
        }

        if (subset->empty())
        {
            delete subset;
            return nullptr;
        }
        return subset;
    }

    void release_overlay_pages_in_range(const vma &src,
                                        uint64 start_page,
                                        uint64 page_count)
    {
        if (src.private_page_overlay == nullptr || page_count == 0)
        {
            return;
        }

        for (const auto &overlay_entry : *src.private_page_overlay)
        {
            if (!page_index_in_range(overlay_entry.first, start_page, page_count))
            {
                continue;
            }

            if (overlay_entry.second != 0)
            {
                mem::k_pmm.free_page(page_pa_to_kernel_ptr(overlay_entry.second));
            }
        }
    }

    void discard_overlay_container(vma &entry)
    {
        if (entry.private_page_overlay != nullptr)
        {
            delete entry.private_page_overlay;
            entry.private_page_overlay = nullptr;
        }
    }
}
