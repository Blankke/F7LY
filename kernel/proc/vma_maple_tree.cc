#include "vma_maple_tree.hh"

namespace proc
{
    VmaMapleTree::LeafNode::LeafNode() : Node(Node::Kind::Leaf), prev(nullptr), next(nullptr)
    {
        for (int i = 0; i < k_leaf_capacity + 1; ++i)
        {
            entries[i] = nullptr;
        }
    }

    VmaMapleTree::InternalNode::InternalNode() : Node(Node::Kind::Internal)
    {
        for (int i = 0; i < k_internal_capacity + 1; ++i)
        {
            keys[i] = 0;
            children[i] = nullptr;
        }
    }

    VmaMapleTree::VmaMapleTree() : root_(nullptr), head_(nullptr), tail_(nullptr), size_(0) {}

    VmaMapleTree::~VmaMapleTree()
    {
        clear();
    }

    void VmaMapleTree::clear()
    {
        destroy_subtree(root_);
        root_ = nullptr;
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    uint64 VmaMapleTree::align_up(uint64 value, uint64 alignment)
    {
        if (alignment == 0)
        {
            return value;
        }
        uint64 remainder = value % alignment;
        if (remainder == 0)
        {
            return value;
        }
        return value + (alignment - remainder);
    }

    int VmaMapleTree::find_entry_slot(const LeafNode *leaf, uint64 key)
    {
        if (leaf == nullptr)
        {
            return 0;
        }

        for (int i = 0; i < leaf->count; ++i)
        {
            const vma *entry = leaf->entries[i];
            if (entry == nullptr)
            {
                continue;
            }
            if (entry->addr >= key)
            {
                return i;
            }
        }
        return leaf->count;
    }

    int VmaMapleTree::upper_bound_slot(const LeafNode *leaf, uint64 key)
    {
        if (leaf == nullptr)
        {
            return 0;
        }

        for (int i = 0; i < leaf->count; ++i)
        {
            const vma *entry = leaf->entries[i];
            if (entry == nullptr)
            {
                continue;
            }
            if (entry->addr > key)
            {
                return i;
            }
        }
        return leaf->count;
    }

    int VmaMapleTree::child_slot(const InternalNode *node, uint64 key)
    {
        if (node == nullptr || node->count <= 0)
        {
            return 0;
        }

        int best = -1;
        for (int i = 0; i < node->count; ++i)
        {
            if (node->children[i] == nullptr)
            {
                continue;
            }
            if (node->keys[i] <= key)
            {
                best = i;
            }
            else
            {
                break;
            }
        }
        if (best >= 0)
        {
            return best;
        }
        for (int i = 0; i < node->count; ++i)
        {
            if (node->children[i] != nullptr)
            {
                return i;
            }
        }
        return 0;
    }

    int VmaMapleTree::find_child_index(const InternalNode *node, const Node *child)
    {
        if (node == nullptr || child == nullptr)
        {
            return -1;
        }

        for (int i = 0; i < node->count; ++i)
        {
            if (node->children[i] == child)
            {
                return i;
            }
        }
        return -1;
    }

    uint64 VmaMapleTree::first_key(const Node *node)
    {
        if (node == nullptr || node->count == 0)
        {
            return 0;
        }

        if (node->is_leaf())
        {
            const LeafNode *leaf = static_cast<const LeafNode *>(node);
            for (int i = 0; i < leaf->count; ++i)
            {
                if (leaf->entries[i] != nullptr)
                {
                    return leaf->entries[i]->addr;
                }
            }
            return 0;
        }

        const InternalNode *internal = static_cast<const InternalNode *>(node);
        for (int i = 0; i < internal->count; ++i)
        {
            if (internal->children[i] != nullptr)
            {
                return first_key(internal->children[i]);
            }
        }
        return 0;
    }

    vma *VmaMapleTree::last_entry_mut(LeafNode *leaf)
    {
        if (leaf == nullptr || leaf->count == 0)
        {
            return nullptr;
        }
        for (int i = leaf->count - 1; i >= 0; --i)
        {
            if (leaf->entries[i] != nullptr)
            {
                return leaf->entries[i];
            }
        }
        return nullptr;
    }

    const vma *VmaMapleTree::last_entry(const LeafNode *leaf)
    {
        if (leaf == nullptr || leaf->count == 0)
        {
            return nullptr;
        }
        for (int i = leaf->count - 1; i >= 0; --i)
        {
            if (leaf->entries[i] != nullptr)
            {
                return leaf->entries[i];
            }
        }
        return nullptr;
    }

    void VmaMapleTree::compact_leaf(LeafNode *leaf)
    {
        if (leaf == nullptr || leaf->count <= 0)
        {
            return;
        }

        int write = 0;
        for (int read = 0; read < leaf->count; ++read)
        {
            vma *entry = leaf->entries[read];
            if (entry == nullptr)
            {
                continue;
            }
            leaf->entries[write++] = entry;
        }
        for (int i = write; i < k_leaf_capacity + 1; ++i)
        {
            leaf->entries[i] = nullptr;
        }
        if (write < leaf->count)
        {
            size_ -= (leaf->count - write);
            if (size_ < 0)
            {
                size_ = 0;
            }
            leaf->count = write;
            refresh_ancestors(leaf);
        }
    }

    VmaMapleTree::LeafNode *VmaMapleTree::find_leaf(uint64 key)
    {
        return const_cast<LeafNode *>(static_cast<const VmaMapleTree *>(this)->find_leaf(key));
    }

    const VmaMapleTree::LeafNode *VmaMapleTree::find_leaf(uint64 key) const
    {
        if (root_ == nullptr)
        {
            return nullptr;
        }

        const Node *node = root_;
        while (!node->is_leaf())
        {
            const InternalNode *internal = static_cast<const InternalNode *>(node);
            int slot = child_slot(internal, key);
            node = (slot >= 0 && slot < internal->count) ? internal->children[slot] : nullptr;
            if (node == nullptr)
            {
                return nullptr;
            }
        }
        return static_cast<const LeafNode *>(node);
    }

    VmaMapleTree::LeafNode *VmaMapleTree::locate_leaf_for_entry(const vma *entry, uint64 key_hint)
    {
        return const_cast<LeafNode *>(static_cast<const VmaMapleTree *>(this)->locate_leaf_for_entry(entry, key_hint));
    }

    const VmaMapleTree::LeafNode *VmaMapleTree::locate_leaf_for_entry(const vma *entry, uint64 key_hint) const
    {
        if (entry == nullptr)
        {
            return nullptr;
        }

        const LeafNode *leaf = find_leaf(key_hint);
        if (leaf == nullptr)
        {
            return nullptr;
        }

        for (const LeafNode *candidate = leaf; candidate != nullptr; candidate = candidate->next)
        {
            for (int i = 0; i < candidate->count; ++i)
            {
                if (candidate->entries[i] == entry)
                {
                    return candidate;
                }
            }
            uint64 first = first_key(candidate);
            if (candidate->count == 0 || (first != 0 && first > key_hint))
            {
                break;
            }
        }

        for (const LeafNode *candidate = leaf->prev; candidate != nullptr; candidate = candidate->prev)
        {
            if (candidate->count == 0)
            {
                continue;
            }
            const vma *last = last_entry(candidate);
            if (last != nullptr && last->addr < key_hint)
            {
                break;
            }
            for (int i = 0; i < candidate->count; ++i)
            {
                if (candidate->entries[i] == entry)
                {
                    return candidate;
                }
            }
        }

        return nullptr;
    }

    void VmaMapleTree::refresh_keys(InternalNode *node)
    {
        if (node == nullptr)
        {
            return;
        }

        for (int i = 0; i < node->count; ++i)
        {
            node->keys[i] = first_key(node->children[i]);
        }
    }

    void VmaMapleTree::refresh_ancestors(Node *node)
    {
        for (Node *cursor = node != nullptr ? node->parent : nullptr; cursor != nullptr; cursor = cursor->parent)
        {
            refresh_keys(static_cast<InternalNode *>(cursor));
        }
    }

    bool VmaMapleTree::insert(vma *entry)
    {
        if (entry == nullptr || !entry->used || !entry->valid_range())
        {
            return false;
        }

        if (root_ == nullptr)
        {
            LeafNode *leaf = new LeafNode();
            leaf->entries[0] = entry;
            leaf->count = 1;
            root_ = leaf;
            head_ = leaf;
            tail_ = leaf;
            size_ = 1;
            return true;
        }

        LeafNode *leaf = find_leaf(entry->addr);
        if (leaf == nullptr)
        {
            return false;
        }
        compact_leaf(leaf);

        int slot = find_entry_slot(leaf, entry->addr);
        const vma *prev_entry = nullptr;
        const vma *next_entry = nullptr;
        if (slot > 0)
        {
            prev_entry = leaf->entries[slot - 1];
        }
        else if (leaf->prev != nullptr)
        {
            prev_entry = last_entry_mut(leaf->prev);
        }

        if (slot < leaf->count)
        {
            next_entry = leaf->entries[slot];
        }
        else if (leaf->next != nullptr && leaf->next->count > 0)
        {
            for (int i = 0; i < leaf->next->count; ++i)
            {
                if (leaf->next->entries[i] != nullptr)
                {
                    next_entry = leaf->next->entries[i];
                    break;
                }
            }
        }

        if ((prev_entry != nullptr && prev_entry->overlaps(*entry)) ||
            (next_entry != nullptr && next_entry->overlaps(*entry)))
        {
            return false;
        }

        for (int i = leaf->count; i > slot; --i)
        {
            leaf->entries[i] = leaf->entries[i - 1];
        }
        leaf->entries[slot] = entry;
        ++leaf->count;
        ++size_;

        if (leaf->count > k_leaf_capacity)
        {
            split_leaf(leaf);
        }
        else
        {
            refresh_ancestors(leaf);
        }
        return true;
    }

    void VmaMapleTree::split_leaf(LeafNode *leaf)
    {
        compact_leaf(leaf);
        LeafNode *right = new LeafNode();
        int left_count = leaf->count / 2;
        int right_count = leaf->count - left_count;

        for (int i = 0; i < right_count; ++i)
        {
            right->entries[i] = leaf->entries[left_count + i];
            leaf->entries[left_count + i] = nullptr;
        }

        leaf->count = left_count;
        right->count = right_count;

        right->next = leaf->next;
        right->prev = leaf;
        if (leaf->next != nullptr)
        {
            leaf->next->prev = right;
        }
        else
        {
            tail_ = right;
        }
        leaf->next = right;

        insert_right_sibling(leaf, right);
    }

    void VmaMapleTree::insert_right_sibling(Node *left, Node *right)
    {
        InternalNode *parent = left != nullptr ? static_cast<InternalNode *>(left->parent) : nullptr;
        right->parent = parent;

        if (parent == nullptr)
        {
            InternalNode *new_root = new InternalNode();
            new_root->children[0] = left;
            new_root->children[1] = right;
            new_root->count = 2;
            left->parent = new_root;
            right->parent = new_root;
            refresh_keys(new_root);
            root_ = new_root;
            return;
        }

        int left_index = find_child_index(parent, left);
        if (left_index < 0)
        {
            return;
        }

        for (int i = parent->count; i > left_index + 1; --i)
        {
            parent->children[i] = parent->children[i - 1];
        }
        parent->children[left_index + 1] = right;
        ++parent->count;
        refresh_keys(parent);

        if (parent->count > k_internal_capacity)
        {
            split_internal(parent);
        }
        else
        {
            refresh_ancestors(parent);
        }
    }

    void VmaMapleTree::split_internal(InternalNode *node)
    {
        InternalNode *right = new InternalNode();
        int left_count = node->count / 2;
        int right_count = node->count - left_count;

        for (int i = 0; i < right_count; ++i)
        {
            right->children[i] = node->children[left_count + i];
            if (right->children[i] != nullptr)
            {
                right->children[i]->parent = right;
            }
            node->children[left_count + i] = nullptr;
        }

        node->count = left_count;
        right->count = right_count;

        refresh_keys(node);
        refresh_keys(right);
        insert_right_sibling(node, right);
    }

    void VmaMapleTree::erase(vma *entry)
    {
        erase(entry, entry != nullptr ? entry->addr : 0);
    }

    void VmaMapleTree::erase(vma *entry, uint64 key_hint)
    {
        if (entry == nullptr || root_ == nullptr)
        {
            return;
        }

        LeafNode *leaf = locate_leaf_for_entry(entry, key_hint);
        if (leaf == nullptr)
        {
            return;
        }
        compact_leaf(leaf);

        int slot = -1;
        for (int i = 0; i < leaf->count; ++i)
        {
            if (leaf->entries[i] == entry)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
        {
            return;
        }

        for (int i = slot + 1; i < leaf->count; ++i)
        {
            leaf->entries[i - 1] = leaf->entries[i];
        }
        leaf->entries[leaf->count - 1] = nullptr;
        --leaf->count;
        --size_;

        if (leaf->count == 0)
        {
            unlink_leaf(leaf);
            remove_child(leaf);
            delete leaf;
        }
        else
        {
            refresh_ancestors(leaf);
        }
    }

    void VmaMapleTree::unlink_leaf(LeafNode *leaf)
    {
        if (leaf->prev != nullptr)
        {
            leaf->prev->next = leaf->next;
        }
        else
        {
            head_ = leaf->next;
        }

        if (leaf->next != nullptr)
        {
            leaf->next->prev = leaf->prev;
        }
        else
        {
            tail_ = leaf->prev;
        }
    }

    void VmaMapleTree::remove_child(Node *child)
    {
        if (child == nullptr)
        {
            return;
        }

        InternalNode *parent = static_cast<InternalNode *>(child->parent);
        if (parent == nullptr)
        {
            if (child == root_)
            {
                root_ = nullptr;
                head_ = nullptr;
                tail_ = nullptr;
            }
            return;
        }

        int slot = find_child_index(parent, child);
        if (slot < 0)
        {
            return;
        }

        for (int i = slot + 1; i < parent->count; ++i)
        {
            parent->children[i - 1] = parent->children[i];
        }
        parent->children[parent->count - 1] = nullptr;
        --parent->count;
        refresh_keys(parent);

        if (parent->count == 0)
        {
            remove_child(parent);
            delete parent;
            return;
        }

        if (parent == root_)
        {
            compress_root();
            return;
        }

        if (parent->count == 1)
        {
            Node *only_child = parent->children[0];
            InternalNode *grand = static_cast<InternalNode *>(parent->parent);
            int parent_slot = find_child_index(grand, parent);
            if (grand != nullptr && parent_slot >= 0)
            {
                grand->children[parent_slot] = only_child;
                only_child->parent = grand;
                refresh_keys(grand);
                delete parent;
                if (grand == root_)
                {
                    compress_root();
                }
                else
                {
                    refresh_ancestors(grand);
                }
            }
            return;
        }

        refresh_ancestors(parent);
    }

    void VmaMapleTree::compress_root()
    {
        while (root_ != nullptr && !root_->is_leaf())
        {
            InternalNode *internal = static_cast<InternalNode *>(root_);
            if (internal->count != 1)
            {
                refresh_keys(internal);
                return;
            }

            root_ = internal->children[0];
            if (root_ != nullptr)
            {
                root_->parent = nullptr;
            }
            delete internal;
        }
    }

    vma *VmaMapleTree::find(uint64 addr)
    {
        return const_cast<vma *>(static_cast<const VmaMapleTree *>(this)->find(addr));
    }

    const vma *VmaMapleTree::find(uint64 addr) const
    {
        const LeafNode *leaf = find_leaf(addr);
        if (leaf == nullptr)
        {
            return nullptr;
        }

        int slot = upper_bound_slot(leaf, addr) - 1;
        for (int i = slot; i >= 0; --i)
        {
            const vma *entry = leaf->entries[i];
            if (entry != nullptr && entry->covers(addr))
            {
                return entry;
            }
        }

        if (slot < 0 && leaf->prev != nullptr)
        {
            const vma *entry = last_entry(leaf->prev);
            if (entry != nullptr && entry->covers(addr))
            {
                return entry;
            }
        }

        return nullptr;
    }

    vma *VmaMapleTree::lower_bound(uint64 start)
    {
        return const_cast<vma *>(static_cast<const VmaMapleTree *>(this)->lower_bound(start));
    }

    const vma *VmaMapleTree::lower_bound(uint64 start) const
    {
        const LeafNode *leaf = find_leaf(start);
        if (leaf == nullptr)
        {
            return nullptr;
        }

        int slot = find_entry_slot(leaf, start);
        for (int i = slot; i < leaf->count; ++i)
        {
            if (leaf->entries[i] != nullptr)
            {
                return leaf->entries[i];
            }
        }
        if (leaf->next != nullptr && leaf->next->count > 0)
        {
            for (int i = 0; i < leaf->next->count; ++i)
            {
                if (leaf->next->entries[i] != nullptr)
                {
                    return leaf->next->entries[i];
                }
            }
        }
        return nullptr;
    }

    vma *VmaMapleTree::prev_by_start(uint64 start)
    {
        return const_cast<vma *>(static_cast<const VmaMapleTree *>(this)->prev_by_start(start));
    }

    const vma *VmaMapleTree::prev_by_start(uint64 start) const
    {
        const LeafNode *leaf = find_leaf(start);
        if (leaf == nullptr)
        {
            return nullptr;
        }

        int slot = find_entry_slot(leaf, start) - 1;
        for (int i = slot; i >= 0; --i)
        {
            if (leaf->entries[i] != nullptr)
            {
                return leaf->entries[i];
            }
        }
        return last_entry(leaf->prev);
    }

    vma *VmaMapleTree::next(const vma *entry)
    {
        return const_cast<vma *>(static_cast<const VmaMapleTree *>(this)->next(entry));
    }

    const vma *VmaMapleTree::next(const vma *entry) const
    {
        if (entry == nullptr)
        {
            return nullptr;
        }

        const LeafNode *leaf = locate_leaf_for_entry(entry, entry->addr);
        if (leaf == nullptr)
        {
            return nullptr;
        }

        for (int i = 0; i < leaf->count; ++i)
        {
            if (leaf->entries[i] != entry)
            {
                continue;
            }
            if (i + 1 < leaf->count)
            {
                for (int j = i + 1; j < leaf->count; ++j)
                {
                    if (leaf->entries[j] != nullptr)
                    {
                        return leaf->entries[j];
                    }
                }
            }
            if (leaf->next != nullptr && leaf->next->count > 0)
            {
                for (int j = 0; j < leaf->next->count; ++j)
                {
                    if (leaf->next->entries[j] != nullptr)
                    {
                        return leaf->next->entries[j];
                    }
                }
            }
            return nullptr;
        }
        return nullptr;
    }

    vma *VmaMapleTree::prev(const vma *entry)
    {
        return const_cast<vma *>(static_cast<const VmaMapleTree *>(this)->prev(entry));
    }

    const vma *VmaMapleTree::prev(const vma *entry) const
    {
        if (entry == nullptr)
        {
            return nullptr;
        }

        const LeafNode *leaf = locate_leaf_for_entry(entry, entry->addr);
        if (leaf == nullptr)
        {
            return nullptr;
        }

        for (int i = 0; i < leaf->count; ++i)
        {
            if (leaf->entries[i] != entry)
            {
                continue;
            }
            if (i > 0)
            {
                for (int j = i - 1; j >= 0; --j)
                {
                    if (leaf->entries[j] != nullptr)
                    {
                        return leaf->entries[j];
                    }
                }
            }
            return last_entry(leaf->prev);
        }
        return nullptr;
    }

    bool VmaMapleTree::has_conflict(uint64 start, uint64 end, const vma *ignore) const
    {
        if (end <= start)
        {
            return false;
        }

        const vma *entry = find(start);
        if (entry != nullptr && entry != ignore)
        {
            return true;
        }

        entry = lower_bound(start);
        return entry != nullptr && entry != ignore && entry->addr < end;
    }

    uint64 VmaMapleTree::find_gap(uint64 start_hint,
                                  uint64 min_addr,
                                  uint64 max_addr,
                                  uint64 size,
                                  uint64 alignment) const
    {
        if (size == 0 || max_addr <= min_addr)
        {
            return 0;
        }

        uint64 cursor = start_hint > min_addr ? start_hint : min_addr;
        cursor = align_up(cursor, alignment);
        if (cursor < min_addr)
        {
            return 0;
        }
        if (cursor > max_addr || size > max_addr - cursor)
        {
            return 0;
        }

        const vma *covering = find(cursor);
        if (covering != nullptr)
        {
            cursor = align_up(covering->end_addr(), alignment);
            if (cursor > max_addr || size > max_addr - cursor)
            {
                return 0;
            }
            covering = next(covering);
        }
        else
        {
            const vma *prev_entry = prev_by_start(cursor);
            if (prev_entry != nullptr && prev_entry->end_addr() > cursor)
            {
                cursor = align_up(prev_entry->end_addr(), alignment);
                if (cursor > max_addr || size > max_addr - cursor)
                {
                    return 0;
                }
            }
            covering = lower_bound(cursor);
        }

        while (covering != nullptr)
        {
            if (covering->addr > max_addr)
            {
                break;
            }

            if (cursor < covering->addr)
            {
                uint64 gap_end = covering->addr;
                if (cursor <= gap_end && size <= gap_end - cursor)
                {
                    return cursor;
                }
            }

            if (covering->end_addr() > cursor)
            {
                cursor = align_up(covering->end_addr(), alignment);
                if (cursor > max_addr || size > max_addr - cursor)
                {
                    return 0;
                }
            }

            covering = next(covering);
        }

        return size <= max_addr - cursor ? cursor : 0;
    }

    void VmaMapleTree::destroy_subtree(Node *node)
    {
        if (node == nullptr)
        {
            return;
        }

        if (!node->is_leaf())
        {
            InternalNode *internal = static_cast<InternalNode *>(node);
            for (int i = 0; i < internal->count; ++i)
            {
                destroy_subtree(internal->children[i]);
            }
            delete internal;
            return;
        }

        delete static_cast<LeafNode *>(node);
    }
} // namespace proc
