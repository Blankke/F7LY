#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_oflags.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/device_file.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "fs/vfs/file/directory_file.hh"
#include "fs/vfs/file/fat32_file.hh"
#include "fs/fat32/fat32.hh"
#include "fs/vfs/fifo_manager.hh"
#include "fs/vfs/virtual_fs.hh"
#include "proc/capability.hh"
#include "proc_manager.hh" // 用于访问当前进程的umask
#include "fs/lwext4/ext4.hh"
#include "fs/vfs/vfs_ext4_ext.hh" // 包含 NS_to_S 宏
#include "tm/time.h"              // 包含 TIME2NS 宏
#include "mem/memlayout.hh"
#include "physical_memory_manager.hh"
#include <EASTL/vector.h>
#include <EASTL/algorithm.h>
#include <libs/string.hh>

namespace
{
#ifdef RISCV
    constexpr uint64 k_min_kernel_file_ptr = KERNBASE;
#elif defined(LOONGARCH)
    constexpr uint64 k_min_kernel_file_ptr = PHYSBASE;
#endif
    constexpr size_t k_linux_path_max = 4096;
    constexpr size_t k_linux_name_max = 255;

    struct MountOverride
    {
        uint64 mount_id = 0;
        uint64 parent_mount_id = 0;
        eastl::string path;
        eastl::string source;
        bool read_only = false;
        VfsMountPropagation propagation = VFS_MOUNT_PRIVATE;
        int peer_group = 0;
        int master_peer_group = 0;

        MountOverride() = default;
        MountOverride(const MountOverride &other)
            : mount_id(other.mount_id),
              parent_mount_id(other.parent_mount_id),
              path(other.path),
              source(other.source),
              read_only(other.read_only),
              propagation(other.propagation),
              peer_group(other.peer_group),
              master_peer_group(other.master_peer_group)
        {
        }

        MountOverride &operator=(const MountOverride &other)
        {
            if (this == &other)
            {
                return *this;
            }
            mount_id = other.mount_id;
            parent_mount_id = other.parent_mount_id;
            path = other.path;
            source = other.source;
            read_only = other.read_only;
            propagation = other.propagation;
            peer_group = other.peer_group;
            master_peer_group = other.master_peer_group;
            return *this;
        }
    };

    struct PropagationReceiver
    {
        uint64 ns_id = proc::k_initial_mount_namespace_id;
        uint64 mount_id = 0;
        size_t mount_index = 0;
        eastl::string path;
        eastl::string source;
        bool read_only = false;
        VfsMountPropagation propagation = VFS_MOUNT_PRIVATE;
        int peer_group = 0;
        int master_peer_group = 0;
        bool via_slave = false;

        PropagationReceiver() = default;
        PropagationReceiver(const PropagationReceiver &other)
            : ns_id(other.ns_id),
              mount_id(other.mount_id),
              mount_index(other.mount_index),
              path(other.path),
              source(other.source),
              read_only(other.read_only),
              propagation(other.propagation),
              peer_group(other.peer_group),
              master_peer_group(other.master_peer_group),
              via_slave(other.via_slave)
        {
        }

        PropagationReceiver &operator=(const PropagationReceiver &other)
        {
            if (this == &other)
            {
                return *this;
            }
            ns_id = other.ns_id;
            mount_id = other.mount_id;
            mount_index = other.mount_index;
            path = other.path;
            source = other.source;
            read_only = other.read_only;
            propagation = other.propagation;
            peer_group = other.peer_group;
            master_peer_group = other.master_peer_group;
            via_slave = other.via_slave;
            return *this;
        }
    };

    struct MountNamespaceState
    {
        uint64 id = proc::k_initial_mount_namespace_id;
        eastl::vector<MountOverride> mounts;
        int next_peer_group = 1;
        int refcnt = 0;

        MountNamespaceState() = default;
        MountNamespaceState(const MountNamespaceState &other)
            : id(other.id),
              mounts(other.mounts),
              next_peer_group(other.next_peer_group),
              refcnt(other.refcnt)
        {
        }

        MountNamespaceState &operator=(const MountNamespaceState &other)
        {
            if (this == &other)
            {
                return *this;
            }
            id = other.id;
            mounts = other.mounts;
            next_peer_group = other.next_peer_group;
            refcnt = other.refcnt;
            return *this;
        }
    };

    struct DirentSnapshot
    {
        eastl::string name;
        unsigned char type = T_UNKNOWN;
    };

    eastl::vector<MountNamespaceState> g_mount_namespaces;
    uint64 g_next_mount_namespace_id = proc::k_initial_mount_namespace_id + 1;
    uint64 g_next_mount_id = 1;
    int g_next_mount_peer_group = 1;

    void resolve_bind_mount_path(const eastl::string &path, eastl::string &resolved_path);
    bool select_effective_backing_path(const eastl::string &requested_path,
                                       eastl::string &selected_path,
                                       bool allow_parent_fallback);

    MountNamespaceState *find_mount_namespace_state(uint64 ns_id)
    {
        for (auto &ns : g_mount_namespaces)
        {
            if (ns.id == ns_id)
            {
                return &ns;
            }
        }
        return nullptr;
    }

    MountNamespaceState &ensure_mount_namespace_state(uint64 ns_id)
    {
        if (MountNamespaceState *existing = find_mount_namespace_state(ns_id))
        {
            return *existing;
        }

        MountNamespaceState ns;
        ns.id = ns_id == 0 ? proc::k_initial_mount_namespace_id : ns_id;
        g_mount_namespaces.push_back(ns);
        return g_mount_namespaces.back();
    }

    uint64 current_mount_namespace_id()
    {
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        if (pcb == nullptr || pcb->_mnt_ns_id == 0)
        {
            return proc::k_initial_mount_namespace_id;
        }
        return pcb->_mnt_ns_id;
    }

    MountNamespaceState &current_mount_namespace_state()
    {
        return ensure_mount_namespace_state(current_mount_namespace_id());
    }

    eastl::vector<MountOverride> &mount_overrides()
    {
        return current_mount_namespace_state().mounts;
    }

    bool dirent_exists(const eastl::vector<DirentSnapshot> &entries, const eastl::string &name)
    {
        return eastl::find_if(entries.begin(), entries.end(),
                              [&](const DirentSnapshot &entry)
                              {
                                  return entry.name == name;
                              }) != entries.end();
    }

    void append_dirent_snapshot(eastl::vector<DirentSnapshot> &entries,
                                const eastl::string &name,
                                unsigned char type)
    {
        if (name.empty() || dirent_exists(entries, name))
        {
            return;
        }
        entries.push_back({name, type});
    }

    int read_symlink_target_for_path(const eastl::string &path, eastl::string &target_path)
    {
        fs::vfile_tree_node *virtual_node = fs::k_vfs.get_virtual_node(path);
        if (virtual_node != nullptr && virtual_node->file_type == fs::FileTypes::FT_SYMLINK)
        {
            if (!virtual_node->provider)
            {
                return -ENOENT;
            }
            target_path = virtual_node->provider->read_symlink_target();
            return target_path.empty() ? -ENOENT : 0;
        }

        char link_target[256];
        size_t link_len = 0;
        eastl::string backing_path;
        resolve_bind_mount_path(path, backing_path);
        int read_ret = ext4_readlink(backing_path.c_str(), link_target, sizeof(link_target) - 1, &link_len);
        if (read_ret != EOK)
        {
            return read_ret == ENOENT ? -ENOENT : -read_ret;
        }

        link_target[link_len] = '\0';
        target_path = link_target;
        return 0;
    }

    int collect_real_directory_entries(const eastl::string &path,
                                       eastl::vector<DirentSnapshot> &entries)
    {
        fs::file *real_dir = nullptr;
        int open_ret = vfs_openat(path, real_dir, O_RDONLY, 0);
        if (open_ret != EOK)
        {
            return open_ret;
        }
        if (real_dir == nullptr)
        {
            return -ENOENT;
        }

        auto cleanup_real_dir = [&]()
        {
            if (real_dir == nullptr)
            {
                return;
            }
            filesystem_t *fs = get_fs_from_path(path.c_str());
            if (fs != nullptr && fs->type == EXT4 && real_dir->_attrs.filetype == fs::FileTypes::FT_DIRECT)
            {
                ext4_dir_close(&real_dir->lwext4_dir_struct);
            }
            real_dir->free_file();
            real_dir = nullptr;
        };

        if (real_dir->is_virtual || real_dir->_attrs.filetype != fs::FileTypes::FT_DIRECT)
        {
            cleanup_real_dir();
            return 0;
        }

        char *kernel_buf = reinterpret_cast<char *>(mem::k_pmm.alloc_page());
        if (kernel_buf == nullptr)
        {
            cleanup_real_dir();
            return -ENOMEM;
        }

        int ret = 0;
        while (true)
        {
            int read_bytes = vfs_getdents(real_dir,
                                          reinterpret_cast<struct linux_dirent64 *>(kernel_buf),
                                          PGSIZE);
            if (read_bytes <= 0)
            {
                ret = read_bytes;
                break;
            }

            size_t offset = 0;
            while (offset < static_cast<size_t>(read_bytes))
            {
                auto *entry = reinterpret_cast<struct linux_dirent64 *>(kernel_buf + offset);
                if (entry->d_reclen < sizeof(struct linux_dirent64) ||
                    offset + entry->d_reclen > static_cast<size_t>(read_bytes))
                {
                    ret = -EIO;
                    goto done_collect_real_dirents;
                }

                if (entry->d_name[0] != '\0' &&
                    strcmp(entry->d_name, ".") != 0 &&
                    strcmp(entry->d_name, "..") != 0)
                {
                    append_dirent_snapshot(entries, entry->d_name, entry->d_type);
                }

                offset += entry->d_reclen;
            }
        }

    done_collect_real_dirents:
        mem::k_pmm.free_page(kernel_buf);
        cleanup_real_dir();
        return ret;
    }

    bool path_matches_mount_prefix(const eastl::string &path, const eastl::string &mount_path)
    {
        if (mount_path.empty())
        {
            return false;
        }

        if (mount_path == "/")
        {
            return !path.empty() && path[0] == '/';
        }

        if (path.compare(0, mount_path.size(), mount_path) != 0)
        {
            return false;
        }

        return path.size() == mount_path.size() || path[mount_path.size()] == '/';
    }

    int find_covering_mount_index_in(const MountNamespaceState &ns,
                                     const eastl::string &path,
                                     size_t limit)
    {
        size_t end = eastl::min(limit, ns.mounts.size());
        for (size_t i = end; i > 0; --i)
        {
            if (path_matches_mount_prefix(path, ns.mounts[i - 1].path))
            {
                return static_cast<int>(i - 1);
            }
        }
        return -1;
    }

    int find_covering_mount_index_in(const MountNamespaceState &ns,
                                     const eastl::string &path)
    {
        return find_covering_mount_index_in(ns, path, ns.mounts.size());
    }

    const MountOverride *find_mount_override(const eastl::string &path)
    {
        MountNamespaceState &ns = current_mount_namespace_state();
        int index = find_covering_mount_index_in(ns, path);
        return index < 0 ? nullptr : &ns.mounts[static_cast<size_t>(index)];
    }

    MountOverride *find_exact_mount_override(const eastl::string &path)
    {
        MountNamespaceState &ns = current_mount_namespace_state();
        int index = find_covering_mount_index_in(ns, path);
        if (index < 0 || ns.mounts[static_cast<size_t>(index)].path != path)
        {
            return nullptr;
        }
        return &ns.mounts[static_cast<size_t>(index)];
    }

    int find_exact_mount_index_in(const MountNamespaceState &ns,
                                  const eastl::string &path)
    {
        int index = find_covering_mount_index_in(ns, path);
        if (index < 0 || ns.mounts[static_cast<size_t>(index)].path != path)
        {
            return -1;
        }
        return index;
    }

    int find_mount_index_by_id(const MountNamespaceState &ns, uint64 mount_id)
    {
        if (mount_id == 0)
        {
            return -1;
        }
        for (size_t i = 0; i < ns.mounts.size(); ++i)
        {
            if (ns.mounts[i].mount_id == mount_id)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int find_exact_mount_index_with_parent(const MountNamespaceState &ns,
                                           const eastl::string &path,
                                           uint64 parent_mount_id)
    {
        for (size_t i = ns.mounts.size(); i > 0; --i)
        {
            const MountOverride &entry = ns.mounts[i - 1];
            if (entry.path == path &&
                entry.parent_mount_id == parent_mount_id)
            {
                return static_cast<int>(i - 1);
            }
        }
        return -1;
    }

    bool is_mount_ancestor_or_self(const MountNamespaceState &ns,
                                   uint64 ancestor_mount_id,
                                   uint64 descendant_mount_id)
    {
        uint64 current_id = descendant_mount_id;
        while (current_id != 0)
        {
            if (current_id == ancestor_mount_id)
            {
                return true;
            }
            int current_index = find_mount_index_by_id(ns, current_id);
            if (current_index < 0)
            {
                return false;
            }
            current_id =
                ns.mounts[static_cast<size_t>(current_index)].parent_mount_id;
        }
        return false;
    }

    bool is_visible_mount_in(const MountNamespaceState &ns, size_t index)
    {
        if (index >= ns.mounts.size())
        {
            return false;
        }
        return find_covering_mount_index_in(ns, ns.mounts[index].path) ==
               static_cast<int>(index);
    }

    int allocate_mount_peer_group()
    {
        return g_next_mount_peer_group++;
    }

    uint64 allocate_mount_id()
    {
        return g_next_mount_id++;
    }

    int ensure_shared_peer_group(MountOverride &entry)
    {
        if (entry.peer_group == 0)
        {
            entry.peer_group = allocate_mount_peer_group();
        }
        return entry.peer_group;
    }

    bool is_shared_mount(const MountOverride &entry)
    {
        return entry.propagation == VFS_MOUNT_SHARED && entry.peer_group != 0;
    }

    void append_mount_suffix(const eastl::string &base,
                             const eastl::string &suffix,
                             eastl::string &result)
    {
        result.clear();
        if (suffix.empty())
        {
            if (base.empty())
            {
                result = "/";
                return;
            }
            result = base;
            return;
        }

        if (base.empty())
        {
            result = "/";
        }
        else
        {
            result = base;
        }
        if (result != "/" && result.back() == '/')
        {
            result.pop_back();
        }

        if (suffix[0] != '/' && result != "/")
        {
            result += "/";
        }
        result += suffix;
        // 调用方传入的挂载根已经规范化，suffix 也来自规范化路径的前缀切分。
        // 这里直接拼接，避免在热路径中制造额外 eastl::string 临时对象。
    }

    void suffix_after_mount_prefix(const eastl::string &path,
                                   const eastl::string &prefix,
                                   eastl::string &suffix)
    {
        suffix.clear();
        if (path.size() <= prefix.size())
        {
            return;
        }
        if (prefix == "/")
        {
            suffix = path.substr(1);
            return;
        }
        suffix = path.substr(prefix.size() + 1);
    }

    int push_mount_override_in(MountNamespaceState &ns,
                               const eastl::string &mount_path,
                               const eastl::string &source_path,
                               bool read_only,
                               VfsMountPropagation propagation,
                               int peer_group,
                               int master_peer_group = 0,
                               uint64 parent_mount_id = 0,
                               uint64 *created_mount_id = nullptr)
    {
        if (mount_path.empty())
        {
            return -EINVAL;
        }

        eastl::string normalized_path = normalize_path(mount_path);
        if (normalized_path.empty())
        {
            normalized_path = "/";
        }

        eastl::string normalized_source;
        if (!source_path.empty())
        {
            normalized_source = normalize_path(source_path);
            if (normalized_source.empty())
            {
                normalized_source = "/";
            }
        }

        MountOverride entry;
        entry.mount_id = allocate_mount_id();
        entry.parent_mount_id = parent_mount_id;
        entry.path = normalized_path;
        entry.source = normalized_source;
        entry.read_only = read_only;
        entry.propagation = propagation;
        entry.peer_group = peer_group;
        entry.master_peer_group = master_peer_group;
        ns.mounts.push_back(entry);
        if (created_mount_id != nullptr)
        {
            *created_mount_id = entry.mount_id;
        }
        return 0;
    }

    int push_mount_override(const eastl::string &mount_path,
                            const eastl::string &source_path,
                            bool read_only,
                            VfsMountPropagation propagation,
                            int peer_group,
                            int master_peer_group = 0,
                            uint64 parent_mount_id = 0,
                            uint64 *created_mount_id = nullptr)
    {
        return push_mount_override_in(current_mount_namespace_state(), mount_path,
                                      source_path, read_only, propagation,
                                      peer_group, master_peer_group,
                                      parent_mount_id, created_mount_id);
    }

    bool erase_mount_tree_at_index(MountNamespaceState &ns, size_t root_index)
    {
        if (root_index >= ns.mounts.size())
        {
            return false;
        }

        /*
         * 路径前缀不能表达叠挂关系：同一路径上的后续挂载可能是兄弟层，
         * 也可能属于另一个 propagation peer。沿父对象 ID 只收集真实后代，
         * 才能在卸载后正确露出栈中的其他挂载对象。
         */
        eastl::vector<uint64> removed_ids;
        removed_ids.push_back(ns.mounts[root_index].mount_id);
        for (size_t i = root_index + 1; i < ns.mounts.size(); ++i)
        {
            if (eastl::find(removed_ids.begin(), removed_ids.end(),
                            ns.mounts[i].parent_mount_id) != removed_ids.end())
            {
                removed_ids.push_back(ns.mounts[i].mount_id);
            }
        }

        for (size_t i = ns.mounts.size(); i > 0; --i)
        {
            if (eastl::find(removed_ids.begin(), removed_ids.end(),
                            ns.mounts[i - 1].mount_id) != removed_ids.end())
            {
                ns.mounts.erase(ns.mounts.begin() + (i - 1));
            }
        }
        return true;
    }

    bool erase_propagated_mount_at_index(MountNamespaceState &ns,
                                         size_t root_index)
    {
        if (root_index >= ns.mounts.size())
        {
            return false;
        }

        const MountOverride candidate = ns.mounts[root_index];
        for (auto &entry : ns.mounts)
        {
            if (entry.parent_mount_id == candidate.mount_id &&
                entry.path == candidate.path)
            {
                /*
                 * 覆盖候选挂载根的独立挂载会阻止整棵候选树被直接移除。
                 * 传播卸载只摘除下面的候选层，并把存活覆盖层接回原父对象。
                 */
                entry.parent_mount_id = candidate.parent_mount_id;
            }
        }
        return erase_mount_tree_at_index(ns, root_index);
    }

    void collect_propagation_receivers(uint64 origin_ns_id,
                                       const eastl::string &origin_path,
                                       int origin_peer_group,
                                       eastl::vector<PropagationReceiver> &receivers,
                                       uint64 origin_mount_id = 0)
    {
        if (origin_peer_group == 0)
        {
            return;
        }

        for (const auto &ns : g_mount_namespaces)
        {
            for (size_t i = 0; i < ns.mounts.size(); ++i)
            {
                const MountOverride &peer = ns.mounts[i];
                if (ns.id == origin_ns_id &&
                    ((origin_mount_id != 0 && peer.mount_id == origin_mount_id) ||
                     (origin_mount_id == 0 && peer.path == origin_path)))
                {
                    continue;
                }

                /*
                 * 被同一路径上的新挂载覆盖后，旧挂载仍是活动的父挂载和
                 * propagation peer。若只遍历可见层，卸载栈顶时就无法同步
                 * 弹出其他 peer 上对应的最近挂载层。
                 */
                bool same_peer_group = is_shared_mount(peer) && peer.peer_group == origin_peer_group;
                bool slave_receiver = peer.master_peer_group == origin_peer_group;
                if (!same_peer_group && !slave_receiver)
                {
                    continue;
                }

                PropagationReceiver receiver;
                receiver.ns_id = ns.id;
                receiver.mount_id = peer.mount_id;
                receiver.mount_index = i;
                receiver.path = peer.path;
                receiver.source = peer.source;
                receiver.read_only = peer.read_only;
                receiver.propagation = peer.propagation;
                receiver.peer_group = peer.peer_group;
                receiver.master_peer_group = peer.master_peer_group;
                receiver.via_slave = slave_receiver;
                receivers.push_back(receiver);
            }
        }
    }

    bool translate_propagation_target(const eastl::string &origin_parent_path,
                                      const eastl::string &origin_parent_source,
                                      const eastl::string &mount_path,
                                      const PropagationReceiver &receiver,
                                      eastl::string &propagated_target)
    {
        if (!path_matches_mount_prefix(mount_path, origin_parent_path))
        {
            return false;
        }

        eastl::string path_suffix;
        suffix_after_mount_prefix(mount_path, origin_parent_path, path_suffix);
        if (origin_parent_source.empty() || receiver.source.empty())
        {
            append_mount_suffix(receiver.path, path_suffix, propagated_target);
            return true;
        }

        /*
         * shared peer 可以是同一底层挂载中不同根目录的 bind 克隆。先把事件
         * 换算到底层 backing 坐标，再映射到接收者根，才能处理这种根偏移。
         */
        eastl::string backing_mountpoint;
        append_mount_suffix(origin_parent_source, path_suffix, backing_mountpoint);
        if (!path_matches_mount_prefix(backing_mountpoint, receiver.source))
        {
            return false;
        }

        eastl::string receiver_suffix;
        suffix_after_mount_prefix(backing_mountpoint, receiver.source, receiver_suffix);
        append_mount_suffix(receiver.path, receiver_suffix, propagated_target);
        return true;
    }

    void propagate_mount_tree_from_shared_parent(
        uint64 origin_ns_id,
        const eastl::string &origin_parent_path,
        const eastl::string &origin_parent_source,
        int origin_peer_group,
        const eastl::vector<MountOverride> &mounted_tree,
        int depth = 0,
        const eastl::vector<PropagationReceiver> *receiver_snapshot = nullptr,
        uint64 origin_parent_mount_id = 0)
    {
        if (origin_peer_group == 0 || depth > 16 || mounted_tree.empty())
        {
            return;
        }

        const MountOverride &tree_root = mounted_tree.front();
        eastl::vector<PropagationReceiver> receivers;
        if (receiver_snapshot != nullptr)
        {
            receivers = *receiver_snapshot;
        }
        else
        {
            collect_propagation_receivers(origin_ns_id, origin_parent_path,
                                          origin_peer_group, receivers,
                                          origin_parent_mount_id);
        }

        for (const auto &receiver : receivers)
        {
            eastl::string propagated_target;
            if (!translate_propagation_target(origin_parent_path, origin_parent_source,
                                              tree_root.path, receiver,
                                              propagated_target))
            {
                /*
                 * shared+slave 中间节点的根可能比其下游 slave 更窄。事件在
                 * 当前根不可达，不代表在下游也不可达；继续沿该节点自己的
                 * peer group 搜索，允许更宽的下游根接收同一上游事件。
                 */
                if (receiver.via_slave &&
                    receiver.propagation == VFS_MOUNT_SHARED &&
                    receiver.peer_group != 0)
                {
                    propagate_mount_tree_from_shared_parent(
                        origin_ns_id, origin_parent_path, origin_parent_source,
                        receiver.peer_group, mounted_tree, depth + 1);
                }
                continue;
            }

            VfsMountPropagation propagated_type = VFS_MOUNT_SHARED;
            int propagated_peer_group = tree_root.peer_group;
            int propagated_master_group = 0;

            if (receiver.via_slave)
            {
                propagated_master_group = tree_root.peer_group;
                if (receiver.propagation == VFS_MOUNT_SHARED && receiver.peer_group != 0)
                {
                    propagated_peer_group = allocate_mount_peer_group();
                }
                else
                {
                    propagated_type = VFS_MOUNT_SLAVE;
                    propagated_peer_group = 0;
                }
            }
            else
            {
                propagated_master_group = tree_root.master_peer_group;
            }

            MountNamespaceState &receiver_ns = ensure_mount_namespace_state(receiver.ns_id);
            int covered_index = find_exact_mount_index_in(receiver_ns, propagated_target);
            if (covered_index >= 0 &&
                receiver_ns.mounts[static_cast<size_t>(covered_index)].propagation ==
                    VFS_MOUNT_UNBINDABLE)
            {
                /*
                 * unbindable 挂载会截断传播树。对应 dentry 已被该挂载覆盖时，
                 * 上游事件不能再在其上方克隆新挂载。
                 */
                continue;
            }
            uint64 propagated_root_id = 0;
            push_mount_override_in(receiver_ns, propagated_target, tree_root.source,
                                   tree_root.read_only,
                                   propagated_type, propagated_peer_group,
                                   propagated_master_group,
                                   receiver.mount_id,
                                   &propagated_root_id);

            eastl::vector<MountOverride> propagated_tree;
            eastl::vector<uint64> source_ids;
            eastl::vector<uint64> propagated_ids;
            MountOverride propagated_root = tree_root;
            propagated_root.mount_id = propagated_root_id;
            propagated_root.parent_mount_id = receiver.mount_id;
            propagated_root.path = propagated_target;
            propagated_root.propagation = propagated_type;
            propagated_root.peer_group = propagated_peer_group;
            propagated_root.master_peer_group = propagated_master_group;
            propagated_tree.push_back(propagated_root);
            source_ids.push_back(tree_root.mount_id);
            propagated_ids.push_back(propagated_root_id);

            for (size_t i = 1; i < mounted_tree.size(); ++i)
            {
                const MountOverride &child = mounted_tree[i];
                if (!path_matches_mount_prefix(child.path, tree_root.path))
                {
                    continue;
                }

                eastl::string child_suffix;
                suffix_after_mount_prefix(child.path, tree_root.path, child_suffix);
                MountOverride propagated_child = child;
                append_mount_suffix(propagated_target, child_suffix,
                                    propagated_child.path);
                uint64 propagated_parent_id = propagated_root_id;
                for (size_t mapping_index = 0;
                     mapping_index < source_ids.size();
                     ++mapping_index)
                {
                    if (source_ids[mapping_index] == child.parent_mount_id)
                    {
                        propagated_parent_id = propagated_ids[mapping_index];
                        break;
                    }
                }
                if (receiver.via_slave)
                {
                    /*
                     * 整棵传播树都要复刻接收者的 shared/slave 关系，而不只是
                     * 根挂载。这样多级 slave 链上的子挂载才能继续单向传播。
                     */
                    propagated_child.master_peer_group = child.peer_group;
                    if (receiver.propagation == VFS_MOUNT_SHARED &&
                        receiver.peer_group != 0)
                    {
                        propagated_child.propagation = VFS_MOUNT_SHARED;
                        propagated_child.peer_group = allocate_mount_peer_group();
                    }
                    else
                    {
                        propagated_child.propagation = VFS_MOUNT_SLAVE;
                        propagated_child.peer_group = 0;
                    }
                }
                uint64 propagated_child_id = 0;
                push_mount_override_in(receiver_ns, propagated_child.path,
                                       propagated_child.source,
                                       propagated_child.read_only,
                                       propagated_child.propagation,
                                       propagated_child.peer_group,
                                       propagated_child.master_peer_group,
                                       propagated_parent_id,
                                       &propagated_child_id);
                propagated_child.mount_id = propagated_child_id;
                propagated_child.parent_mount_id = propagated_parent_id;
                propagated_tree.push_back(propagated_child);
                source_ids.push_back(child.mount_id);
                propagated_ids.push_back(propagated_child_id);
            }

            /*
             * shared+slave 挂载接收上游事件后，还要继续向自己的下游 slave
             * 传播；纯 slave 只接收，不再向外发起传播。
             */
            if (receiver.via_slave && receiver.propagation == VFS_MOUNT_SHARED &&
                receiver.peer_group != 0)
            {
                propagate_mount_tree_from_shared_parent(
                    receiver.ns_id, receiver.path, receiver.source,
                    receiver.peer_group, propagated_tree, depth + 1,
                    nullptr, receiver.mount_id);
            }
        }
    }

    void propagate_unmount_from_shared_parent(uint64 origin_ns_id,
                                              const eastl::string &origin_parent_path,
                                              const eastl::string &origin_parent_source,
                                              int origin_peer_group,
                                              const eastl::string &mount_path,
                                              int depth = 0,
                                              uint64 origin_parent_mount_id = 0)
    {
        if (origin_peer_group == 0 || depth > 16 ||
            !path_matches_mount_prefix(mount_path, origin_parent_path))
        {
            return;
        }

        eastl::vector<PropagationReceiver> receivers;
        collect_propagation_receivers(origin_ns_id, origin_parent_path,
                                      origin_peer_group, receivers,
                                      origin_parent_mount_id);

        for (const auto &receiver : receivers)
        {
            eastl::string propagated_target;
            if (!translate_propagation_target(origin_parent_path, origin_parent_source,
                                              mount_path, receiver,
                                              propagated_target))
            {
                continue;
            }
            MountNamespaceState &receiver_ns = ensure_mount_namespace_state(receiver.ns_id);
            int covered_index = find_exact_mount_index_with_parent(
                receiver_ns, propagated_target, receiver.mount_id);
            if (covered_index < 0)
            {
                continue;
            }
            const MountOverride candidate =
                receiver_ns.mounts[static_cast<size_t>(covered_index)];
            if (receiver.ns_id == origin_ns_id &&
                is_mount_ancestor_or_self(receiver_ns, candidate.mount_id,
                                          origin_parent_mount_id))
            {
                /*
                 * shared 树在自身子树中再次出现时，坐标映射可能把卸载
                 * 事件折回承载事件的父挂载或其祖先。删除该祖先会连同
                 * 原目标整树拆除，必须留给显式的外层 umount 处理。
                 */
                continue;
            }
            if (candidate.propagation == VFS_MOUNT_UNBINDABLE)
            {
                // unbindable 同时截断 mount 与 umount 传播，不能被上游事件移除。
                continue;
            }
            bool erased = erase_propagated_mount_at_index(
                receiver_ns, static_cast<size_t>(covered_index));

            // shared+slave 接收上游 umount 后，还要把事件继续传给自己的下游 slave。
            if (erased && receiver.via_slave && receiver.propagation == VFS_MOUNT_SHARED &&
                receiver.peer_group != 0)
            {
                propagate_unmount_from_shared_parent(receiver.ns_id, receiver.path,
                                                     receiver.source,
                                                     receiver.peer_group,
                                                     propagated_target,
                                                     depth + 1,
                                                     receiver.mount_id);
            }
        }
    }

    bool open_wants_write_access(uint flags)
    {
        int access_mode = flags & O_ACCMODE;
        return access_mode != O_RDONLY ||
               (flags & (O_CREAT | O_TRUNC | O_TMPFILE)) != 0;
    }

    int validate_linux_path_length(const eastl::string &path)
    {
        if (path.length() >= k_linux_path_max)
        {
            return -ENAMETOOLONG;
        }

        size_t component_len = 0;
        for (size_t i = 0; i < path.length(); ++i)
        {
            if (path[i] == '/')
            {
                component_len = 0;
                continue;
            }

            ++component_len;
            if (component_len > k_linux_name_max)
            {
                return -ENAMETOOLONG;
            }
        }

        return EOK;
    }

    int required_open_access_mask(uint flags)
    {
        int access_mask = 0;
        switch (flags & O_ACCMODE)
        {
        case O_WRONLY:
            access_mask |= W_OK;
            break;
        case O_RDWR:
            access_mask |= (R_OK | W_OK);
            break;
        case O_RDONLY:
        default:
            access_mask |= R_OK;
            break;
        }

        if (flags & O_TRUNC)
        {
            access_mask |= W_OK;
        }

        return access_mask;
    }

    int check_mode_bits_with_fsids(uint32 mode, uint32 owner_uid, uint32 owner_gid,
                                   uint32 fsuid, uint32 fsgid, int requested_mask)
    {
        if (fsuid == 0)
        {
            return 0;
        }

        uint32 perms = 0;
        if (fsuid == owner_uid)
        {
            perms = (mode >> 6) & 0x7;
        }
        else if (fsgid == owner_gid)
        {
            perms = (mode >> 3) & 0x7;
        }
        else
        {
            perms = mode & 0x7;
        }

        if ((requested_mask & R_OK) && !(perms & 0x4))
            return -EACCES;
        if ((requested_mask & W_OK) && !(perms & 0x2))
            return -EACCES;
        if ((requested_mask & X_OK) && !(perms & 0x1))
            return -EACCES;
        return 0;
    }

    int validate_existing_open_permissions(const eastl::string &absolute_path, uint flags)
    {
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr)
        {
            return -EFAULT;
        }
        if (current_proc->get_fsuid() == 0)
        {
            return EOK;
        }

        fs::Kstat st;
        int stat_ret = vfs_path_stat(absolute_path.c_str(), &st, true);
        if (stat_ret < 0)
        {
            return stat_ret;
        }

        return check_mode_bits_with_fsids(st.mode,
                                          st.uid,
                                          st.gid,
                                          current_proc->get_fsuid(),
                                          current_proc->get_fsgid(),
                                          required_open_access_mask(flags));
    }

    inline bool is_kernel_mapped_file_range(uint64 addr, uint64 size)
    {
        if (addr < k_min_kernel_file_ptr || size == 0)
        {
            return false;
        }

        uint64 end = addr + size - 1;
        if (end < addr)
        {
            return false;
        }

        return mem::k_pagetable.kwalk_addr(addr) != 0 &&
               mem::k_pagetable.kwalk_addr(end) != 0;
    }

    inline bool is_probably_live_file_object(fs::file *file_obj)
    {
        if (file_obj == nullptr)
        {
            return false;
        }

        if (!is_kernel_mapped_file_range((uint64)file_obj, sizeof(fs::file)))
        {
            return false;
        }

        uint64 vtable_addr = *(uint64 *)file_obj;
        return is_kernel_mapped_file_range(vtable_addr, sizeof(void *));
    }

    inline bool path_equals_or_has_child(const eastl::string &path, const char *prefix)
    {
        if (prefix == nullptr)
        {
            return false;
        }

        size_t prefix_len = strlen(prefix);
        if (path.length() < prefix_len || path.compare(0, prefix_len, prefix) != 0)
        {
            return false;
        }

        return path.length() == prefix_len || path[prefix_len] == '/';
    }

    bool remap_glibc_runtime_path(const eastl::string &path, eastl::string &remapped_path)
    {
        if (path == "/code/lmbench_src/bin/build/lmbench_all")
        {
            /*
             * lmbench 镜像里的 hello/lat_* 小 wrapper 保留了构建机绝对路径。
             * 评测运行时实际二进制位于当前 libc 根目录，按当前工作目录选择
             * /musl 或 /glibc，避免把两套 libc 的 lmbench_all 混用。
             */
            proc::Pcb *p = proc::k_pm.get_cur_pcb();
            if (p != nullptr && path_equals_or_has_child(p->_cwd_name, "/glibc"))
            {
                remapped_path = "/glibc/lmbench_all";
            }
            else
            {
                remapped_path = "/musl/lmbench_all";
            }
            return true;
        }

        struct PrefixAlias
        {
            const char *from;
            const char *to;
        };

        static const PrefixAlias k_prefix_aliases[] = {
            {"/lib/riscv64-linux-gnu", "/glibc/lib"},
            {"/lib/loongarch64-linux-gnu", "/glibc/lib"},
            {"/usr/lib/riscv64-linux-gnu", "/glibc/lib"},
            {"/usr/lib/loongarch64-linux-gnu", "/glibc/lib"},
            {"/lib64", "/glibc/lib"},
            {"/usr/lib64", "/glibc/lib"},
        };

        static const PrefixAlias k_exact_aliases[] = {
            {"/lib/ld-linux-riscv64-lp64d.so.1", "/glibc/lib/ld-linux-riscv64-lp64d.so.1"},
            {"/lib64/ld-linux-riscv64-lp64d.so.1", "/glibc/lib/ld-linux-riscv64-lp64d.so.1"},
            {"/lib/ld-linux-loongarch-lp64d.so.1", "/glibc/lib/ld-linux-loongarch-lp64d.so.1"},
            {"/lib64/ld-linux-loongarch-lp64d.so.1", "/glibc/lib/ld-linux-loongarch-lp64d.so.1"},
        };

        for (const auto &alias : k_exact_aliases)
        {
            if (path == alias.from)
            {
                remapped_path = alias.to;
                return true;
            }
        }

        for (const auto &alias : k_prefix_aliases)
        {
            if (!path_equals_or_has_child(path, alias.from))
            {
                continue;
            }

            remapped_path = alias.to;
            if (path.length() > strlen(alias.from))
            {
                remapped_path += path.c_str() + strlen(alias.from);
            }
            return true;
        }

        return false;
    }

    eastl::string get_parent_path(const eastl::string &path)
    {
        size_t last_slash = path.find_last_of('/');
        if (last_slash == eastl::string::npos)
        {
            return ".";
        }
        if (last_slash == 0)
        {
            return "/";
        }
        return path.substr(0, last_slash);
    }

    int raw_vfs_is_file_exist(const eastl::string &path)
    {
        struct filesystem *fs = get_fs_from_path(path.c_str());
        if (fs && fs->type == FAT32)
        {
            const char *rel_path = path.c_str();
            if (strcmp(fs->path, "/") != 0)
            {
                size_t mplen = strlen(fs->path);
                if (strncmp(rel_path, fs->path, mplen) == 0)
                {
                    if (rel_path[mplen] == '\0')
                        rel_path = "/";
                    else if (rel_path[mplen] == '/')
                        rel_path += mplen;
                }
            }
            struct fat32_entry *ep = ename((char *)rel_path);
            if (Printer::trace_group_enabled())
                tracef("[raw_vfs_is_file_exist] fat32 path=%s rel=%s entry=%p\n",
                       path.c_str(), rel_path, ep);
            if (ep != nullptr)
            {
                eput(ep);
                return 1;
            }
            return 0;
        }

        struct ext4_inode inode;
        uint32_t ino;
        int res = ext4_raw_inode_fill(path.c_str(), &ino, &inode);
        if (res == EOK)
        {
            return 1;
        }
        if (res == ENOENT)
        {
            return 0;
        }
        return -res;
    }

    int raw_vfs_path2filetype(const eastl::string &path)
    {
        struct filesystem *fs = get_fs_from_path(path.c_str());
        if (fs && fs->type == FAT32)
        {
            const char *rel_path = path.c_str();
            if (strcmp(fs->path, "/") != 0)
            {
                size_t mplen = strlen(fs->path);
                if (strncmp(rel_path, fs->path, mplen) == 0)
                {
                    if (rel_path[mplen] == '\0')
                        rel_path = "/";
                    else if (rel_path[mplen] == '/')
                        rel_path += mplen;
                }
            }
            struct fat32_entry *ep = ename((char *)rel_path);
            if (!ep)
                return -1;
            int type = fs::FileTypes::FT_NORMAL;
            if (ep->attribute & ATTR_DIRECTORY)
                type = fs::FileTypes::FT_DIRECT;
            eput(ep);
            return type;
        }

        struct ext4_inode inode;
        uint32 ino;
        if (ext4_raw_inode_fill(path.c_str(), &ino, &inode) != EOK)
        {
            return -1;
        }

        struct ext4_sblock *sb = NULL;
        ext4_get_sblock(path.c_str(), &sb);
        if (sb == NULL)
        {
            return -1;
        }

        switch (ext4_inode_type(sb, &inode))
        {
        case EXT4_INODE_MODE_CHARDEV:
        case EXT4_INODE_MODE_BLOCKDEV:
            return fs::FileTypes::FT_DEVICE;
        case EXT4_INODE_MODE_DIRECTORY:
            return fs::FileTypes::FT_DIRECT;
        case EXT4_INODE_MODE_FILE:
            return fs::FileTypes::FT_NORMAL;
        case EXT4_INODE_MODE_SOFTLINK:
            return fs::FileTypes::FT_SYMLINK;
        case EXT4_INODE_MODE_FIFO:
            return fs::FileTypes::FT_PIPE;
        case EXT4_INODE_MODE_SOCKET:
            return fs::FileTypes::FT_DEVICE;
        default:
            printfRed("raw_vfs_path2filetype: unknown file type for path: %s\n", path.c_str());
            return -1;
        }
    }

    int raw_vfs_path_stat(const eastl::string &path, fs::Kstat *st)
    {
        struct filesystem *fs = get_fs_from_path(path.c_str());
        if (fs && fs->type == FAT32)
        {
            const char *rel_path = path.c_str();
            if (strcmp(fs->path, "/") != 0)
            {
                size_t mplen = strlen(fs->path);
                if (strncmp(rel_path, fs->path, mplen) == 0)
                {
                    if (rel_path[mplen] == '\0')
                        rel_path = "/";
                    else if (rel_path[mplen] == '/')
                        rel_path += mplen;
                }
            }
            struct fat32_entry *ep = ename((char *)rel_path);
            if (!ep)
                return -ENOENT;

            memset(st, 0, sizeof(fs::Kstat));
            st->ino = (uint64)ep;
            st->dev = fs->dev;
            st->mode = 0;
            if (ep->attribute & ATTR_DIRECTORY)
                st->mode |= S_IFDIR;
            else
                st->mode |= S_IFREG;

            st->mode |= 0755;
            if (ep->attribute & ATTR_READ_ONLY)
                st->mode &= ~0222;

            st->nlink = 1;
            st->uid = 0;
            st->gid = 0;
            st->size = ep->file_size;

            struct mntfs *mnt = (struct mntfs *)fs->fs_data;
            st->blksize = mnt ? mnt->byts_per_clus : 4096;
            st->blocks = (st->size + 511) / 512;

            eput(ep);
            return EOK;
        }

        struct ext4_inode inode;
        uint32 inode_num = 0;
        int status = ext4_raw_inode_fill(path.c_str(), &inode_num, &inode);
        if (status != EOK)
        {
            printfRed("vfs_path_stat: ext4_raw_inode_fill failed for %s, error: %d\n",
                      path.c_str(), status);
            return -status;
        }

        struct ext4_sblock *sb = NULL;
        status = ext4_get_sblock(path.c_str(), &sb);
        if (status != EOK)
        {
            return -status;
        }

        st->dev = 0;
        st->ino = inode_num;
        st->mode = ext4_inode_get_mode(sb, &inode);
        st->nlink = ext4_inode_get_links_cnt(&inode);

        uint32_t raw_uid = ext4_inode_get_uid(&inode);
        uint32_t raw_gid = ext4_inode_get_gid(&inode);
        if (raw_uid > 65535)
        {
            st->uid = 0;
            printfRed("vfs_path_stat: invalid uid %u, using 0\n", raw_uid);
        }
        else
        {
            st->uid = raw_uid;
        }

        if (raw_gid > 65535)
        {
            st->gid = 0;
            printfRed("vfs_path_stat: invalid gid %u, using 0\n", raw_gid);
        }
        else
        {
            st->gid = raw_gid;
        }

        st->rdev = ext4_inode_get_dev(&inode);
        st->size = inode.size_lo;
        st->blksize = 4096;
        if (st->size == 0)
        {
            st->blocks = 0;
        }
        else
        {
            st->blocks = (st->size + 511) / 512;
            if (st->blocks == 0 && st->size > 0)
            {
                st->blocks = 1;
            }
        }

        st->st_atime_sec = ext4_inode_get_access_time(&inode);
        st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF;
        st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
        st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF;
        st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
        st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF;
        st->mnt_id = 0;

        return EOK;
    }

    bool select_runtime_alias_path(const eastl::string &requested_path,
                                   eastl::string &selected_path,
                                   bool allow_parent_fallback)
    {
        selected_path = requested_path;

        eastl::string remapped_path;
        if (!remap_glibc_runtime_path(requested_path, remapped_path) || remapped_path == requested_path)
        {
            return false;
        }

        if (raw_vfs_is_file_exist(requested_path) == 1)
        {
            return false;
        }

        if (raw_vfs_is_file_exist(remapped_path) == 1)
        {
            selected_path = remapped_path;
            return true;
        }

        if (!allow_parent_fallback)
        {
            return false;
        }

        eastl::string requested_parent = get_parent_path(requested_path);
        eastl::string remapped_parent = get_parent_path(remapped_path);
        if (raw_vfs_is_file_exist(requested_parent) != 1 &&
            raw_vfs_is_file_exist(remapped_parent) == 1)
        {
            selected_path = remapped_path;
            return true;
        }

        return false;
    }

    void resolve_bind_mount_path(const eastl::string &path, eastl::string &resolved_path)
    {
        eastl::string current = normalize_path(path);
        if (current.empty())
        {
            current = "/";
        }

        const MountOverride *mount = find_mount_override(current);
        if (mount == nullptr || mount->source.empty())
        {
            resolved_path = current;
            return;
        }

        // 自绑定只改变挂载属性和传播关系，不改变底层路径。
        if (mount->source == mount->path)
        {
            resolved_path = current;
            return;
        }

        /*
         * 普通 bind 只提供一个新的根视图，不能隐式穿过源路径下的子挂载。
         * rbind 或传播产生的子挂载会以独立 MountOverride 记录在目标树中，
         * 因而最长前缀匹配会先命中那些显式子挂载。
         */
        eastl::string suffix;
        suffix_after_mount_prefix(current, mount->path, suffix);
        append_mount_suffix(mount->source, suffix, resolved_path);
    }

    bool select_effective_backing_path(const eastl::string &requested_path,
                                       eastl::string &selected_path,
                                       bool allow_parent_fallback)
    {
        eastl::string mounted_path;
        resolve_bind_mount_path(requested_path, mounted_path);
        selected_path = mounted_path;
        return select_runtime_alias_path(mounted_path, selected_path, allow_parent_fallback);
    }
}

// 路径规范化函数：处理 . 和 ..
eastl::string normalize_path(const eastl::string &path)
{
    if (path.empty())
        return path;

    eastl::vector<eastl::string> components;
    eastl::string current_component;
    bool is_absolute = (path[0] == '/');

    // 分割路径组件
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '/')
        {
            if (!current_component.empty())
            {
                components.push_back(current_component);
                current_component.clear();
            }
        }
        else
        {
            current_component += path[i];
        }
    }
    if (!current_component.empty())
    {
        components.push_back(current_component);
    }

    // 处理 . 和 ..
    eastl::vector<eastl::string> normalized;
    for (const auto &comp : components)
    {
        if (comp == ".")
        {
            // 忽略当前目录
            continue;
        }
        else if (comp == "..")
        {
            // 上级目录
            if (!normalized.empty() && normalized.back() != "..")
            {
                normalized.pop_back();
            }
            else if (!is_absolute)
            {
                // 对于相对路径，保留 ..
                normalized.push_back(comp);
            }
            // 对于绝对路径，根目录的上级还是根目录，所以忽略
        }
        else
        {
            normalized.push_back(comp);
        }
    }

    // 重建路径
    eastl::string result;
    if (is_absolute)
    {
        result = "/";
    }

    for (size_t i = 0; i < normalized.size(); ++i)
    {
        if (i > 0 || is_absolute)
        {
            if (result.back() != '/')
                result += "/";
        }
        result += normalized[i];
    }

    // 如果结果为空且是绝对路径，返回根目录
    if (result.empty() && is_absolute)
    {
        result = "/";
    }
    // 如果结果为空且是相对路径，返回当前目录
    else if (result.empty())
    {
        result = ".";
    }

    return result;
}

// 解析符号链接路径
static int resolve_symlinks(const eastl::string &input_path, eastl::string &resolved_path, int max_depth = 8)
{
    if (max_depth <= 0)
    {
        return -ELOOP; // 符号链接嵌套太深
    }

    // 这里改成显式迭代而不是递归。
    // link08 这种深路径 + 符号链接环在 LoongArch 上会把递归栈一层层压进内核栈 guard page，
    // 最终不是返回 ELOOP，而是在下一次 trap/save 现场时直接 kerneltrap。
    // 迭代化后，最多只保留一层解析栈帧，跨架构行为也会更稳定。
    eastl::string pending_path = input_path;

    for (int remaining_depth = max_depth; remaining_depth > 0; --remaining_depth)
    {
        resolved_path = pending_path;

        // 按 '/' 分割路径
        eastl::vector<eastl::string> path_parts;
        eastl::string current_part;

        for (size_t i = 0; i < pending_path.length(); i++)
        {
            if (pending_path[i] == '/')
            {
                if (!current_part.empty())
                {
                    path_parts.push_back(current_part);
                    current_part.clear();
                }
            }
            else
            {
                current_part += pending_path[i];
            }
        }
        if (!current_part.empty())
        {
            path_parts.push_back(current_part);
        }

        // 重新构建路径，逐步检查每个组件是否为符号链接
        eastl::string current_path = "/";
        bool expanded_symlink = false;

        for (size_t i = 0; i < path_parts.size(); i++)
        {
            if (current_path.back() != '/')
            {
                current_path += "/";
            }
            current_path += path_parts[i];

            // 检查当前路径是否为符号链接
            int type = fs::k_vfs.path2filetype(current_path);
            if (type != fs::FileTypes::FT_SYMLINK)
            {
                continue;
            }

            eastl::string link_path;
            int r = read_symlink_target_for_path(current_path, link_path);
            if (r != EOK)
            {
                return r;
            }
            eastl::string new_path;

            // 如果符号链接是绝对路径，重新开始
            if (!link_path.empty() && link_path[0] == '/')
            {
                new_path = link_path;
            }
            else
            {
                // 相对路径：需要相对于当前组件的父目录
                size_t last_slash = current_path.find_last_of('/');
                if (last_slash == eastl::string::npos || last_slash == 0)
                {
                    new_path = "/" + link_path;
                }
                else
                {
                    new_path = current_path.substr(0, last_slash + 1) + link_path;
                }
            }

            // 添加剩余的路径组件
            for (size_t j = i + 1; j < path_parts.size(); j++)
            {
                if (new_path.back() != '/')
                {
                    new_path += "/";
                }
                new_path += path_parts[j];
            }

            pending_path = normalize_path(new_path);
            expanded_symlink = true;
            break;
        }

        if (!expanded_symlink)
        {
            resolved_path = current_path;
            return 0;
        }
    }

    return -ELOOP;
}

int vfs_resolve_path(const eastl::string &input_path, eastl::string &resolved_path)
{
    return resolve_symlinks(input_path, resolved_path);
}

// 将flags转换为可读的字符串表示
eastl::string flags_to_string(uint flags)
{
    eastl::string result;

    // 处理访问模式（互斥的，只能是其中一个）
    int access_mode = flags & 0x3;
    switch (access_mode)
    {
    case O_RDONLY:
        result += "O_RDONLY";
        break;
    case O_WRONLY:
        result += "O_WRONLY";
        break;
    case O_RDWR:
        result += "O_RDWR";
        break;
    default:
        result += "UNKNOWN_ACCESS";
        break;
    }

    // 处理其他标志（可以组合）
    if (flags & O_CREAT)
        result += "|O_CREAT";
    if (flags & O_EXCL)
        result += "|O_EXCL";
    if (flags & O_NOCTTY)
        result += "|O_NOCTTY";
    if (flags & O_TRUNC)
        result += "|O_TRUNC";
    if (flags & O_APPEND)
        result += "|O_APPEND";
    if (flags & O_NONBLOCK)
        result += "|O_NONBLOCK";
    if (flags & O_DSYNC)
        result += "|O_DSYNC";
    if (flags & O_ASYNC)
        result += "|O_ASYNC";
    if (flags & O_DIRECT)
        result += "|O_DIRECT";
    if (flags & O_LARGEFILE)
        result += "|O_LARGEFILE";
    if (flags & O_DIRECTORY)
        result += "|O_DIRECTORY";
    if (flags & O_NOFOLLOW)
        result += "|O_NOFOLLOW";
    if (flags & O_NOATIME)
        result += "|O_NOATIME";
    if (flags & O_CLOEXEC)
        result += "|O_CLOEXEC";
    if (flags & O_SYNC)
        result += "|O_SYNC";
    if (flags & O_PATH)
        result += "|O_PATH";
    if (flags & O_TMPFILE)
        result += "|O_TMPFILE";

    // 如果有未识别的标志，显示原始十六进制值
    uint known_flags = O_RDWR | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND |
                       O_NONBLOCK | O_DSYNC | O_ASYNC | O_DIRECT | O_LARGEFILE |
                       O_DIRECTORY | O_NOFOLLOW | O_NOATIME | O_CLOEXEC | O_SYNC |
                       O_PATH | O_TMPFILE;
    uint unknown_flags = flags & ~known_flags;
    if (unknown_flags)
    {
        printfRed("Unknown flags: 0x%x\n", unknown_flags);
    }

    return result;
}

// 辅助函数：应用进程的umask到权限模式
static mode_t apply_umask(mode_t mode)
{
    proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
    if (current_proc == nullptr)
    {
        // 如果无法获取当前进程，使用默认umask 022
        return mode & ~0022;
    }

    // 应用当前进程的umask：从mode中清除umask中设置的权限位
    return mode & ~(current_proc->_umask);
}

// 辅助函数：根据flags和文件类型确定文件权限
static mode_t determine_file_mode(uint flags, fs::FileTypes file_type, bool file_exists, int requested_mode)
{
    mode_t mode;

    switch (file_type)
    {
    case fs::FileTypes::FT_NORMAL:
        if (!file_exists && (flags & O_CREAT))
        {
            // 新创建的普通文件，使用请求的权限模式并应用umask
            mode = apply_umask(requested_mode);
        }
        else
        {
            // 现有的普通文件，保持当前权限（这里给默认值）
            mode = 0644;
        }
        break;

    case fs::FileTypes::FT_DEVICE:
        mode = 0666; // rw-rw-rw-，设备文件通常不应用umask
        break;

    case fs::FileTypes::FT_PIPE:
        // FIFO/管道文件，使用默认权限并应用umask
        mode = apply_umask(0644); // rw-r--r--
        break;

    case fs::FileTypes::FT_DIRECT:
        if (!file_exists)
        {
            // 新创建的目录，应用umask
            mode = apply_umask(0755); // rwxr-xr-x
        }
        else
        {
            mode = 0755; // 现有目录保持原权限
        }
        break;

    default:
        mode = apply_umask(0644); // 默认权限并应用umask
        break;
    }

    // 正确处理文件访问模式：检查低两位来确定读写权限
    // 注意：只修改基本权限位（低9位），保留特殊权限位（sticky bit, setuid, setgid）
    mode_t special_bits = mode & 07000; // 保存特殊权限位（sticky, setuid, setgid）
    mode_t basic_perms = mode & 0777;   // 获取基本权限位

    int access_mode = flags & 0x3; // 取低两位
    if (access_mode == O_RDONLY)   // 0x00 - 只读
    {
        basic_perms &= ~0222; // 清除写权限
        basic_perms |= 0444;  // 设置读权限
    }
    else if (access_mode == O_WRONLY) // 0x01 - 只写
    {
        basic_perms &= ~0444; // 清除读权限
        basic_perms |= 0222;  // 设置写权限
    }
    else if (access_mode == O_RDWR) // 0x02 - 读写
    {
        basic_perms |= 0444; // 设置读权限
        basic_perms |= 0222; // 设置写权限
    }

    // 合并特殊权限位和基本权限位
    mode = special_bits | basic_perms;

    return mode;
}

// 新建 inode 的权限应该只来源于调用者传入的 mode 与 umask，
// 不能被这次 open() 的 O_RDONLY/O_WRONLY/O_RDWR 访问模式污染。
static mode_t determine_created_inode_mode(fs::FileTypes file_type, int requested_mode)
{
    switch (file_type)
    {
    case fs::FileTypes::FT_NORMAL:
        return apply_umask(requested_mode);
    case fs::FileTypes::FT_PIPE:
        return apply_umask(0644);
    case fs::FileTypes::FT_DIRECT:
        return apply_umask(0755);
    case fs::FileTypes::FT_DEVICE:
        return 0666;
    default:
        return apply_umask(requested_mode);
    }
}

// 新建 inode 后，owner/group 应继承当前进程的 fsuid/fsgid。
// 这既符合 Linux 文件系统权限语义，也能避免“当前用户刚创建目录，
// 但随后 chmod()/stat() 相关测例又被误判成非 owner”的问题。
static int set_created_inode_owner_from_current_proc(const char *path)
{
    if (path == nullptr)
    {
        return -EFAULT;
    }

    proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
    uint32_t current_uid = 0;
    uint32_t current_gid = 0;
    uint32_t inherited_gid = 0;

    if (current_proc != nullptr)
    {
        current_uid = current_proc->get_fsuid();
        current_gid = current_proc->get_fsgid();
        inherited_gid = current_gid;
    }

    // Linux 在 setgid 目录下创建新 inode 时，需要继承父目录的 gid。
    // 这条规则直接影响 open10 这类测例里“父目录组归属是否被正确继承”。
    eastl::string parent_path = get_parent_path(path);
    if (!parent_path.empty())
    {
        eastl::string resolved_parent;
        int resolve_ret = resolve_symlinks(parent_path, resolved_parent);
        if (resolve_ret == EOK)
        {
            select_effective_backing_path(resolved_parent, resolved_parent, false);

            fs::Kstat parent_st{};
            int parent_stat_ret = raw_vfs_path_stat(resolved_parent, &parent_st);
            if (parent_stat_ret == EOK &&
                (parent_st.mode & S_IFMT) == S_IFDIR &&
                (parent_st.mode & S_ISGID) != 0)
            {
                inherited_gid = parent_st.gid;
            }
        }
    }

    return ext4_owner_set(path, current_uid, inherited_gid);
}

static int validate_lookup_prefix_permissions(const eastl::string &absolute_path)
{
    if (absolute_path.empty() || absolute_path[0] != '/')
    {
        return EOK;
    }

    size_t last_slash = absolute_path.find_last_of('/');
    if (last_slash == eastl::string::npos || last_slash == 0)
    {
        return EOK;
    }

    proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
    if (current_proc == nullptr)
    {
        return -EFAULT;
    }

    eastl::string parent_path = absolute_path.substr(0, last_slash);

    for (size_t start = 1; start < parent_path.length();)
    {
        size_t end = parent_path.find('/', start);
        if (end == eastl::string::npos)
            end = parent_path.length();

        eastl::string current_path = parent_path.substr(0, end);
        eastl::string resolved_current;
        int resolve_ret = resolve_symlinks(current_path, resolved_current);
        if (resolve_ret < 0)
            return resolve_ret;

        eastl::string lookup_current = resolved_current;
        select_effective_backing_path(resolved_current, lookup_current, false);

        fs::vfile_tree_node *virtual_node = fs::k_vfs.get_virtual_node(lookup_current);
        if (virtual_node != nullptr)
        {
            if (virtual_node->file_type != fs::FileTypes::FT_DIRECT)
            {
                return -ENOTDIR;
            }
            start = end + 1;
            continue;
        }

        int exists = raw_vfs_is_file_exist(lookup_current);
        if (exists < 0)
        {
            return exists;
        }
        if (exists == 0)
        {
            return -ENOENT;
        }

        fs::Kstat st;
        int stat_ret = raw_vfs_path_stat(lookup_current, &st);
        if (stat_ret < 0)
        {
            return stat_ret;
        }
        if ((st.mode & S_IFMT) != S_IFDIR)
        {
            return -ENOTDIR;
        }

        uint32_t fsuid = current_proc->get_fsuid();
        uint32_t fsgid = current_proc->get_fsgid();
        if (fsuid != 0)
        {
            bool has_search_permission = false;
            if (fsuid == st.uid)
                has_search_permission = (st.mode & S_IXUSR) != 0;
            else if (fsgid == st.gid)
                has_search_permission = (st.mode & S_IXGRP) != 0;
            else
                has_search_permission = (st.mode & S_IXOTH) != 0;

            if (!has_search_permission)
                return -EACCES;
        }

        start = end + 1;
    }

    return EOK;
}

int vfs_openat(eastl::string absolute_path, fs::file *&file, uint flags, int mode)
{
    // printfYellow("[vfs_openat] : absolute_path=%s, flags=%o, mode=0%o\n", absolute_path.c_str(), flags, mode);
    int length_ret = validate_linux_path_length(absolute_path);
    if (length_ret != EOK)
    {
        printfRed("vfs_openat: path too long: len=%u\n", (uint32)absolute_path.length());
        return length_ret;
    }

    // 解析路径中的符号链接
    eastl::string resolved_path = absolute_path;

    // 分离父目录和文件名
    eastl::string parent_dir;
    eastl::string filename;

    size_t last_slash = absolute_path.find_last_of('/');
    if (last_slash != eastl::string::npos && last_slash > 0)
    {
        parent_dir = absolute_path.substr(0, last_slash);
        filename = absolute_path.substr(last_slash + 1);
    }
    else
    {
        parent_dir = "/";
        if (last_slash == 0 && absolute_path.length() > 1)
            filename = absolute_path.substr(1);
        else
            filename = absolute_path;
    }

    int r = EOK;
    if (flags & O_NOFOLLOW)
    {
        // O_NOFOLLOW 要保留最终组件本体，只解析父目录中的符号链接。
        eastl::string resolved_parent;
        r = resolve_symlinks(parent_dir, resolved_parent);
        if (r < 0)
        {
            printfRed("vfs_openat: failed to resolve parent path %s, error: %d\n", parent_dir.c_str(), r);
            // 只有在严重错误时才返回，否则继续使用原路径
            if (r == -ELOOP)
                return r;
            resolved_parent = parent_dir; // 使用原路径
        }

        // 重新构建路径
        resolved_path = resolved_parent;
        if (resolved_path.back() != '/')
            resolved_path += "/";
        resolved_path += filename;
    }
    else
    {
        // 默认 open 会跟随最终符号链接，完整路径解析一次即可，避免父目录重复遍历。
        r = resolve_symlinks(absolute_path, resolved_path);
        if (r < 0)
        {
            printfRed("vfs_openat: failed to resolve path %s, error: %d\n", absolute_path.c_str(), r);
            if (r == -ELOOP)
                return r;
        }
    }

    length_ret = validate_linux_path_length(resolved_path);
    if (length_ret != EOK)
    {
        printfRed("vfs_openat: resolved path too long: len=%u\n", (uint32)resolved_path.length());
        return length_ret;
    }

    // bind/rbind 挂载改变的是当前命名空间中的路径视图，底层 ext4/FAT 操作
    // 仍然要落到源路径。这里统一在解析符号链接之后做最长前缀映射。
    eastl::string lookup_path = resolved_path;
    select_effective_backing_path(resolved_path, lookup_path, true);

    // 检查是否为 FAT32 分区
    struct filesystem *fs = get_fs_from_path(lookup_path.c_str());
    if (fs && fs->type == FAT32) {
         const char* rel_path = lookup_path.c_str();
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         struct fat32_entry *ep = ename((char*)rel_path);
         
         if (!ep && (flags & O_CREAT)) {
             // Create file
             // Need to find parent entry first
             char name_buf[FAT32_MAX_FILENAME + 1];
             struct fat32_entry *dp = enameparent((char*)rel_path, name_buf);
             if (dp) {
                 elock(dp);
                 int attr = 0;
                 if (flags & O_DIRECTORY) attr = ATTR_DIRECTORY;
                 ep = ealloc(dp, name_buf, attr);
                 eunlock(dp);
                 eput(dp);
             }
         }
         
         if (ep) {
             // Check directory if O_DIRECTORY set
             if ((flags & O_DIRECTORY) && !(ep->attribute & ATTR_DIRECTORY)) {
                  eput(ep);
                  return -ENOTDIR;
             }
             
             // Create file object
             fs::FileAttrs attrs;
             if (ep->attribute & ATTR_DIRECTORY) attrs.filetype = fs::FileTypes::FT_DIRECT;
             else attrs.filetype = fs::FileTypes::FT_NORMAL;
             
             // Determine mode (simplified for FAT32)
             mode_t file_mode = determine_file_mode(flags, attrs.filetype, true, mode);
             attrs._value = file_mode;
             
             file = new fs::fat32_file(attrs, lookup_path, ep);
             /*
              * fd 对外暴露的是挂载命名空间中的路径，底层 FAT 操作继续使用
              * 已解析的 backing 路径，二者不能因 bind 挂载而混为一体。
              */
             file->_path_name = resolved_path;
             file->set_backing_path(lookup_path);
             
             // Handle O_TRUNC
             if ((flags & O_TRUNC) && !(ep->attribute & ATTR_DIRECTORY)) {
                 elock(ep);
                 etrunc(ep);
                 eunlock(ep);
             }
             
             // Handle O_APPEND
             if ((flags & O_APPEND) && !(ep->attribute & ATTR_DIRECTORY)) {
                  file->lseek(0, SEEK_END);
             }

             return EOK;
         } else {
             return -ENOENT;
         }
    }

    int prefix_permission_ret = validate_lookup_prefix_permissions(lookup_path);
    if (prefix_permission_ret < 0)
    {
        printfRed("vfs_openat: prefix permission/path check failed for %s, error: %d\n",
                  lookup_path.c_str(), prefix_permission_ret);
        return prefix_permission_ret;
    }

    bool file_exists = (raw_vfs_is_file_exist(lookup_path) == 1);

    if (open_wants_write_access(flags) && vfs_is_readonly_path(lookup_path))
    {
        printfRed("vfs_openat: readonly mount rejects write-like open for %s\n", lookup_path.c_str());
        return -EROFS;
    }

    // 处理 O_EXCL + O_CREAT 组合：如果文件存在，应该失败
    if ((flags & O_CREAT) && (flags & O_EXCL) && file_exists)
    {
        printfRed("vfs_openat: file %s already exists with O_CREAT|O_EXCL\n", lookup_path.c_str());
        return -EEXIST;
    }

    if (file_exists)
    {
        int perm_ret = validate_existing_open_permissions(lookup_path, flags);
        if (perm_ret < 0)
        {
            printfRed("vfs_openat: permission denied for %s, flags=%s, err=%d\n",
                      lookup_path.c_str(), flags_to_string(flags).c_str(), perm_ret);
            return perm_ret;
        }
    }

    // 处理 O_TMPFILE：创建匿名临时文件
    if (flags & O_TMPFILE)
    {
        // 去除末尾斜杠
        eastl::string dir_path = lookup_path;
        if (!dir_path.empty() && dir_path.back() == '/')
            dir_path.pop_back();

        // O_TMPFILE 要求路径必须是一个存在的目录
        if (!file_exists)
        {
            printfRed("vfs_openat: O_TMPFILE specified but directory %s does not exist\n", dir_path.c_str());
            return -ENOENT;
        }

        int dir_type = vfs_path2filetype(dir_path);
        if (dir_type != fs::FileTypes::FT_DIRECT)
        {
            printfRed("vfs_openat: O_TMPFILE specified but %s is not a directory\n", dir_path.c_str());
            return -ENOTDIR;
        }

        // O_TMPFILE 的两种情况处理
        int access_mode = flags & O_ACCMODE;
        if (access_mode == O_RDONLY)
        {
            // O_TMPFILE | O_RDONLY：打开目录进行读取，而不是创建临时文件
            printfGreen("vfs_openat: O_TMPFILE|O_RDONLY - opening directory %s for reading\n", dir_path.c_str());

            // 移除 O_TMPFILE 标志，按普通目录打开处理
            flags &= ~O_TMPFILE;
            resolved_path = dir_path; // 使用处理过的路径（去除末尾斜杠）
            // 继续执行下面的普通文件处理逻辑
        }
        else
        {
            // O_TMPFILE | O_RDWR/O_WRONLY：创建匿名临时文件
            printfGreen("vfs_openat: O_TMPFILE with write access - creating anonymous temporary file\n");

            // 创建匿名临时文件 - 使用静态计数器和进程地址生成唯一路径
            static uint64_t tmp_counter = 0;
            proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
            uint64_t unique_id = ++tmp_counter + (uint64_t)current_proc;

            char tmp_name[256];
            snprintf(tmp_name, sizeof(tmp_name), "%s/.tmpfile_%x",
                     dir_path.c_str(), unique_id);

            eastl::string tmp_path(tmp_name);

            // 创建临时文件（移除 O_DIRECTORY 和 O_TMPFILE 标志）
            uint temp_flags = flags & ~(O_DIRECTORY | O_TMPFILE);
            temp_flags |= O_CREAT | O_EXCL; // 确保创建新文件

            mode_t file_mode = determine_file_mode(temp_flags, fs::FileTypes::FT_NORMAL, false, mode);
            mode_t inode_mode = determine_created_inode_mode(fs::FileTypes::FT_NORMAL, mode);

            fs::FileAttrs attrs;
            attrs.filetype = fs::FileTypes::FT_NORMAL;
            attrs._value = file_mode;

            fs::normal_file *temp_file = new fs::normal_file(attrs, tmp_path);

            // 创建临时文件
            int status = ext4_fopen2(&temp_file->lwext4_file_struct, tmp_path.c_str(), temp_flags);
            if (status != EOK)
            {
                delete temp_file;
                printfRed("vfs_openat: failed to create O_TMPFILE: %d\n", status);
                return -status; // 返回正确的错误码
            }

            // 重要：恢复 O_TMPFILE 标志，以便权限检查时能识别这是一个临时文件
            temp_file->lwext4_file_struct.flags |= O_TMPFILE;

            // // 立即从目录中删除文件条目，使其成为匿名文件
            // // 这样文件就只能通过文件描述符访问，实现真正的O_TMPFILE语义
            // int unlink_status = ext4_fremove(tmp_path.c_str());
            // if (unlink_status != EOK)
            // {
            //     printfRed("vfs_openat: warning - failed to unlink O_TMPFILE: %d\n", unlink_status);
            //     // 不返回错误，因为文件已经创建成功
            // }

            // 设置文件权限
            status = ext4_mode_set(tmp_path.c_str(), inode_mode);
            if (status != EOK)
            {
                printfGreen("vfs_openat: ext4_mode_set skipped for O_TMPFILE\n");
                // 对于临时文件，这是正常的
            }

            printfGreen("vfs_openat: created O_TMPFILE file, mode: 0%o\n", inode_mode);

            file = temp_file;
            return EOK;
        }
    }

    // 如果文件不存在且没有O_CREAT标志，返回错误
    if (!file_exists && (flags & O_CREAT) == 0)
    {
        printfRed("vfs_openat: file %s does not exist, flags: %d\n", lookup_path.c_str(), flags);
        return -ENOENT; // 文件不存在
    }

    // 确定要使用的实际路径和文件类型
    eastl::string actual_path = lookup_path;
    int type = -1;

    if (file_exists)
    {
        type = raw_vfs_path2filetype(actual_path);
    }
    else
    {
        type = fs::FileTypes::FT_NORMAL; // 新文件默认为普通文件
    }

    const bool open_symlink_itself =
        file_exists && type == fs::FileTypes::FT_SYMLINK &&
        (flags & O_NOFOLLOW) && (flags & O_PATH);

    // 处理 O_NOFOLLOW：如果最终路径是符号链接，应该返回错误
    if ((flags & O_NOFOLLOW) && file_exists && type == fs::FileTypes::FT_SYMLINK &&
        !open_symlink_itself)
    {
        printfRed("vfs_openat: O_NOFOLLOW specified but %s is a symlink\n", resolved_path.c_str());
        return -ELOOP; // 符号链接循环错误
    }

    // 处理 O_DIRECTORY：如果指定了此标志，路径必须是目录
    if ((flags & O_DIRECTORY))
    {
        if (file_exists && type != fs::FileTypes::FT_DIRECT)
        {
            printfRed("vfs_openat: O_DIRECTORY specified but %s is not a directory\n", resolved_path.c_str());
            return -ENOTDIR; // 不是目录
        }
    }

    // 处理目录的特殊限制
    if (file_exists && type == fs::FileTypes::FT_DIRECT)
    {
        int access_mode = flags & O_ACCMODE;

        // 目录只能用 O_RDONLY 打开
        if (access_mode != O_RDONLY)
        {
            printfRed("vfs_openat: cannot open directory %s with write access (flags: %s)\n",
                      resolved_path.c_str(), flags_to_string(flags).c_str());
            return -EISDIR; // 是目录错误
        }

        // 不能对已存在的目录使用 O_CREAT
        if (flags & O_CREAT)
        {
            printfRed("vfs_openat: cannot use O_CREAT on existing directory %s\n", resolved_path.c_str());
            return -EISDIR; // 是目录错误
        }
    }

    // Linux 允许 open(path, O_PATH | O_NOFOLLOW) 获取“符号链接本体”的 fd。
    // readlinkat01 正是依赖这条语义来覆盖空路径 readlinkat(fd, "", ...)。
    if (open_symlink_itself)
    {
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_SYMLINK;
        attrs._value = 0777;

        fs::normal_file *temp_file = new fs::normal_file(attrs, actual_path);
        temp_file->lwext4_file_struct.flags = flags;
        temp_file->_path_name = resolved_path;
        temp_file->set_backing_path(actual_path);
        file = temp_file;
        return EOK;
    }

    int status = -100;

    if (type == fs::FileTypes::FT_NORMAL || (flags & O_CREAT) != 0)
    {
        // 根据flags和文件类型确定适当的权限
        // 专门重写了个函数来确定这个权限
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_NORMAL, file_exists, mode);
        mode_t inode_mode = determine_created_inode_mode(fs::FileTypes::FT_NORMAL, mode);

        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_NORMAL;
        attrs._value = file_mode;

        fs::normal_file *temp_file = new fs::normal_file(attrs, actual_path);
        // printfYellow("vfs_openat: flags: %o, mode: 0%o, actual_path: %s\n", flags, temp_file->_attrs.transMode(), actual_path.c_str());

        // ext4库会自动处理 O_TRUNC, O_RDONLY, O_WRONLY, O_RDWR 等标志
        // 真是前人栽树，后人乘凉啊！
        status = ext4_fopen2(&temp_file->lwext4_file_struct, actual_path.c_str(), flags);
        if (status != EOK)
        {
            delete temp_file;
            printfRed("ext4_fopen2 failed with status: %d for path: %s\n", status, actual_path.c_str());
            return -status;
        }

        // 如果是新创建的文件，设置文件权限到 ext4 inode
        bool is_newly_created = !file_exists && (flags & O_CREAT);
        if (is_newly_created)
        {
            status = ext4_mode_set(actual_path.c_str(), inode_mode);
            if (status != EOK)
            {
                printfRed("ext4_mode_set failed for %s, status: %d\n", actual_path.c_str(), status);
                // 不返回错误，因为文件已经创建成功了
            }
            else
            {
                printfGreen("ext4_mode_set success for %s, mode: 0%o\n", actual_path.c_str(), file_mode);
            }

            // 设置文件所有者和组
            status = set_created_inode_owner_from_current_proc(actual_path.c_str());
            if (status != EOK)
            {
                printfRed("ext4_owner_set failed for %s, status: %d\n", actual_path.c_str(), status);
            }
            else
            {
                printfGreen("ext4_owner_set success for %s\n", actual_path.c_str());
            }
        }

        // 处理 O_APPEND：将文件指针设置到文件末尾
        if (flags & O_APPEND)
        {
            // 这是纯sb设计，后面有机会把这个删了
            temp_file->setAppend();
        }

        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_DEVICE)
    {
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_DEVICE, file_exists, mode);

        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_DEVICE;
        attrs._value = file_mode;

        fs::device_file *temp_file = new fs::device_file(attrs, actual_path);
        status = ext4_fopen2(&temp_file->lwext4_file_struct, actual_path.c_str(), flags);
        if (status != EOK)
        {
            delete temp_file;
            printfRed("Failed to open device file: %d\n", status);
            return status;
        }
        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_DIRECT)
    {
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_DIRECT, file_exists, mode);

        // 创建目录文件对象
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_DIRECT;
        attrs._value = file_mode;

        fs::directory_file *temp_dir = new fs::directory_file(attrs, actual_path);

        // 使用 ext4_dir_open 打开目录
        status = ext4_dir_open(&temp_dir->lwext4_dir_struct, actual_path.c_str());
        if (status != EOK)
        {
            delete temp_dir;
            printfRed("Failed to open directory: %d\n", status);
            return status;
        }

        file = temp_dir;
    }
    else if (type == fs::FileTypes::FT_PIPE)
    {
        mode_t file_mode;

        if (file_exists)
        {
            // 如果文件已存在，从文件系统读取权限
            struct ext4_inode inode;
            uint32 ino;
            if (ext4_raw_inode_fill(absolute_path.c_str(), &ino, &inode) == EOK)
            {
                struct ext4_sblock *sb = NULL;
                ext4_get_sblock(absolute_path.c_str(), &sb);
                if (sb != NULL)
                {
                    file_mode = ext4_inode_get_mode(sb, &inode);
                }
                else
                {
                    file_mode = 0644; // 默认权限
                }
            }
            else
            {
                file_mode = 0644; // 默认权限
            }
        }
        else
        {
            file_mode = determine_file_mode(flags, fs::FileTypes::FT_PIPE, file_exists, mode);
        }

        // 根据打开模式确定是读端还是写端
        int access_mode = flags & O_ACCMODE;

        // 检查 O_NONBLOCK | O_WRONLY 的组合
        // 根据 Linux manual，当使用 O_NONBLOCK | O_WRONLY 打开 FIFO 时，
        // 如果没有进程打开该 FIFO 进行读取，应该返回 ENXIO 错误
        if ((flags & O_NONBLOCK) && (access_mode == O_WRONLY))
        {
            // 检查是否有其他进程已经打开了这个 FIFO 进行读取
            if (!fs::k_fifo_manager.has_readers(absolute_path))
            {
                printfRed("vfs_openat: O_NONBLOCK | O_WRONLY on FIFO %s with no readers\n", absolute_path.c_str());
                return -ENXIO; // 没有设备或地址
            }
        }

        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_PIPE;
        attrs._value = file_mode & 0777; // 只保留权限位

        // 对于 FIFO，使用全局管理器获取或创建 Pipe 对象
        proc::ipc::Pipe *pipe = fs::k_fifo_manager.get_or_create_fifo(absolute_path);
        if (flags & O_NONBLOCK)
        {
            pipe->set_nonblock(true);
            printfYellow("vfs_openat: O_NONBLOCK set for FIFO %s\n", absolute_path.c_str());
        }
        bool is_write_end = false;
        if (access_mode == O_WRONLY)
        {
            is_write_end = true;
        }
        else if (access_mode == O_RDONLY)
        {
            is_write_end = false;
        }
        else
        {
            // O_RDWR 需要像 Linux FIFO 一样同时保留读端和写端，
            // 否则同一个 fd 上的写入会被误判成 EBADF。
            is_write_end = false;
        }

        // 创建带有路径信息的 pipe_file
        fs::pipe_file *temp_file = new fs::pipe_file(attrs, pipe, is_write_end, absolute_path);

        // 注册到全局管理器
        if (access_mode == O_RDWR)
        {
            temp_file->set_duplex_mode();
            fs::k_fifo_manager.open_fifo(absolute_path, false);
            fs::k_fifo_manager.open_fifo(absolute_path, true);
        }
        else
        {
            fs::k_fifo_manager.open_fifo(absolute_path, is_write_end);
        }

        printfCyan("vfs_openat: Created FIFO/pipe file: %s, write_end: %d, mode: 0%o\n",
                   absolute_path.c_str(), is_write_end, temp_file->_stat.mode);
        status = EOK; // 直接设置为成功

        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_SOCKET)
    {
        // Socket文件暂时不支持
        printfRed("vfs_openat: O_SOCKET not supported yet\n");
        return -ENOSYS; // Socket文件暂时不支持
    }
    else
    {
        printfRed("Unsupported file type: %d\n", type);
        panic("Unsupported file type: %d", type);
        return -ENOTSUP;
    }

    if (file != nullptr)
    {
        /*
         * /proc/self/fd、fchdir 和 *at(dirfd, ...) 必须保留调用者看到的路径；
         * 已打开的 ext4/FAT 句柄及后续底层操作则使用 backing_path。
         */
        file->_path_name = resolved_path;
        file->set_backing_path(lookup_path);
    }

    // 处理 O_LARGEFILE：检查文件大小限制
    if (!(flags & O_LARGEFILE) && file != nullptr)
    {
        // 对于32位系统，如果文件大小超过2GB且没有指定O_LARGEFILE，应该失败
        // 这里简化处理，假设如果文件存在且大小超过限制就报错
        if (file_exists && file->_stat.size > 0x7FFFFFFF) // 2GB
        {
            printfRed("vfs_openat: file %s is too large, O_LARGEFILE required\n", absolute_path.c_str());
            delete file;
            file = nullptr;
            return -EOVERFLOW;
        }
    }

    // 处理 O_CLOEXEC：设置执行时关闭标志
    if ((flags & O_CLOEXEC) && file != nullptr)
    {
        // 在文件对象上设置相应的标志
        // 这个标志会在exec系统调用时自动关闭文件描述符
        // 注意：这里需要在实际使用时在文件描述符表中设置FD_CLOEXEC
        printfYellow("vfs_openat: O_CLOEXEC flag set for file %s\n", absolute_path.c_str());
    }
    return EOK;
}

int vfs_is_dir(eastl::string &absolute_path)
{
    // 这个函数可以滚蛋了，以后弃用
    struct ext4_dir dir_obj;
    struct ext4_dir *dir = &dir_obj;
    printfRed("dir: %p\n", dir);

    int status = ext4_dir_open(dir, absolute_path.c_str());
    printfYellow("dir->f.mp->name: %s\n", dir->f.mp->name);
    if (status < 0)
    {
        return status;
    }
    // Do something with the directory
    return 0;
}

int vfs_path2filetype(eastl::string &absolute_path)
{
    eastl::string lookup_path = absolute_path;
    select_effective_backing_path(absolute_path, lookup_path, false);
    return raw_vfs_path2filetype(lookup_path);
}

int create_and_write_file(const char *path, const char *data)
{
    int res;
    ext4_file file;

    // 检查文件是否已存在
    if (vfs_is_file_exist(path) == 1)
    {
        printf("File already exists: %s\n", path);
        ext4_fclose(&file);
        return EEXIST;
    }

    // 创建并打开文件
    res = ext4_fopen(&file, path, "wb+");
    if (res != EOK)
    {
        printf("Failed to open file: %d\n", res);
        return res;
    }

    // 写入数据
    size_t data_len = strlen(data);
    size_t written;
    res = ext4_fwrite(&file, data, data_len, &written);
    if (res != EOK || written != data_len)
    {
        printf("Failed to write file: %d, written: %u\n", res, written);
        ext4_fclose(&file);
        return res;
    }

    // 关闭文件
    res = ext4_fclose(&file);
    if (res != EOK)
    {
        printf("Failed to close file: %d\n", res);
        return res;
    }

    return EOK;
}

int vfs_is_file_exist(const char *path)
{
    // 先解析符号链接
    eastl::string resolved_path;
    int res = resolve_symlinks(eastl::string(path), resolved_path);
    if (res != 0) {
        printfRed("vfs_is_file_exist: failed to resolve symlinks for path: %s\n", path);
        return 0;
    }

    eastl::string lookup_path = resolved_path;
    select_effective_backing_path(resolved_path, lookup_path, false);

    int exists = raw_vfs_is_file_exist(lookup_path);
    if (exists == 1)
    {
        return 1;
    }

    eastl::string remapped_path;
    if (exists == 0 &&
        remap_glibc_runtime_path(lookup_path, remapped_path) &&
        remapped_path != lookup_path)
    {
        exists = raw_vfs_is_file_exist(remapped_path);
        if (exists == 1)
        {
            return 1;
        }
    }

    if (exists == 0)
    {
        printfRed("vfs_is_file_exist: file not found: %s\n", path);
        return 0;
    }

    printfRed("vfs_is_file_exist: error %d for path: %s\n", -exists, path);
    return exists;
}
uint vfs_read_file(const char *path, uint64 buffer_addr, size_t offset, size_t size)
{
    // if (vfs_is_file_exist(path) != 1)
    // {
    //     printfRed("文件不存在\n");
    //     return -ENOENT;
    // }

    int res;
    ext4_file file;
    
    // 解析符号链接
    eastl::string resolved_path;
    res = resolve_symlinks(eastl::string(path), resolved_path);
    if (res != 0) {
        printfRed("Failed to resolve symlinks for path: %s, error: %d\n", path, res);
        return res;
    }

    select_effective_backing_path(resolved_path, resolved_path, false);
    
    struct filesystem *fs = get_fs_from_path(resolved_path.c_str());
    if (fs && fs->type == FAT32) {
         const char* rel_path = resolved_path.c_str();
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         struct fat32_entry *ep = ename((char*)rel_path);
         if (!ep) return -ENOENT;
         
         elock(ep);
         int n = eread(ep, 0, buffer_addr, offset, size);
         eunlock(ep);
         eput(ep);
         return n;
    }

    // printfCyan("vfs_read_file: resolved path %s -> %s\n", path, resolved_path.c_str());

    // 打开文件（只读模式）
    res = ext4_fopen(&file, resolved_path.c_str(), "rb");
    if (res != EOK)
    {
        printfRed("Failed to open file: %d\n", res);
        return res;
    }

    // 如果有偏移，设置文件指针位置
    if (offset > 0)
    {
        res = ext4_fseek(&file, offset, SEEK_SET);
        if (res != EOK)
        {
            printfRed("Failed to seek file: %d\n", res);
            ext4_fclose(&file);
            return res;
        }
    }

    // 读取数据
    size_t bytes_read;
    res = ext4_fread(&file, (void *)buffer_addr, size, &bytes_read);
    if (res != EOK)
    {
        printfRed("Failed to read file: %d\n", res);
        ext4_fclose(&file);
        return res;
    }

    // 关闭文件
    res = ext4_fclose(&file);
    if (res != EOK)
    {
        printfRed("Failed to close file: %d\n", res);
        return res;
    }

    // 返回实际读取的字节数
    return bytes_read;
}

int vfs_getdents(fs::file *const file, struct linux_dirent64 *dirp, uint count)
{
    if (file && file->is_virtual && file->_attrs.filetype == fs::FileTypes::FT_DIRECT)
    {
        eastl::vector<DirentSnapshot> entries;
        append_dirent_snapshot(entries, ".", T_DIR);
        append_dirent_snapshot(entries, "..", T_DIR);

        eastl::vector<eastl::string> virtual_children;
        fs::k_vfs.list_virtual_files(file->_path_name, virtual_children);
        for (const auto &name : virtual_children)
        {
            eastl::string child_path = file->_path_name;
            if (child_path.empty() || child_path.back() != '/')
                child_path += "/";
            child_path += name;

            unsigned char dtype = T_FILE;
            fs::vfile_tree_node *node = fs::k_vfs.get_virtual_node(child_path);
            if (node && node->file_type == fs::FileTypes::FT_DIRECT)
                dtype = T_DIR;
            append_dirent_snapshot(entries, name, dtype);
        }

        // /proc 需要暴露当前活跃 PID 目录，否则 ps/top 一类工具看不到进程。
        if (file->_path_name == "/proc")
        {
            for (const proc::Pcb &pcb : proc::k_proc_pool)
            {
                if (pcb._state == proc::ProcState::UNUSED || pcb._pid <= 0)
                    continue;

                char pid_buf[16];
                snprintf(pid_buf, sizeof(pid_buf), "%d", pcb._pid);
                append_dirent_snapshot(entries, eastl::string(pid_buf), T_DIR);
            }
        }

        int real_dir_ret = collect_real_directory_entries(file->_path_name, entries);
        if (real_dir_ret < 0 && real_dir_ret != -ENOENT && real_dir_ret != -ENOTDIR)
        {
            return real_dir_ret;
        }

        size_t index = static_cast<size_t>(file->_file_ptr);
        struct linux_dirent64 *d = dirp;
        int totlen = 0;

        while (index < entries.size())
        {
            const DirentSnapshot &entry = entries[index];
            const eastl::string &name = entry.name;
            uint reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + name.size() + 1;
            if (reclen % 8)
                reclen = reclen - reclen % 8 + 8;
            if (reclen < sizeof(struct linux_dirent64))
                reclen = sizeof(struct linux_dirent64);
            if (totlen + (int)reclen > (int)count)
                break;

            memset(d, 0, reclen);
            strncpy(d->d_name, name.c_str(), name.size() + 1);
            d->d_ino = index + 1;
            d->d_off = index + 1;
            d->d_reclen = reclen;
            d->d_type = entry.type;

            totlen += reclen;
            d = (struct linux_dirent64 *)((char *)d + reclen);
            ++index;
        }

        file->_file_ptr = index;
        return totlen;
    }

    // FAT32 support
    if (file && file->_attrs.filetype == fs::FileTypes::FT_DIRECT) {
        struct filesystem *fs = get_fs_from_path(file->_path_name.c_str());
        if (fs && fs->type == FAT32) {
             fs::fat32_file *fat_f = (fs::fat32_file*)file;
             if (!fat_f || !fat_f->fat_info.entry) return -EINVAL;
             
             struct fat32_entry *dp = fat_f->fat_info.entry;
             elock(dp);
             
             [[maybe_unused]]int index = 0;
             struct linux_dirent64 *d = dirp;
             int totlen = 0;
             uint32 off = file->_file_ptr; // Use file pointer as offset in directory entries

             struct fat32_entry ep_store;
             struct fat32_entry *ep = &ep_store;
             int ent_count = 0;
             int type;
             
             while (totlen + sizeof(struct linux_dirent64) <= count) {
                  ep->valid = 0;
                  type = enext(dp, ep, off, &ent_count);
                  
                  if (type == -1) {
                      break; // End of directory
                  }
                  
                  off += ent_count * 32; // Advance offset by consumed 32-byte entries
                  
                  if (type == 0) { // Empty entry
                       continue;
                  }

                  int namelen = strlen(ep->filename);
                  uint reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + namelen + 1;
                  if (reclen % 8) reclen = reclen - reclen % 8 + 8;
                  if (reclen < sizeof(struct linux_dirent64)) reclen = sizeof(struct linux_dirent64);
                  
                  if (totlen + reclen > count) {
                      off -= ent_count * 32;
                      break; 
                  }
                  
                  strncpy(d->d_name, ep->filename, namelen + 1);
                  d->d_ino = 0; 
                  d->d_off = off;
                  d->d_reclen = reclen;
                  
                  if (ep->attribute & ATTR_DIRECTORY) d->d_type = T_DIR;
                  else d->d_type = T_FILE;
                  
                  totlen += reclen;
                  d = (struct linux_dirent64 *)((char *)d + reclen);
             }
             
             file->_file_ptr = off; // Update file pointer
             eunlock(dp);
             printfYellow("[vfs_getdents] returning totlen=%d, new off=%u\n", totlen, off);
             return totlen;
        }
    }

    int index = 0;
    struct linux_dirent64 *d;
    const ext4_direntry *rentry;
    int totlen = 0;
    uint64 current_offset = 0;

    /* make integer count */
    if (count == 0)
    {
        return EINVAL;
    }
    if (file == nullptr || file->lwext4_dir_struct.f.mp == nullptr)
    {
        printfRed("[vfs_getdents] file is null or mount point is null\n");
        return EINVAL;
    }
    // ext4_dir_entry_next(&file->lwext4_dir_struct);
    // ext4_dir_entry_next(&file->lwext4_dir_struct); //< 跳过/.和/..
    d = dirp;
    while (1)
    {
        const uint64 entry_offset = file->lwext4_dir_struct.next_off;
        rentry = ext4_dir_entry_next(&file->lwext4_dir_struct);
        if (rentry == NULL)
            break;

        // ext4 目录项名不是以 '\0' 结尾的 C 字符串，必须使用目录项自带长度。
        const uint16_t raw_namelen = rentry->name_length;
        if (raw_namelen == 0 || raw_namelen > EXT4_DIRECTORY_FILENAME_LEN)
        {
            printfRed("[vfs_getdents] 遇到异常 ext4 目录项长度: %u\n", raw_namelen);
            return -EIO;
        }

        const uint16_t namelen = raw_namelen;
        /*
         * 长度是前四项的19加上namelen(字符串长度包括结尾的\0)
         * reclen是namelen+2,如果是+1会错误。原因是没考虑name[]开头的'\'
         */
        uint reclen = sizeof d->d_ino + sizeof d->d_off + sizeof d->d_reclen + sizeof d->d_type + namelen + 1;
        if (reclen % 8)
            reclen = reclen - reclen % 8 + 8; //<对齐
        if (reclen < sizeof(struct linux_dirent64))
            reclen = sizeof(struct linux_dirent64);

        if (totlen + (int)reclen > (int)count)
        {
            // 当前目录项尚未复制给用户，下一次 getdents64 必须从同一项重试。
            file->lwext4_dir_struct.next_off = entry_offset;
            break;
        }

        char name[MAXPATH] = {0};
        const size_t copy_len = eastl::min<size_t>(namelen, MAXPATH - 1);
        memcpy(name, rentry->name, copy_len);
        name[copy_len] = '\0';

        // 过滤掉 O_TMPFILE 创建的临时文件，让它们在目录遍历时不可见
        if (strncmp(name, ".tmpfile_", 9) == 0)
        {
            printfYellow("vfs_getdents: filtering out O_TMPFILE: %s\n", name);
            continue; // 跳过这个条目，不返回给用户空间
        }

        memcpy(d->d_name, name, copy_len + 1);

        if (rentry->inode_type == EXT4_DE_DIR)
        {
            d->d_type = T_DIR;
        }
        else if (rentry->inode_type == EXT4_DE_REG_FILE)
        {
            d->d_type = T_FILE;
        }
        else if (rentry->inode_type == EXT4_DE_CHRDEV)
        {
            d->d_type = T_CHR;
        }
        else
        {
            d->d_type = T_UNKNOWN;
        }
        d->d_ino = rentry->inode;
        d->d_off = current_offset + reclen; // start from 1
        d->d_reclen = reclen;
        ++index;
        totlen += d->d_reclen;
        current_offset += reclen;
        d = (struct linux_dirent64 *)((char *)d + d->d_reclen);
    }

    return totlen;
}

int vfs_mkdir(const char *path, uint64_t mode)
{
    eastl::string normalized_path = normalize_path(path);
    eastl::string effective_path;
    resolve_bind_mount_path(normalized_path, effective_path);
    path = effective_path.c_str();
    printfYellow("vfs_mkdir: creating directory: %s with mode: 0%o\n", path, mode);
    /* Check if the directory already exists */
    if (vfs_is_file_exist(path) == 1)
    {
        printfRed("vfs_mkdir: directory already exists: %s\n", path);
        return -EEXIST;
    }

    struct filesystem *fs = get_fs_from_path(path);
    if (fs && fs->type == FAT32) {
         const char* rel_path = path;
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         
         char name_buf[FAT32_MAX_FILENAME + 1];
         struct fat32_entry *dp = enameparent((char*)rel_path, name_buf);
         if (!dp) {
             printfRed("vfs_mkdir: parent not found for %s\n", path);
             return -ENOENT;
         }
         
         elock(dp);
         struct fat32_entry *ep = ealloc(dp, name_buf, ATTR_DIRECTORY);
         eunlock(dp);
         eput(dp);
         
         if (ep) {
             eput(ep);
             printfGreen("vfs_mkdir: created FAT32 directory %s\n", path);
             return EOK;
         }
         printfRed("vfs_mkdir: failed to create FAT32 directory %s\n", path);
         return -EIO;
    }

    /* Create the directory. */
    int status = ext4_dir_mk(path);
    if (status != EOK)
        return -status;

    /* Apply umask to the mode and set directory permissions. */
    mode_t final_mode = apply_umask(mode);
    // Linux 会让“在 setgid 父目录里新建的子目录”自动继承 S_ISGID。
    // mkdir02 依赖这条语义来验证组继承和目录 setgid 传播。
    eastl::string parent_path = get_parent_path(path);
    if (!parent_path.empty())
    {
        eastl::string resolved_parent;
        int resolve_ret = resolve_symlinks(parent_path, resolved_parent);
        if (resolve_ret == EOK)
        {
            select_effective_backing_path(resolved_parent, resolved_parent, false);
            fs::Kstat parent_st{};
            int parent_stat_ret = raw_vfs_path_stat(resolved_parent, &parent_st);
            if (parent_stat_ret == EOK &&
                (parent_st.mode & S_IFMT) == S_IFDIR &&
                (parent_st.mode & S_ISGID) != 0)
            {
                final_mode |= S_ISGID;
            }
        }
    }
    status = ext4_mode_set(path, final_mode);
    if (status != EOK)
    {
        return -status;
    }

    status = set_created_inode_owner_from_current_proc(path);
    if (status != EOK)
    {
        printfRed("vfs_mkdir: ext4_owner_set failed for %s, status: %d\n", path, status);
        return -status;
    }

    return EOK;
}

int vfs_mknod(const eastl::string &path, mode_t mode, dev_t dev)
{
    if (path.empty())
    {
        return -ENOENT;
    }

    int length_ret = validate_linux_path_length(path);
    if (length_ret != EOK)
    {
        return length_ret;
    }

    // mknod 不跟随最后一个路径组件，但必须解析父目录中的符号链接。
    eastl::string parent_path = get_parent_path(path);
    size_t last_slash = path.find_last_of('/');
    eastl::string filename = last_slash == eastl::string::npos
                                 ? path
                                 : path.substr(last_slash + 1);
    if (filename.empty())
    {
        return -ENOENT;
    }

    eastl::string resolved_parent;
    int resolve_ret = resolve_symlinks(parent_path, resolved_parent);
    if (resolve_ret < 0)
    {
        return resolve_ret;
    }

    eastl::string effective_path = resolved_parent;
    if (effective_path.empty())
    {
        effective_path = "/";
    }
    if (effective_path.back() != '/')
    {
        effective_path += "/";
    }
    effective_path += filename;
    effective_path = normalize_path(effective_path);

    length_ret = validate_linux_path_length(effective_path);
    if (length_ret != EOK)
    {
        return length_ret;
    }

    int prefix_ret = validate_lookup_prefix_permissions(effective_path);
    if (prefix_ret < 0)
    {
        return prefix_ret;
    }

    // 只读挂载是文件系统级约束，errno 优先于父目录写权限。
    // 非特权调用者在只读挂载点创建节点时也必须得到 EROFS。
    if (vfs_is_readonly_path(effective_path))
    {
        return -EROFS;
    }

    fs::Kstat parent_st{};
    int parent_ret = raw_vfs_path_stat(resolved_parent, &parent_st);
    if (parent_ret < 0)
    {
        return parent_ret;
    }
    if ((parent_st.mode & S_IFMT) != S_IFDIR)
    {
        return -ENOTDIR;
    }

    proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
    if (current_proc == nullptr)
    {
        return -EFAULT;
    }

    int permission_ret = check_mode_bits_with_fsids(parent_st.mode,
                                                     parent_st.uid,
                                                     parent_st.gid,
                                                     current_proc->get_fsuid(),
                                                     current_proc->get_fsgid(),
                                                     W_OK | X_OK);
    if (permission_ret < 0)
    {
        return permission_ret;
    }

    int exists = raw_vfs_is_file_exist(effective_path);
    if (exists < 0)
    {
        return exists;
    }
    if (exists == 1)
    {
        return -EEXIST;
    }

    const mode_t file_type = mode & S_IFMT;
    if (file_type == S_IFREG || file_type == 0)
    {
        fs::file *created_file = nullptr;
        int open_ret = vfs_openat(effective_path,
                                  created_file,
                                  O_CREAT | O_EXCL | O_WRONLY,
                                  mode & 07777);
        if (open_ret < 0)
        {
            return open_ret;
        }
        if (created_file == nullptr)
        {
            return -EIO;
        }
        return vfs_free_file(created_file);
    }

    uint32 internal_mode = 0;
    switch (file_type)
    {
    case S_IFCHR:
    case S_IFBLK:
    {
        constexpr uint32 cap_mknod = 27;
        bool has_cap_mknod =
            proc::k_capability.has_effective(current_proc, cap_mknod);
        if (!has_cap_mknod)
        {
            return -EPERM;
        }
        internal_mode = file_type == S_IFCHR ? T_CHR : T_BLK;
        break;
    }
    case S_IFIFO:
        internal_mode = T_FIFO;
        break;
    case S_IFSOCK:
        internal_mode = T_SOCK;
        break;
    default:
        return -EINVAL;
    }

    filesystem_t *target_fs = get_fs_from_path(effective_path.c_str());
    if (target_fs == nullptr)
    {
        return -ENOENT;
    }
    if (target_fs->type != EXT4)
    {
        return -ENOTSUP;
    }

    int create_ret = vfs_ext_mknod(effective_path.c_str(), internal_mode, dev);
    if (create_ret < 0)
    {
        return create_ret;
    }

    // 特殊文件与普通 O_CREAT 文件遵循同一套 umask、owner/group 初始化规则。
    int status = ext4_mode_set(effective_path.c_str(), apply_umask(mode & 07777));
    if (status == EOK)
    {
        status = set_created_inode_owner_from_current_proc(effective_path.c_str());
    }
    if (status != EOK)
    {
        // 元数据初始化失败时撤销目录项，避免向调用者留下半初始化 inode。
        (void)vfs_ext_unlink(effective_path.c_str());
        return -status;
    }

    return EOK;
}

int vfs_fstat(fs::file *f, fs::Kstat *st)
{
    if (!is_probably_live_file_object(f))
    {
        printfRed("[vfs_fstat] 检测到异常文件对象: file=%p\n", f);
        return -EBADF;
    }

    // 检查是否是 pipe_file，如果是，直接使用其内部的 _stat
    if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
    {
        *st = f->_stat;
        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] pipe mode=0%o\n", st->mode);
        return EOK;
    }

    // 检查是否是 memfd 文件（对外名字和底层匿名临时文件路径是分离的）
    if (f->is_memfd())
    {
        // 为 memfd 文件创建合成的 stat 信息
        memset(st, 0, sizeof(fs::Kstat));

        st->dev = 0;                            // 设备号
        st->ino = f->shared_memfd_state() ? (uint64)f->shared_memfd_state() : (uint64)f;
        st->mode = S_IFREG | 0600;              // 常规文件，拥有者读写权限
        st->nlink = 1;                          // 链接数
        st->uid = 0;                            // 用户 ID (root)
        st->gid = 0;                            // 组 ID (root)
        st->rdev = 0;                           // 设备 ID
        st->size = f->memfd_size();
        st->blksize = 4096;                     // 块大小
        st->blocks = (st->size + 511) / 512;    // 512字节块数

        // 设置时间戳（使用当前时间）
        uint64 current_time = NS_to_S(TIME2NS(rdtime()));
        st->st_atime_sec = current_time;
        st->st_atime_nsec = 0;
        st->st_ctime_sec = current_time;
        st->st_ctime_nsec = 0;
        st->st_mtime_sec = current_time;
        st->st_mtime_nsec = 0;
        st->mnt_id = 0;

        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] memfd path=%s size=%u\n", f->_path_name.c_str(), st->size);
        return EOK;
    }

    // 检查是否是设备文件
    if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
    {
        // 为设备文件创建合成的stat信息
        memset(st, 0, sizeof(fs::Kstat));
        
        st->dev = 0;                                        // 设备号
        st->ino = (uint64)f;                                // 使用文件对象地址作为伪inode号
        st->mode = S_IFCHR | 0600;                          // 字符设备，拥有者读写权限
        st->nlink = 1;                                      // 链接数
        st->uid = 0;                                        // 用户ID (root)
        st->gid = 0;                                        // 组ID (root)
        
        // 设备号设为简单的默认值，因为_dev_num是私有成员
        st->rdev = 1;                                       // 标准输出设备号
        
        st->size = 0;                                       // 设备文件大小通常为0
        st->blksize = 4096;                                 // 块大小
        st->blocks = 0;                                     // 设备文件不占用块
        
        // 设置时间戳（使用当前时间）
        uint64 current_time = NS_to_S(TIME2NS(rdtime()));
        st->st_atime_sec = current_time;
        st->st_atime_nsec = 0;
        st->st_ctime_sec = current_time;
        st->st_ctime_nsec = 0;
        st->st_mtime_sec = current_time;
        st->st_mtime_nsec = 0;
        st->mnt_id = 0;
        
        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] device path=%s rdev=%u\n", f->_path_name.c_str(), st->rdev);
        return EOK;
    }

    // 检查是否是符号链接文件
    if (f->_attrs.filetype == fs::FileTypes::FT_SYMLINK)
    {
        struct ext4_inode inode;
        uint32 inode_num = 0;
        const char *file_path = f->_path_name.c_str();

        int status = ext4_raw_inode_fill(file_path, &inode_num, &inode);
        if (status != EOK)
            return -status;

        struct ext4_sblock *sb = NULL;
        status = ext4_get_sblock(file_path, &sb);
        if (status != EOK)
            return -status;

        st->dev = 0;
        st->ino = inode_num;
        st->mode = ext4_inode_get_mode(sb, &inode);
        st->nlink = ext4_inode_get_links_cnt(&inode);

        // 获取原始 uid 和 gid
        st->uid = ext4_inode_get_uid(&inode);
        st->gid = ext4_inode_get_gid(&inode);

        st->rdev = ext4_inode_get_dev(&inode);
        st->size = ext4_inode_get_size(sb, &inode);
        st->blksize = 4096;
        st->blocks = (st->size + 511) / 512;

        st->st_atime_sec = ext4_inode_get_access_time(&inode);
        st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF;
        st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
        st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF;
        st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
        st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF;
        st->mnt_id = 0;

        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] symlink path=%s mode=0%o size=%u\n",
                   f->_path_name.c_str(), st->mode, st->size);
        return EOK;
    }

    struct filesystem *fs = get_fs_from_path(f->_path_name.c_str());
    if (fs && fs->type == FAT32) {
         fs::fat32_file *fat_f = (fs::fat32_file*)f;
         if (fat_f && fat_f->fat_info.entry) {
             struct fat32_entry *ep = fat_f->fat_info.entry;
             memset(st, 0, sizeof(fs::Kstat));
             
             // Use the first cluster number as a stable, non-negative inode id
             uint64 cluster_id = ep->first_clus;
             if (cluster_id == 0)
                 cluster_id = ep->off;
             if (cluster_id == 0)
                 cluster_id = ((uint64)ep) & 0x7fffffff;
             st->ino = cluster_id;
             st->dev = fs->dev;
             
             st->mode = 0;
             if (ep->attribute & ATTR_DIRECTORY) st->mode |= S_IFDIR;
             else st->mode |= S_IFREG;
             
             st->mode |= 0755;
             if (ep->attribute & ATTR_READ_ONLY) st->mode &= ~0222;
             
             st->nlink = 1;
             st->uid = 0;
             st->gid = 0;
             st->rdev = 0;
             st->size = ep->file_size;
             
             struct mntfs *mnt = (struct mntfs *)fs->fs_data;
             if (mnt) st->blksize = mnt->byts_per_clus;
             else st->blksize = 512;
             
             st->blocks = (st->size + 511) / 512;
             
             if (Printer::trace_group_enabled())
                 tracef("[vfs_fstat] fat32 path=%s mode=0%o size=%u\n",
                        f->_path_name.c_str(), st->mode, st->size);
             return EOK;
         }
    }

    // 对已 unlink 但仍被打开的 ext4 文件（如 tmpfile），不能再按路径回查 inode。
    // 优先使用打开时保存的 ext4 inode 句柄，这样匿名临时文件和 O_TMPFILE 都能正确 fstat。
    if (f->lwext4_file_struct.mp != nullptr && f->lwext4_file_struct.inode > 0)
    {
        struct ext4_inode_ref inode_ref;
        int result = ext4_fs_get_inode_ref(&f->lwext4_file_struct.mp->fs,
                                           f->lwext4_file_struct.inode,
                                           &inode_ref);
        if (result == EOK)
        {
            memset(st, 0, sizeof(fs::Kstat));
            st->dev = 0;
            st->ino = inode_ref.index;
            st->mode = ext4_inode_get_mode(&f->lwext4_file_struct.mp->fs.sb, inode_ref.inode);
            st->nlink = ext4_inode_get_links_cnt(inode_ref.inode);

            uint32 raw_uid = ext4_inode_get_uid(inode_ref.inode);
            uint32 raw_gid = ext4_inode_get_gid(inode_ref.inode);
            st->uid = raw_uid > 65535 ? 0 : raw_uid;
            st->gid = raw_gid > 65535 ? 0 : raw_gid;
            st->rdev = ext4_inode_get_dev(inode_ref.inode);
            uint64 inode_size = ext4_inode_get_size(&f->lwext4_file_struct.mp->fs.sb, inode_ref.inode);
            st->size = inode_size;
            if (f->lwext4_file_struct.fsize > st->size)
            {
                st->size = f->lwext4_file_struct.fsize;
            }
            // 对已经打开的普通文件，内核 file 对象里维护的逻辑大小可能比 inode 更“新”：
            // 例如 stdio/fflush 刚经过 write 路径、但 ext4 侧元数据尚未被本次 fstat 看到时，
            // 直接信 inode 容易把 st_size 报成 0。这里取三者最大值，和 RV 行为保持一致。
            if (f->_stat.size > st->size)
            {
                st->size = f->_stat.size;
            }
            st->blksize = 4096;

            if (st->size == 0)
            {
                st->blocks = 0;
            }
            else
            {
                st->blocks = (st->size + 511) / 512;
                uint64 ext4_blocks_512 = ((uint64)inode_ref.inode->blocks_count_lo * 4096) / 512;
                if (ext4_blocks_512 > 0 && ext4_blocks_512 < st->blocks)
                {
                    st->blocks = ext4_blocks_512;
                }
                if (st->blocks == 0)
                {
                    st->blocks = 1;
                }
            }

            st->st_atime_sec = ext4_inode_get_access_time(inode_ref.inode);
            st->st_atime_nsec = (inode_ref.inode->atime_extra >> 2) & 0x3FFFFFFF;
            st->st_ctime_sec = ext4_inode_get_change_inode_time(inode_ref.inode);
            st->st_ctime_nsec = (inode_ref.inode->ctime_extra >> 2) & 0x3FFFFFFF;
            st->st_mtime_sec = ext4_inode_get_modif_time(inode_ref.inode);
            st->st_mtime_nsec = (inode_ref.inode->mtime_extra >> 2) & 0x3FFFFFFF;
            st->mnt_id = 0;
            ext4_fs_put_inode_ref(&inode_ref);

            if (Printer::trace_group_enabled())
            {
                tracef("[vfs_fstat] ext4-open-file path=%s ino=%u size=%u mode=0%o\n",
                       f->_path_name.c_str(), st->ino, st->size, st->mode);
            }
            return EOK;
        }
    }

    struct ext4_inode inode;
    uint32 inode_num = 0;
    const char *file_path = f->_path_name.c_str();
    int status = ext4_raw_inode_fill(file_path, &inode_num, &inode);
    if (status != EOK)
    {
        printfRed("vfs_fstat: ext4_raw_inode_fill failed for %s, error: %d\n", file_path, status);
        return -status;
    }
    struct ext4_sblock *sb = NULL;
    status = ext4_get_sblock(file_path, &sb);
    if (status != EOK)
        return -status;

    st->dev = 0;
    st->ino = inode_num;
    st->mode = ext4_inode_get_mode(sb, &inode);
    st->nlink = ext4_inode_get_links_cnt(&inode);

    // 获取原始 uid 和 gid，进行范围检查
    uint32_t raw_uid = ext4_inode_get_uid(&inode);
    uint32_t raw_gid = ext4_inode_get_gid(&inode);

    // 检查是否有异常值，如果有则使用默认值
    if (raw_uid > 65535)
    {                // 超出合理范围
        st->uid = 0; // 默认 root
        printfRed("vfs_fstat: invalid uid %u, using 0\n", raw_uid);
    }
    else
    {
        st->uid = raw_uid;
    }

    if (raw_gid > 65535)
    {                // 超出合理范围
        st->gid = 0; // 默认 root group
        printfRed("vfs_fstat: invalid gid %u, using 0\n", raw_gid);
    }
    else
    {
        st->gid = raw_gid;
    }

    st->rdev = ext4_inode_get_dev(&inode);
    st->size = ext4_inode_get_size(sb, &inode);
    if (Printer::trace_group_enabled())
        tracef("[vfs_fstat] ext4 path=%s size=%u mode=0%o\n",
               f->_path_name.c_str(), st->size, st->mode);
    // 修复 blksize 计算：避免除零错误，使用标准块大小
    st->blksize = 4096; // 使用标准 4KB 块大小

    // 修复 blocks 计算：根据文件大小计算所需的512字节块数
    // Linux stat 中的 blocks 字段表示分配给文件的512字节块数
    // 对于小文件，通常文件大小决定块数
    if (st->size == 0)
    {
        st->blocks = 0;
    }
    else
    {
        // 计算所需的512字节块数，向上取整
        st->blocks = (st->size + 511) / 512;

        // 但是要考虑文件系统的实际分配情况
        // 如果 ext4 报告的块数更小且合理，使用它
        uint64 ext4_blocks_512 = ((uint64)inode.blocks_count_lo * 4096) / 512;
        if (ext4_blocks_512 > 0 && ext4_blocks_512 < st->blocks)
        {
            st->blocks = ext4_blocks_512;
        }

        // 确保至少有1个块（对于非空文件）
        if (st->blocks == 0 && st->size > 0)
        {
            st->blocks = 1;
        }
    }

    st->st_atime_sec = ext4_inode_get_access_time(&inode);
    st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
    st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
    st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->mnt_id = 0;                                            // ext4暂时不支持挂载点ID
    return EOK;
}

int vfs_frename(const char *oldpath, const char *newpath)
{
    if (oldpath == nullptr || newpath == nullptr)
    {
        return -EFAULT;
    }

    eastl::string old_effective = normalize_path(oldpath);
    eastl::string new_effective = normalize_path(newpath);
    if (old_effective.empty() || new_effective.empty())
    {
        return -ENOENT;
    }

    int status = validate_linux_path_length(old_effective);
    if (status != EOK)
        return status;
    status = validate_linux_path_length(new_effective);
    if (status != EOK)
        return status;

    auto resolve_parent = [](const eastl::string &path, eastl::string &resolved) -> int
    {
        eastl::string parent = get_parent_path(path);
        size_t slash = path.find_last_of('/');
        eastl::string name = slash == eastl::string::npos ? path : path.substr(slash + 1);
        if (name.empty())
            return -ENOENT;

        eastl::string resolved_parent;
        int ret = resolve_symlinks(parent, resolved_parent);
        if (ret < 0)
            return ret;

        resolved = resolved_parent;
        if (resolved.empty())
            resolved = "/";
        if (resolved.back() != '/')
            resolved += "/";
        resolved += name;
        resolved = normalize_path(resolved);
        return validate_linux_path_length(resolved);
    };

    status = resolve_parent(old_effective, old_effective);
    if (status < 0)
        return status;
    status = resolve_parent(new_effective, new_effective);
    if (status < 0)
        return status;

    status = validate_lookup_prefix_permissions(old_effective);
    if (status < 0)
        return status;
    status = validate_lookup_prefix_permissions(new_effective);
    if (status < 0)
        return status;

    if (vfs_is_readonly_path(old_effective) || vfs_is_readonly_path(new_effective))
        return -EROFS;

    fs::Kstat old_stat{};
    status = vfs_path_stat(old_effective.c_str(), &old_stat, false);
    if (status < 0)
        return status;

    if (old_effective == new_effective)
        return 0;

    fs::Kstat new_stat{};
    int new_stat_ret = vfs_path_stat(new_effective.c_str(), &new_stat, false);
    if (new_stat_ret == 0 &&
        old_stat.dev == new_stat.dev &&
        old_stat.ino == new_stat.ino)
    {
        return 0;
    }
    if (new_stat_ret < 0 && new_stat_ret != -ENOENT)
        return new_stat_ret;

    if ((old_stat.mode & S_IFMT) == S_IFDIR &&
        new_effective.size() > old_effective.size() &&
        new_effective.compare(0, old_effective.size(), old_effective) == 0 &&
        new_effective[old_effective.size()] == '/')
    {
        return -EINVAL;
    }

    status = ext4_frename(old_effective.c_str(), new_effective.c_str());
    if (status != EOK)
        return -status;

    return -status;
}

int vfs_path_stat(const char *path, fs::Kstat *st, bool follow_symlinks)
{
    if (path == nullptr || st == nullptr)
    {
        return -EFAULT;
    }

    eastl::string effective_path(path);
    int length_ret = validate_linux_path_length(effective_path);
    if (length_ret != EOK)
    {
        printfRed("vfs_path_stat: path too long: len=%u\n", (uint32)effective_path.length());
        return length_ret;
    }

    if (follow_symlinks)
    {
        int resolve_ret = resolve_symlinks(effective_path, effective_path);
        if (resolve_ret < 0)
        {
            return resolve_ret;
        }
        length_ret = validate_linux_path_length(effective_path);
        if (length_ret != EOK)
        {
            printfRed("vfs_path_stat: resolved path too long: len=%u\n", (uint32)effective_path.length());
            return length_ret;
        }
    }
    else
    {
        // lstat/fstatat(AT_SYMLINK_NOFOLLOW) 只是不跟最终组件，
        // 但路径前缀里的符号链接仍然必须按正常路径遍历规则解析。
        // 否则像 ".../test_eloop/..." 这样的前缀环会直接落进底层 ext4 路径打开逻辑，
        // 本该返回 ELOOP 的场景就可能退化成异常甚至 panic。
        size_t last_slash = effective_path.find_last_of('/');
        eastl::string parent_dir;
        eastl::string filename;

        if (last_slash == eastl::string::npos)
        {
            parent_dir = ".";
            filename = effective_path;
        }
        else if (last_slash == 0)
        {
            parent_dir = "/";
            if (effective_path.length() > 1)
                filename = effective_path.substr(1);
        }
        else
        {
            parent_dir = effective_path.substr(0, last_slash);
            filename = effective_path.substr(last_slash + 1);
        }

        eastl::string resolved_parent;
        int resolve_ret = resolve_symlinks(parent_dir, resolved_parent);
        if (resolve_ret < 0)
        {
            return resolve_ret;
        }

        if (filename.empty())
        {
            effective_path = resolved_parent;
        }
        else
        {
            effective_path = resolved_parent;
            if (effective_path.empty())
                effective_path = ".";
            if (effective_path.back() != '/')
                effective_path += "/";
            effective_path += filename;
        }

        length_ret = validate_linux_path_length(effective_path);
        if (length_ret != EOK)
        {
            printfRed("vfs_path_stat: resolved path too long: len=%u\n", (uint32)effective_path.length());
            return length_ret;
        }
    }

    int prefix_permission_ret = validate_lookup_prefix_permissions(effective_path);
    if (prefix_permission_ret < 0)
    {
        return prefix_permission_ret;
    }

    select_effective_backing_path(effective_path, effective_path, false);

    // path-based stat 必须看见当前进程/其他进程已经写入但仍留在写合并缓冲里的数据。
    // 典型场景是 LTP mmap01：write(fd) 后立刻 stat(path)，若不刷新会把 st_size 误报为 0。
    int flush_ret = proc::k_pm.flush_open_files_for_path(effective_path);
    if (flush_ret < 0)
    {
        return flush_ret;
    }

    int status = raw_vfs_path_stat(effective_path, st);
    if (status != EOK)
    {
        return status;
    }

    if (Printer::trace_group_enabled())
        tracef("[vfs_path_stat] path=%s mode=0%o size=%u follow_symlinks=%s\n",
               effective_path.c_str(), st->mode, st->size, follow_symlinks ? "true" : "false");

    return EOK;
}

int vfs_link(const char *oldpath, const char *newpath)
{
    if (oldpath == nullptr || newpath == nullptr)
    {
        return -EFAULT;
    }

    eastl::string old_path_str(oldpath);
    eastl::string new_path_str(newpath);
    if (old_path_str.empty() || new_path_str.empty())
    {
        return -ENOENT;
    }

    // 虚拟节点没有可持久化的硬链接目录项，不能继续下探到根 ext4。
    if (fs::k_vfs.get_virtual_node(oldpath) != nullptr)
    {
        return -EXDEV;
    }

    eastl::string new_parent_path = get_parent_path(new_path_str);
    filesystem_t *source_fs = get_fs_from_path(oldpath);
    filesystem_t *target_fs = get_fs_from_path(new_parent_path.c_str());
    if (source_fs == nullptr || target_fs == nullptr)
    {
        return -ENOENT;
    }
    if (source_fs != target_fs)
    {
        return -EXDEV;
    }
    if (vfs_is_readonly_path(new_path_str))
    {
        return -EROFS;
    }

    // hard link 默认不跟随最后一个符号链接组件。
    // 这样 oldpath 是符号链接本身时，行为才能和 Linux 保持一致。
    fs::Kstat source_st{};
    int source_stat_ret = vfs_path_stat(oldpath, &source_st, false);
    if (source_stat_ret < 0)
    {
        return source_stat_ret;
    }

    if ((source_st.mode & S_IFMT) == S_IFDIR)
    {
        printfRed("vfs_link: cannot create hard link to directory %s\n", oldpath);
        return -EPERM;
    }

    // 创建目录项前，父目录至少要同时具备 search/write 权限。
    // 这里仅在父目录可以可靠解析时提前拦截 EACCES，其余 ENOENT/ENOTDIR
    // 仍交给底层 ext4_flink() 保持 Linux 的 errno 细节。
    fs::Kstat new_parent_st{};
    int new_parent_ret = vfs_path_stat(new_parent_path.c_str(), &new_parent_st, true);
    if (new_parent_ret == EOK && (new_parent_st.mode & S_IFMT) == S_IFDIR)
    {
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr)
        {
            return -EFAULT;
        }

        int perm_ret = check_mode_bits_with_fsids(new_parent_st.mode,
                                                  new_parent_st.uid,
                                                  new_parent_st.gid,
                                                  current_proc->get_fsuid(),
                                                  current_proc->get_fsgid(),
                                                  W_OK | X_OK);
        if (perm_ret < 0)
        {
            return perm_ret;
        }
    }
    else if (new_parent_ret == -EACCES || new_parent_ret == -ELOOP || new_parent_ret == -ENAMETOOLONG)
    {
        return new_parent_ret;
    }

    // 使用 ext4_flink 创建硬链接
    int status = ext4_flink(oldpath, newpath);
    if (status != EOK)
    {
        printfRed("vfs_link: ext4_flink failed for %s -> %s, error: %d\n",
                  oldpath, newpath, status);
        return -status;
    }

    printfGreen("vfs_link: successfully created hard link %s -> %s\n", newpath, oldpath);
    return EOK;
}

int vfs_unlink_path(const char *path, bool remove_dir)
{
    if (path == nullptr)
    {
        return -EFAULT;
    }

    eastl::string absolute_path(path);
    if (absolute_path.empty())
    {
        return -ENOENT;
    }

    // 删除目录项前，先按 Linux 语义检查：
    // 1. 目标本身是否存在/类型是否匹配；
    // 2. 父目录是否具备 write+search 权限；
    // 3. sticky 目录下是否有权删除该条目。
    fs::Kstat target_st{};
    int target_stat_ret = vfs_path_stat(path, &target_st, false);
    if (target_stat_ret < 0)
    {
        return target_stat_ret;
    }

    if (remove_dir)
    {
        if ((target_st.mode & S_IFMT) != S_IFDIR)
        {
            return -ENOTDIR;
        }
    }
    else if ((target_st.mode & S_IFMT) == S_IFDIR)
    {
        return -EISDIR;
    }

    eastl::string parent_path = get_parent_path(absolute_path);
    fs::Kstat parent_st{};
    int parent_stat_ret = vfs_path_stat(parent_path.c_str(), &parent_st, true);
    if (parent_stat_ret < 0)
    {
        return parent_stat_ret;
    }

    if ((parent_st.mode & S_IFMT) != S_IFDIR)
    {
        return -ENOTDIR;
    }

    proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
    if (current_proc == nullptr)
    {
        return -EFAULT;
    }

    int parent_perm_ret = check_mode_bits_with_fsids(parent_st.mode,
                                                     parent_st.uid,
                                                     parent_st.gid,
                                                     current_proc->get_fsuid(),
                                                     current_proc->get_fsgid(),
                                                     W_OK | X_OK);
    if (parent_perm_ret < 0)
    {
        return parent_perm_ret;
    }

    if ((parent_st.mode & S_ISVTX) != 0 && current_proc->get_fsuid() != 0 &&
        current_proc->get_fsuid() != parent_st.uid &&
        current_proc->get_fsuid() != target_st.uid)
    {
        return -EPERM;
    }

    struct filesystem *fs = get_fs_from_path(path);
    if (fs && fs->type == FAT32)
    {
        const char *rel_path = path;
        if (strcmp(fs->path, "/") != 0)
        {
            size_t mplen = strlen(fs->path);
            if (strncmp(rel_path, fs->path, mplen) == 0)
            {
                if (rel_path[mplen] == '\0')
                    rel_path = "/";
                else if (rel_path[mplen] == '/')
                    rel_path += mplen;
            }
        }

        struct fat32_entry *ep = ename((char *)rel_path);
        if (!ep)
        {
            return -ENOENT;
        }

        if (remove_dir)
        {
            if (!(ep->attribute & ATTR_DIRECTORY))
            {
                eput(ep);
                return -ENOTDIR;
            }

            elock(ep);
            // 检查目录是否为空
            struct fat32_entry child = {};
            uint off = 0;
            int cnt = 0;
            bool nonempty = false;
            while (true)
            {
                memset(&child, 0, sizeof(child));
                int type = enext(ep, &child, off, &cnt);
                if (type == -1)
                    break;
                if (type == 1)
                {
                    nonempty = true;
                    break;
                }
                off += (cnt ? cnt : 1) << 5;
            }
            if (nonempty)
            {
                eunlock(ep);
                eput(ep);
                return -ENOTEMPTY;
            }
            eremove(ep);
            eunlock(ep);
            eput(ep);
            return 0;
        }

        if (ep->attribute & ATTR_DIRECTORY)
        {
            eput(ep);
            return -EISDIR;
        }

        if (ep->parent)
            elock(ep->parent);
        elock(ep);
        etrunc(ep);
        eremove(ep);
        eunlock(ep);
        if (ep->parent)
            eunlock(ep->parent);
        eput(ep);
        return 0;
    }

    if (remove_dir)
    {
        return vfs_ext_rmdir(path);
    }
    return vfs_ext_unlink(path);
}

int vfs_truncate(fs::file *f, size_t length)
{
    if (f == nullptr)
    {
        printfRed("vfs_truncate: file is null\n");
        return -EINVAL;
    }

    // memfd 的大小在多个 open file description 之间共享。
    // 进入真正的 ext4 截断逻辑前，先把当前 file object 的本地缓存 size
    // 与共享状态对齐，避免 /proc/self/fd 重开后的跨 fd truncate 导致旧缓存继续生效。
    int visibility_status = f->flush_visibility_state();
    if (visibility_status != 0)
    {
        return visibility_status;
    }
    f->sync_file_size_from_memfd();

    // 获取当前文件大小。这里不能只信 file object 里的 fsize 缓存：
    // ftruncate/fallocate 可能在同一 inode 的不同 fd 之间交错执行，缓存一旦旧于
    // inode，truncate 与后续读写就会围绕错误的 EOF 做决策。
    uint64_t current_size = ext4_fsize(&f->lwext4_file_struct);
    if (f->lwext4_file_struct.mp != nullptr && f->lwext4_file_struct.inode > 0)
    {
        struct ext4_inode_ref inode_ref;
        int result = ext4_fs_get_inode_ref(&f->lwext4_file_struct.mp->fs,
                                           f->lwext4_file_struct.inode,
                                           &inode_ref);
        if (result == EOK)
        {
            current_size = ext4_inode_get_size(&f->lwext4_file_struct.mp->fs.sb, inode_ref.inode);
            f->lwext4_file_struct.fsize = current_size;
            f->_stat.size = current_size;
            ext4_fs_put_inode_ref(&inode_ref);
        }
    }

    // 如果新大小等于当前大小，无需操作
    if (length == current_size)
    {
        f->_stat.size = current_size;
        f->invalidate_cached_file_data();
        f->sync_memfd_size_from_file();
        return EOK;
    }

    /*
     * 普通文件 truncate 的权威语义交给 lwext4：
     * - 收缩释放超出范围的数据块；
     * - 扩展只更新 inode size，形成稀疏洞，读路径再把洞读成 0。
     * 这里不能通过写零模拟扩展，否则 iperf 这类频繁 ftruncate(128KiB)
     * 的程序会把截断语义绑定到块分配和 I/O 成功，甚至在 EOF 缓存不同步时
     * 触发底层写路径越界读。
     */
    if (length != current_size)
    {
        int status = ext4_ftruncate(&f->lwext4_file_struct, length);
        if (status != EOK)
        {
            printfRed("vfs_truncate: failed to truncate file %s to size %zu, error: %d\n",
                      f->_path_name.c_str(), length, status);
            return -status;
        }
        f->_stat.size = length;
        f->lwext4_file_struct.fsize = length;
        f->invalidate_cached_file_data();
        f->sync_memfd_size_from_file();
        return EOK;
    }
    return EOK;
}
int vfs_chmod(eastl::string pathname, mode_t mode)
{
    if (vfs_is_readonly_path(pathname))
    {
        return -EROFS;
    }

    if (vfs_is_file_exist(pathname.c_str()) != 1)
    {
        printfRed("[vfs_chmod] 文件不存在: %s\n", pathname.c_str());
        return -ENOENT; // 文件不存在
    }

    // 调用ext4的模式设置函数
    int status = ext4_mode_set(pathname.c_str(), mode);
    if (status != EOK)
    {
        printfRed("[vfs_chmod] 设置文件权限失败: %s, 错误码: %d\n", pathname.c_str(), status);
        return -EACCES; // 访问被拒绝
    }

    return EOK;
}

int vfs_fallocate(fs::file *f, int mode, off_t offset, size_t length)
{
    if (f == nullptr)
    {
        printfRed("vfs_fallocate: file is null\n");
        return -EINVAL;
    }

    constexpr int kFallocKeepSize = 0x01;
    constexpr int kFallocPunchHole = 0x02;
    constexpr int kFallocCollapseRange = 0x08;
    constexpr int kFallocZeroRange = 0x10;
    constexpr int kFallocInsertRange = 0x20;
    constexpr int kFallocUnshareRange = 0x40;
    constexpr int kFallocSupported =
        kFallocKeepSize | kFallocPunchHole | kFallocCollapseRange |
        kFallocZeroRange | kFallocInsertRange | kFallocUnshareRange;

    // 与 vfs_truncate 同理，memfd 进入 size-sensitive 逻辑前先同步共享大小。
    f->sync_file_size_from_memfd();

    // 检查参数合法性
    if (offset < 0 || length <= 0)
    {
        printfRed("vfs_fallocate: invalid offset or length\n");
        return -EINVAL;
    }
    if ((mode & ~kFallocSupported) != 0)
    {
        printfRed("vfs_fallocate: unsupported mode flags: %d\n", mode);
        return -ENOTSUP;
    }

    // 获取当前文件大小
    uint64_t current_size = ext4_fsize(&f->lwext4_file_struct);
    uint64_t target_size = offset + length;
    if (target_size > EXT4_MAX_FILE_SIZE)
    {
        printfRed("vfs_fallocate: target size exceeds maximum file size\n");
        return -EFBIG; // 文件过大
    }

    // FALLOC_FL_KEEP_SIZE 只预留空间，不改变 st_size。当前 ext4 适配层没有独立的
    // 预分配块接口；保留稀疏文件语义并返回成功，后续写入仍会按普通写扩展文件。
    if ((mode & kFallocKeepSize) != 0)
    {
        return EOK;
    }

    // 如果目标大小小于等于当前大小，不需要分配空间
    if (target_size <= current_size)
    {
        return EOK;
    }

    // 使用 ext4_ftruncate 来扩展文件大小
    // 这会自动分配必要的磁盘块
    int status = vfs_truncate(f, target_size);
    if (status != EOK)
    {
        printfRed("vfs_fallocate: failed to allocate space for file %s, error: %d\n",
                  f->_path_name.c_str(), status);
        return status;
    }

    // 更新文件大小信息
    f->_stat.size = target_size;
    f->sync_memfd_size_from_file();

    printfGreen("vfs_fallocate: successfully allocated space for file %s, new size: %u\n",
                f->_path_name.c_str(), target_size);

    return EOK;
}

int vfs_free_file(fs::file *file)
{
    ///@todo 锁
    delete file;
    return 0;
}

int vfs_chown(const eastl::string &pathname, int owner, int group, bool follow_symlinks)
{
    if (vfs_is_readonly_path(pathname))
    {
        return -EROFS;
    }

    // Basic existence check
    if (vfs_is_file_exist(pathname.c_str()) != 1)
    {
        return -ENOENT;
    }

    // Symlink handling: if not following, ensure the path is a symlink and operate on it directly.
    // Our ext4_owner_set operates on the path's inode. vfs_openat already has a resolver,
    // here we emulate lchown by avoiding resolving final symlink.
    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        // Resolve symlinks to the final target
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
        {
            target_path = resolved;
        }
        else if (r < 0)
        {
            return r; // e.g., -ELOOP
        }
    }

    // Handle "-1 means unchanged" per POSIX: read current owner/group if needed
    uint32_t cur_uid = 0, cur_gid = 0;
    int gstat = ext4_owner_get(target_path.c_str(), &cur_uid, &cur_gid);
    if (gstat != EOK)
    {
        // If we can't read current owner/group, propagate as access error
        return -EACCES;
    }

    uint32_t new_uid = (owner < 0) ? cur_uid : (uint32_t)owner;
    uint32_t new_gid = (group < 0) ? cur_gid : (uint32_t)group;

    int status = ext4_owner_set(target_path.c_str(), new_uid, new_gid);
    if (status != EOK)
    {
        // Map typical errors
        if (status == ENOENT)
            return -ENOENT;
        if (status == EROFS)
            return -EROFS;
        return -EACCES;
    }
    return 0;
}

int vfs_owner_get(const eastl::string &pathname, uint32_t &uid, uint32_t &gid, bool follow_symlinks)
{
    if (vfs_is_file_exist(pathname.c_str()) != 1)
        return -ENOENT;

    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
            target_path = resolved;
        else if (r < 0)
            return r;
    }
    uint32_t u = 0, g = 0;
    int s = ext4_owner_get(target_path.c_str(), &u, &g);
    if (s != EOK)
    {
        if (s == ENOENT)
            return -ENOENT;
        return -EACCES;
    }
    uid = u;
    gid = g;
    return 0;
}

int vfs_mode_get(const eastl::string &pathname, uint32_t &mode, bool follow_symlinks)
{
    if (vfs_is_file_exist(pathname.c_str()) != 1)
        return -ENOENT;
    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
            target_path = resolved;
        else if (r < 0)
            return r;
    }
    uint32_t m = 0;
    int s = ext4_mode_get(target_path.c_str(), &m);
    if (s != EOK)
    {
        if (s == ENOENT)
            return -ENOENT;
        return -EACCES;
    }
    mode = m;
    return 0;
}

int vfs_mode_set(const eastl::string &pathname, uint32_t mode, bool follow_symlinks)
{
    if (vfs_is_readonly_path(pathname))
        return -EROFS;
    if (vfs_is_file_exist(pathname.c_str()) != 1)
        return -ENOENT;
    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
            target_path = resolved;
        else if (r < 0)
            return r;
    }
    int s = ext4_mode_set(target_path.c_str(), mode);
    if (s != EOK)
    {
        if (s == ENOENT)
            return -ENOENT;
        if (s == EROFS)
            return -EROFS;
        return -EACCES;
    }
    return 0;
}

int vfs_register_mount(const eastl::string &mount_path, bool read_only)
{
    if (mount_path.empty())
        return -EINVAL;

    eastl::string normalized_path = normalize_path(mount_path);
    if (normalized_path.empty())
        normalized_path = "/";

    /*
     * 新文件系统挂载是独立的私有挂载对象。即使目标已有挂载，也必须压入
     * 新的一层，后续 umount 才能逐层露出原有视图。
     */
    const MountOverride *target_parent = find_mount_override(normalized_path);
    uint64 target_parent_id =
        target_parent == nullptr ? 0 : target_parent->mount_id;
    return push_mount_override(normalized_path, normalized_path, read_only,
                               VFS_MOUNT_PRIVATE, 0, 0,
                               target_parent_id);
}

int vfs_register_bind_mount(const eastl::string &source_path, const eastl::string &mount_path,
                            bool read_only, bool recursive)
{
    if (source_path.empty() || mount_path.empty())
        return -EINVAL;

    uint64 origin_ns_id = current_mount_namespace_id();
    eastl::string normalized_source = normalize_path(source_path);
    eastl::string normalized_target = normalize_path(mount_path);
    if (normalized_source.empty())
        normalized_source = "/";
    if (normalized_target.empty())
        normalized_target = "/";

    MountNamespaceState source_snapshot = current_mount_namespace_state();
    int source_mount_index = find_covering_mount_index_in(source_snapshot, normalized_source);
    const MountOverride *source_mount =
        source_mount_index < 0 ? nullptr :
        &source_snapshot.mounts[static_cast<size_t>(source_mount_index)];
    if (source_mount != nullptr && source_mount->propagation == VFS_MOUNT_UNBINDABLE)
    {
        // unbindable 挂载不能成为 bind/rbind 的根，避免创建无法保持传播约束的克隆。
        return -EINVAL;
    }

    eastl::string backing_source;
    resolve_bind_mount_path(normalized_source, backing_source);

    MountOverride target_parent_copy;
    const MountOverride *target_parent = find_mount_override(normalized_target);
    bool has_target_parent = target_parent != nullptr;
    if (has_target_parent)
    {
        target_parent_copy = *target_parent;
    }

    bool propagate_from_target_parent = false;
    eastl::string target_parent_path;
    int target_parent_peer_group = 0;
    eastl::vector<PropagationReceiver> target_parent_receivers;

    VfsMountPropagation propagation = VFS_MOUNT_PRIVATE;
    int peer_group = 0;
    int master_peer_group = 0;

    if (source_mount != nullptr)
    {
        propagation = source_mount->propagation;
        peer_group = source_mount->peer_group;
        master_peer_group = source_mount->master_peer_group;
    }

    if (has_target_parent && is_shared_mount(target_parent_copy))
    {
        /*
         * 私有或 slave 挂载附着到 shared 父挂载时，新挂载及其传播副本必须
         * 组成新的 peer group；slave 的上游关系仍然保留，形成 shared+slave。
         */
        if (propagation != VFS_MOUNT_SHARED || peer_group == 0)
        {
            propagation = VFS_MOUNT_SHARED;
            peer_group = allocate_mount_peer_group();
        }
        propagate_from_target_parent = true;
        target_parent_path = target_parent_copy.path;
        target_parent_peer_group = target_parent_copy.peer_group;
        /*
         * 接收树必须在新挂载接入前确定。rbind 可能复制出与目标父挂载
         * 同组的后代；若事后扫描，这些新副本会错误接收正在创建自己的
         * 同一次传播事件，造成树内指数式叠挂。
         */
        collect_propagation_receivers(origin_ns_id, target_parent_path,
                                      target_parent_peer_group,
                                      target_parent_receivers,
                                      target_parent_copy.mount_id);
    }

    if (propagation == VFS_MOUNT_SHARED && peer_group == 0)
    {
        peer_group = allocate_mount_peer_group();
    }

    bool effective_read_only = read_only ||
                               (source_mount != nullptr && source_mount->read_only);
    uint64 mounted_root_id = 0;
    int ret = push_mount_override(normalized_target, backing_source, effective_read_only,
                                  propagation, peer_group, master_peer_group,
                                  has_target_parent ? target_parent_copy.mount_id : 0,
                                  &mounted_root_id);
    if (ret < 0)
    {
        return ret;
    }

    eastl::vector<MountOverride> mounted_tree;
    eastl::vector<uint64> source_ids;
    eastl::vector<uint64> mounted_ids;
    MountOverride mounted_root;
    mounted_root.mount_id = mounted_root_id;
    mounted_root.parent_mount_id =
        has_target_parent ? target_parent_copy.mount_id : 0;
    mounted_root.path = normalized_target;
    mounted_root.source = backing_source;
    mounted_root.read_only = effective_read_only;
    mounted_root.propagation = propagation;
    mounted_root.peer_group = peer_group;
    mounted_root.master_peer_group = master_peer_group;
    mounted_tree.push_back(mounted_root);
    if (source_mount != nullptr)
    {
        source_ids.push_back(source_mount->mount_id);
        mounted_ids.push_back(mounted_root_id);
    }

    if (recursive)
    {
        eastl::vector<MountOverride> descendants;
        for (size_t i = 0; i < source_snapshot.mounts.size(); ++i)
        {
            const MountOverride &entry = source_snapshot.mounts[i];
            if (!path_matches_mount_prefix(entry.path, normalized_source) ||
                entry.path == normalized_source)
            {
                continue;
            }

            bool below_unbindable = false;
            for (size_t parent_index = 0;
                 parent_index < source_snapshot.mounts.size();
                 ++parent_index)
            {
                const MountOverride &candidate = source_snapshot.mounts[parent_index];
                if (candidate.propagation != VFS_MOUNT_UNBINDABLE ||
                    !path_matches_mount_prefix(candidate.path, normalized_source))
                {
                    continue;
                }
                if (path_matches_mount_prefix(entry.path, candidate.path))
                {
                    below_unbindable = true;
                    break;
                }
            }
            if (!below_unbindable)
            {
                descendants.push_back(entry);
            }
        }

        /*
         * mounts 按父层先于子层、下层先于栈顶的顺序记录。保持该顺序复制，
         * 才能让同一路径的多层挂载在目标处维持相同的父子和弹栈关系。
         */
        for (const auto &entry : descendants)
        {
            eastl::string suffix;
            suffix_after_mount_prefix(entry.path, normalized_source, suffix);
            eastl::string target_child;
            append_mount_suffix(normalized_target, suffix, target_child);

            /*
             * shared 目标会传播整棵接入树，因此树中每一层都必须拥有 peer
             * group。原有 shared 层继续加入源 peer group；private/slave 层
             * 获得新组，slave 的 master 关系保持不变。
             */
            MountOverride mounted_child = entry;
            mounted_child.path = target_child;
            uint64 mounted_parent_id = mounted_root_id;
            for (size_t mapping_index = 0;
                 mapping_index < source_ids.size();
                 ++mapping_index)
            {
                if (source_ids[mapping_index] == entry.parent_mount_id)
                {
                    mounted_parent_id = mounted_ids[mapping_index];
                    break;
                }
            }
            if (propagate_from_target_parent)
            {
                mounted_child.propagation = VFS_MOUNT_SHARED;
                if (mounted_child.peer_group == 0)
                {
                    mounted_child.peer_group = allocate_mount_peer_group();
                }
            }
            uint64 mounted_child_id = 0;
            ret = push_mount_override(target_child, mounted_child.source,
                                      mounted_child.read_only,
                                      mounted_child.propagation,
                                      mounted_child.peer_group,
                                      mounted_child.master_peer_group,
                                      mounted_parent_id,
                                      &mounted_child_id);
            if (ret < 0)
            {
                return ret;
            }
            mounted_child.mount_id = mounted_child_id;
            mounted_child.parent_mount_id = mounted_parent_id;
            mounted_tree.push_back(mounted_child);
            source_ids.push_back(entry.mount_id);
            mounted_ids.push_back(mounted_child_id);
        }
    }

    if (propagate_from_target_parent)
    {
        propagate_mount_tree_from_shared_parent(
            origin_ns_id, target_parent_path, target_parent_copy.source,
            target_parent_peer_group, mounted_tree, 0,
            &target_parent_receivers, target_parent_copy.mount_id);
    }

    return 0;
}

int vfs_move_mount(const eastl::string &source_path, const eastl::string &target_path)
{
    if (source_path.empty() || target_path.empty())
        return -EINVAL;

    uint64 origin_ns_id = current_mount_namespace_id();
    eastl::string normalized_source = normalize_path(source_path);
    eastl::string normalized_target = normalize_path(target_path);
    if (normalized_source.empty())
        normalized_source = "/";
    if (normalized_target.empty())
        normalized_target = "/";

    if (normalized_source == normalized_target ||
        path_matches_mount_prefix(normalized_target, normalized_source))
    {
        return -EINVAL;
    }

    MountNamespaceState &ns = current_mount_namespace_state();
    int source_root_index = find_exact_mount_index_in(ns, normalized_source);
    if (source_root_index < 0)
    {
        return -ENOENT;
    }

    MountOverride source_root = ns.mounts[static_cast<size_t>(source_root_index)];
    int source_parent_index =
        find_mount_index_by_id(ns, source_root.parent_mount_id);
    const MountOverride *source_parent =
        source_parent_index < 0 ? nullptr :
        &ns.mounts[static_cast<size_t>(source_parent_index)];
    if (source_parent != nullptr && is_shared_mount(*source_parent))
    {
        /*
         * shared 父挂载下的子挂载移动会让同一传播组内的层级关系变得不唯一，
         * Linux 语义要求这类 move 失败。
         */
        return -EINVAL;
    }

    MountOverride target_parent_copy;
    const MountOverride *target_parent = find_mount_override(normalized_target);
    bool has_target_parent = target_parent != nullptr;
    if (has_target_parent)
    {
        target_parent_copy = *target_parent;
    }
    bool propagate_from_target_parent =
        has_target_parent && is_shared_mount(target_parent_copy);
    eastl::string target_parent_path;
    int target_parent_peer_group = 0;
    eastl::vector<PropagationReceiver> target_parent_receivers;
    if (propagate_from_target_parent)
    {
        target_parent_path = target_parent_copy.path;
        target_parent_peer_group = target_parent_copy.peer_group;
        // move 接入目标树前固定接收者，避免移动树内部的 peer 重复接收本次事件。
        collect_propagation_receivers(origin_ns_id, target_parent_path,
                                      target_parent_peer_group,
                                      target_parent_receivers,
                                      target_parent_copy.mount_id);
    }
    int moved_peer_group = source_root.peer_group;
    bool source_root_shared = is_shared_mount(source_root);
    bool promote_moved_root_to_shared = propagate_from_target_parent && !source_root_shared;

    if (propagate_from_target_parent)
    {
        eastl::vector<uint64> checked_ids;
        checked_ids.push_back(source_root.mount_id);
        for (size_t i = static_cast<size_t>(source_root_index);
             i < ns.mounts.size(); ++i)
        {
            const MountOverride &entry = ns.mounts[i];
            bool in_moved_tree =
                entry.mount_id == source_root.mount_id ||
                eastl::find(checked_ids.begin(), checked_ids.end(),
                            entry.parent_mount_id) != checked_ids.end();
            if (!in_moved_tree)
            {
                continue;
            }
            checked_ids.push_back(entry.mount_id);
            if (entry.propagation == VFS_MOUNT_UNBINDABLE)
            {
                return -EINVAL;
            }
        }

        if (promote_moved_root_to_shared)
        {
            moved_peer_group = allocate_mount_peer_group();
        }
    }

    eastl::vector<MountOverride> moved_tree;
    eastl::vector<uint64> moved_ids;
    moved_ids.push_back(source_root.mount_id);
    for (size_t i = static_cast<size_t>(source_root_index);
         i < ns.mounts.size(); ++i)
    {
        const MountOverride &entry = ns.mounts[i];
        bool in_moved_tree =
            entry.mount_id == source_root.mount_id ||
            eastl::find(moved_ids.begin(), moved_ids.end(),
                        entry.parent_mount_id) != moved_ids.end();
        if (!in_moved_tree)
        {
            continue;
        }
        if (entry.mount_id != source_root.mount_id)
        {
            moved_ids.push_back(entry.mount_id);
        }

        MountOverride moved_entry = entry;
        eastl::string suffix;
        suffix_after_mount_prefix(entry.path, normalized_source, suffix);
        bool is_root_entry = entry.mount_id == source_root.mount_id;
        append_mount_suffix(normalized_target, suffix, moved_entry.path);
        if (entry.mount_id == source_root.mount_id)
        {
            moved_entry.parent_mount_id =
                has_target_parent ? target_parent_copy.mount_id : 0;
        }
        if (propagate_from_target_parent &&
            (moved_entry.propagation != VFS_MOUNT_SHARED ||
             moved_entry.peer_group == 0))
        {
            /*
             * Linux 在把挂载树移动到 shared 目标前会为整棵树分配传播组，
             * 因而所有层都能随目标传播树复制，而不仅是根挂载。
             */
            moved_entry.propagation = VFS_MOUNT_SHARED;
            moved_entry.peer_group = is_root_entry && promote_moved_root_to_shared
                                         ? moved_peer_group
                                         : allocate_mount_peer_group();
        }
        moved_tree.push_back(moved_entry);
    }

    if (propagate_from_target_parent)
    {
        for (auto &receiver : target_parent_receivers)
        {
            if (receiver.ns_id != origin_ns_id ||
                eastl::find(moved_ids.begin(), moved_ids.end(),
                            receiver.mount_id) == moved_ids.end())
            {
                continue;
            }

            /*
             * move 不会销毁源挂载对象。若该对象也是目标 shared 父挂载的
             * propagation peer，接收事件的位置必须随挂载树一起移动；
             * 继续使用快照中的旧路径会在已移空的位置制造孤立副本。
             */
            eastl::string receiver_suffix;
            suffix_after_mount_prefix(receiver.path, normalized_source,
                                      receiver_suffix);
            append_mount_suffix(normalized_target, receiver_suffix,
                                receiver.path);
        }
    }

    for (size_t i = ns.mounts.size(); i > static_cast<size_t>(source_root_index); --i)
    {
        size_t index = i - 1;
        if (eastl::find(moved_ids.begin(), moved_ids.end(),
                        ns.mounts[index].mount_id) != moved_ids.end())
        {
            ns.mounts.erase(ns.mounts.begin() + index);
        }
    }
    for (const auto &entry : moved_tree)
    {
        ns.mounts.push_back(entry);
    }

    if (propagate_from_target_parent)
    {
        propagate_mount_tree_from_shared_parent(
            origin_ns_id, target_parent_path, target_parent_copy.source,
            target_parent_peer_group, moved_tree, 0,
            &target_parent_receivers, target_parent_copy.mount_id);
    }

    return 0;
}

int vfs_set_mount_propagation(const eastl::string &mount_path, VfsMountPropagation propagation,
                              bool recursive)
{
    if (mount_path.empty())
        return -EINVAL;

    eastl::string normalized_path = normalize_path(mount_path);
    if (normalized_path.empty())
        normalized_path = "/";

    if (find_exact_mount_override(normalized_path) == nullptr)
    {
        // 传播属性属于挂载对象，普通目录必须先通过 bind 建立独立挂载。
        return -EINVAL;
    }

    MountNamespaceState &ns = current_mount_namespace_state();
    eastl::vector<size_t> visible_indices;
    for (size_t i = 0; i < ns.mounts.size(); ++i)
    {
        if (!is_visible_mount_in(ns, i))
        {
            continue;
        }
        if ((recursive && path_matches_mount_prefix(ns.mounts[i].path, normalized_path)) ||
            (!recursive && ns.mounts[i].path == normalized_path))
        {
            visible_indices.push_back(i);
        }
    }

    bool changed = false;
    for (size_t index : visible_indices)
    {
        MountOverride &entry = ns.mounts[index];
        entry.propagation = propagation;
        if (propagation == VFS_MOUNT_SHARED)
        {
            ensure_shared_peer_group(entry);
        }
        else if (propagation == VFS_MOUNT_SLAVE)
        {
            /*
             * shared 或 shared+slave 挂载离开 peer group 后，以刚离开的组
             * 作为直接 master。保留更上游的旧 master 会跳过中间传播层，
             * 破坏多级 slave 链。
             */
            if (entry.peer_group != 0)
            {
                entry.master_peer_group = entry.peer_group;
            }
            entry.peer_group = 0;
        }
        else if (propagation == VFS_MOUNT_PRIVATE ||
                 propagation == VFS_MOUNT_UNBINDABLE)
        {
            entry.peer_group = 0;
            entry.master_peer_group = 0;
        }
        changed = true;
    }

    return changed ? 0 : -ENOENT;
}

int vfs_unregister_mount(const eastl::string &mount_path)
{
    if (mount_path.empty())
        return -EINVAL;

    uint64 origin_ns_id = current_mount_namespace_id();
    eastl::string normalized_path = normalize_path(mount_path);
    if (normalized_path.empty())
        normalized_path = "/";

    MountNamespaceState &ns = current_mount_namespace_state();
    int target_index = find_exact_mount_index_in(ns, normalized_path);
    if (target_index < 0)
    {
        return -EINVAL;
    }

    MountOverride target_copy =
        ns.mounts[static_cast<size_t>(target_index)];
    int parent_index =
        find_mount_index_by_id(ns, target_copy.parent_mount_id);
    const MountOverride *parent =
        parent_index < 0 ? nullptr :
        &ns.mounts[static_cast<size_t>(parent_index)];
    MountOverride parent_copy;
    /*
     * 卸载是发生在父挂载 mountpoint 上的事件。只要父挂载 shared，
     * 子挂载无论是 private、slave 还是 shared 都要向父传播树同步。
     */
    bool propagate_from_parent =
        parent != nullptr && is_shared_mount(*parent);
    if (propagate_from_parent)
    {
        parent_copy = *parent;
        propagate_unmount_from_shared_parent(origin_ns_id, parent_copy.path,
                                             parent_copy.source,
                                             parent_copy.peer_group, normalized_path,
                                             0, parent_copy.mount_id);
    }

    target_index = find_mount_index_by_id(ns, target_copy.mount_id);
    if (target_index < 0)
    {
        /*
         * shared peer 可能位于自身子树中。传播到另一个 peer 时若卸载了
         * 包含原目标的祖先子树，原目标已经随传播成功移除，不应再报错。
         */
        return propagate_from_parent ? 0 : -EINVAL;
    }
    int erase_result =
        erase_mount_tree_at_index(ns, static_cast<size_t>(target_index)) ? 0 : -EINVAL;
    return erase_result;
}

bool vfs_is_readonly_path(const eastl::string &path)
{
    if (path.empty())
        return false;

    eastl::string normalized_path = normalize_path(path);
    if (normalized_path.empty())
        normalized_path = "/";

    const MountOverride *entry = find_mount_override(normalized_path);
    return entry != nullptr && entry->read_only;
}

void vfs_append_mounts_snapshot(eastl::string &result)
{
    for (const auto &entry : mount_overrides())
    {
        if (entry.source.empty())
        {
            continue;
        }

        /*
         * bind 的 backing 目录不是块设备名。若把目录写入首列，umount 等
         * 工具会按“设备”匹配所有等价 peer，导致一次命令卸载多个挂载。
         */
        filesystem_t *backing_fs = get_fs_from_path(entry.source.c_str());
        if (backing_fs != nullptr && backing_fs->type == FAT32)
            result += "/dev/data";
        else
            result += "/dev/root";
        result += " ";
        result += entry.path;
        result += ((backing_fs != nullptr && backing_fs->type == FAT32) ?
                   " vfat " : " ext4 ");
        result += entry.read_only ? "ro,bind" : "rw,bind";
        if (entry.propagation == VFS_MOUNT_SHARED)
            result += ",shared";
        else if (entry.propagation == VFS_MOUNT_SLAVE)
            result += ",slave";
        else if (entry.propagation == VFS_MOUNT_UNBINDABLE)
            result += ",unbindable";
        if (entry.master_peer_group != 0 && entry.propagation == VFS_MOUNT_SHARED)
            result += ",slave";
        result += " 0 0\n";
    }
}

uint64 vfs_clone_mount_namespace(uint64 source_ns_id)
{
    MountNamespaceState &source = ensure_mount_namespace_state(
        source_ns_id == 0 ? proc::k_initial_mount_namespace_id : source_ns_id);

    MountNamespaceState clone;
    clone.id = g_next_mount_namespace_id++;
    clone.mounts = source.mounts;
    eastl::vector<uint64> source_mount_ids;
    eastl::vector<uint64> clone_mount_ids;
    source_mount_ids.reserve(clone.mounts.size());
    clone_mount_ids.reserve(clone.mounts.size());
    for (auto &entry : clone.mounts)
    {
        source_mount_ids.push_back(entry.mount_id);
        entry.mount_id = allocate_mount_id();
        clone_mount_ids.push_back(entry.mount_id);
    }
    for (auto &entry : clone.mounts)
    {
        if (entry.parent_mount_id == 0)
        {
            continue;
        }
        for (size_t i = 0; i < source_mount_ids.size(); ++i)
        {
            if (source_mount_ids[i] == entry.parent_mount_id)
            {
                entry.parent_mount_id = clone_mount_ids[i];
                break;
            }
        }
    }
    clone.next_peer_group = source.next_peer_group;
    clone.refcnt = 1;
    g_mount_namespaces.push_back(clone);
    return clone.id;
}

void vfs_hold_mount_namespace(uint64 ns_id)
{
    MountNamespaceState &ns = ensure_mount_namespace_state(
        ns_id == 0 ? proc::k_initial_mount_namespace_id : ns_id);
    ++ns.refcnt;
}

void vfs_put_mount_namespace(uint64 ns_id)
{
    if (ns_id == 0)
    {
        return;
    }

    for (auto it = g_mount_namespaces.begin(); it != g_mount_namespaces.end(); ++it)
    {
        if (it->id != ns_id)
        {
            continue;
        }

        if (it->refcnt > 0)
        {
            --it->refcnt;
        }
        if (it->refcnt == 0 && it->id != proc::k_initial_mount_namespace_id)
        {
            g_mount_namespaces.erase(it);
        }
        return;
    }
}

bool vfs_mount_namespace_exists(uint64 ns_id)
{
    return find_mount_namespace_state(ns_id == 0 ? proc::k_initial_mount_namespace_id : ns_id) != nullptr;
}

bool is_lock_conflict(const struct flock &existing_lock, const struct flock &new_lock)
{
    printfCyan("[is_lock_conflict] Existing: type=%d, start=%ld, len=%ld, pid=%d\n",
               existing_lock.l_type, existing_lock.l_start, existing_lock.l_len, existing_lock.l_pid);
    printfCyan("[is_lock_conflict] New: type=%d, start=%ld, len=%ld, pid=%d\n",
               new_lock.l_type, new_lock.l_start, new_lock.l_len, new_lock.l_pid);

    // 如果现有锁类型是F_UNLCK或目标锁类型是解锁（F_UNLCK），就不需要检测冲突
    if (existing_lock.l_type == 2 || new_lock.l_type == 2) // F_UNLCK = 2
    {
        printfCyan("[is_lock_conflict] One lock is F_UNLCK, no conflict\n");
        return false;
    }

    // 判断锁的范围是否重叠
    off_t start1 = existing_lock.l_start;
    off_t end1 = (existing_lock.l_len == 0) ? LONG_MAX : existing_lock.l_start + existing_lock.l_len;
    off_t start2 = new_lock.l_start;
    off_t end2 = (new_lock.l_len == 0) ? LONG_MAX : new_lock.l_start + new_lock.l_len;

    printfCyan("[is_lock_conflict] Range check: [%ld,%ld] vs [%ld,%ld]\n", start1, end1, start2, end2);

    // 如果锁的范围没有交集，直接返回不冲突
    if (end1 <= start2 || end2 <= start1)
    {
        printfCyan("[is_lock_conflict] No range overlap, no conflict\n");
        return false;
    }

    // 如果是同一个进程的锁，不冲突（进程可以修改自己的锁）
    if (existing_lock.l_pid == new_lock.l_pid && existing_lock.l_pid != 0)
    {
        printfCyan("[is_lock_conflict] Same process, no conflict\n");
        return false;
    }

    // 检查锁类型是否冲突
    if (existing_lock.l_type == 1 || new_lock.l_type == 1) // F_WRLCK = 1
    {
        printfCyan("[is_lock_conflict] Write lock involved, conflict!\n");
        return true; // 写锁和任何锁都冲突
    }
    if (existing_lock.l_type == 0 && new_lock.l_type == 0) // F_RDLCK = 0
    {
        printfCyan("[is_lock_conflict] Both read locks, no conflict\n");
        return false; // 读锁之间不冲突
    }

    printfCyan("[is_lock_conflict] Other case, conflict!\n");
    return true; // 其他情况下有冲突
}

// 检查文件锁是否允许指定的读写操作
bool check_file_lock_access(const struct flock &file_lock, off_t offset, size_t size, bool is_write)
{
    // 如果文件没有锁，允许任何操作
    if (file_lock.l_type == F_UNLCK)
        return true;

    // 计算操作的范围
    off_t op_start = offset;
    off_t op_end = offset + size;

    // 计算锁的范围
    off_t lock_start = file_lock.l_start;
    off_t lock_end = (file_lock.l_len == 0) ? LONG_MAX : file_lock.l_start + file_lock.l_len;

    // 如果操作范围与锁范围没有交集，允许操作
    if (op_end <= lock_start || lock_end <= op_start)
        return true;

    // 如果有交集，检查锁类型
    if (file_lock.l_type == F_WRLCK)
        return false; // 写锁阻止任何操作

    if (file_lock.l_type == F_RDLCK && is_write)
        return false; // 读锁阻止写操作

    return true; // 读锁允许读操作
}

int vfs_write_file(const char *path, uint64 buffer_addr, size_t offset, size_t size)
{
    eastl::string normalized_path = normalize_path(path);
    eastl::string effective_path;
    resolve_bind_mount_path(normalized_path, effective_path);
    path = effective_path.c_str();

    // 检查文件是否存在
    if (vfs_is_file_exist(path) != 1)
    {
        printfRed("File does not exist: %s\n", path);
        return -ENOENT;
    }

    struct filesystem *fs = get_fs_from_path(path);
    if (fs && fs->type == FAT32) {
         const char* rel_path = path;
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         struct fat32_entry *ep = ename((char*)rel_path);
         if (!ep) return -ENOENT;
         
         elock(ep);
         int n = ewrite(ep, 0, buffer_addr, offset, size);
         eunlock(ep);
         eput(ep);
         return n;
    }

    int res;
    ext4_file file;

    // 打开文件（读写模式）
    res = ext4_fopen(&file, path, "r+b");
    if (res != EOK)
    {
        printfRed("Failed to open file for writing: %d\n", res);
        return -res;
    }

    // 如果有偏移，设置文件指针位置
    if (offset > 0)
    {
        res = ext4_fseek(&file, offset, SEEK_SET);
        if (res != EOK)
        {
            printfRed("Failed to seek file: %d\n", res);
            ext4_fclose(&file);
            return -res;
        }
    }

    // 写入数据
    size_t bytes_written;
    res = ext4_fwrite(&file, (void *)buffer_addr, size, &bytes_written);
    if (res != EOK)
    {
        printfRed("Failed to write file: %d\n", res);
        ext4_fclose(&file);
        return -res;
    }

    // 关闭文件
    res = ext4_fclose(&file);
    if (res != EOK)
    {
        printfRed("Failed to close file: %d\n", res);
        return -res;
    }

    // 返回实际写入的字节数
    return bytes_written;
}

// ====================== xattr VFS wrappers ==========================

static inline int map_ext4_err_to_sys(int r)
{
    // ext4.cc returns EOK on success and standard errno values otherwise.
    if (r == EOK)
        return 0;
    // Already errno-like, return negative for syscall path callers
    return -r;
}

int vfs_setxattr(const eastl::string &pathname, const char *name, const void *data,
                 size_t size, int flags, bool follow_symlinks)
{
    constexpr int k_xattr_create = 0x1;
    constexpr int k_xattr_replace = 0x2;

    if (!name)
        return -EINVAL;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }

    size_t existing_size = 0;
    int query_ret = ext4_getxattr(target.c_str(), name, strlen(name), nullptr, 0, &existing_size);
    bool exists = query_ret == EOK;
    if (query_ret != EOK && query_ret != ENODATA)
        return map_ext4_err_to_sys(query_ret);
    if ((flags & k_xattr_create) && exists)
        return -EEXIST;
    if ((flags & k_xattr_replace) && !exists)
        return -ENODATA;

    int r = ext4_setxattr(target.c_str(), name, strlen(name), data, size);
    return map_ext4_err_to_sys(r);
}

int vfs_getxattr(const eastl::string &pathname, const char *name, void *buf, size_t buf_size, size_t &out_size, bool follow_symlinks)
{
    out_size = 0;
    if (!name)
        return -EINVAL;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }
    size_t data_size = 0;
    int r = ext4_getxattr(target.c_str(), name, strlen(name), buf, buf_size, &data_size);
    if (r == EOK)
    {
        out_size = data_size;
        if (buf_size != 0 && data_size > buf_size)
        {
            return -ERANGE;
        }
    }
    return map_ext4_err_to_sys(r);
}

int vfs_listxattr(const eastl::string &pathname, char *list, size_t size, size_t &ret_size, bool follow_symlinks)
{
    ret_size = 0;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }
    size_t used = 0;
    int r = ext4_listxattr(target.c_str(), list, size, &used);
    if (r == EOK)
    {
        ret_size = used;
    }
    return map_ext4_err_to_sys(r);
}

int vfs_removexattr(const eastl::string &pathname, const char *name, bool follow_symlinks)
{
    if (!name)
        return -EINVAL;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }
    int r = ext4_removexattr(target.c_str(), name, strlen(name));
    return map_ext4_err_to_sys(r);
}

int vfs_fsetxattr(fs::file *f, const char *name, const void *data, size_t size, int flags)
{
    if (!f || !name)
    {
        printfRed("vfs_fsetxattr: invalid file or name\n");
        return -EINVAL;
    }
    if (f->_attrs.filetype == fs::FileTypes::FT_PIPE ||
        f->_attrs.filetype == fs::FileTypes::FT_DEVICE ||
        f->_attrs.filetype == fs::FileTypes::FT_SOCKET)
    {
        // Linux 的 user.* xattr 只允许普通文件和目录；特殊文件没有该属性。
        return -ENODATA;
    }
    return vfs_setxattr(f->_path_name, name, data, size, flags, /*follow_symlinks*/ true);
}

int vfs_fgetxattr(fs::file *f, const char *name, void *buf, size_t buf_size, size_t &out_size)
{
    if (!f || !name)
        return -EINVAL;
    if (f->_attrs.filetype == fs::FileTypes::FT_PIPE ||
        f->_attrs.filetype == fs::FileTypes::FT_DEVICE ||
        f->_attrs.filetype == fs::FileTypes::FT_SOCKET)
    {
        out_size = 0;
        // 特殊文件没有 user.* xattr，返回 ENODATA 而不是向 ext4 普通文件路径下探。
        return -ENODATA;
    }
    return vfs_getxattr(f->_path_name, name, buf, buf_size, out_size, /*follow_symlinks*/ true);
}

int vfs_flistxattr(fs::file *f, char *list, size_t size, size_t &ret_size)
{
    if (!f)
        return -EINVAL;
    return vfs_listxattr(f->_path_name, list, size, ret_size, /*follow_symlinks*/ true);
}

int vfs_fremovexattr(fs::file *f, const char *name)
{
    if (!f || !name)
        return -EINVAL;
    return vfs_removexattr(f->_path_name, name, /*follow_symlinks*/ true);
}
