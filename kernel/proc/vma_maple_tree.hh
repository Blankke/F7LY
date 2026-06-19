#pragma once

#include "vm_area.hh"

namespace proc
{
    /**
     * @brief 面向非重叠 VMA 区间的 Maple Tree 风格索引。
     *
     * 设计目标参考 Linux Maple Tree：
     * 1. 按地址 O(logN) 查找覆盖某地址的 VMA；
     * 2. 支持顺序遍历相邻 VMA，用于 gap 搜索和区间打洞；
     * 3. 叶子节点链表化，避免频繁全表扫描。
     *
     * 这里实现的是适合 F7LY 内核的简化版本：
     * - 数据仍由外部 vma 槽位池持有，树只维护索引；
     * - 节点使用高扇出 B+Tree 形态，叶子按地址排序并链接；
     * - 只支持“不重叠区间”这一类场景，专门服务 VMA。
     */
    class VmaMapleTree
    {
    public:
        VmaMapleTree();
        ~VmaMapleTree();

        VmaMapleTree(const VmaMapleTree &) = delete;
        VmaMapleTree &operator=(const VmaMapleTree &) = delete;

        void clear();
        bool empty() const { return root_ == nullptr; }
        int size() const { return size_; }

        bool insert(vma *entry);
        void erase(vma *entry);
        void erase(vma *entry, uint64 key_hint);

        vma *find(uint64 addr);
        const vma *find(uint64 addr) const;

        vma *lower_bound(uint64 start);
        const vma *lower_bound(uint64 start) const;

        vma *prev_by_start(uint64 start);
        const vma *prev_by_start(uint64 start) const;

        vma *next(const vma *entry);
        const vma *next(const vma *entry) const;

        vma *prev(const vma *entry);
        const vma *prev(const vma *entry) const;

        bool has_conflict(uint64 start, uint64 end, const vma *ignore = nullptr) const;

        uint64 find_gap(uint64 start_hint,
                        uint64 min_addr,
                        uint64 max_addr,
                        uint64 size,
                        uint64 alignment) const;

        template <typename Fn>
        bool for_each(Fn &&fn)
        {
            return const_cast<const VmaMapleTree *>(this)->for_each(static_cast<Fn &&>(fn));
        }

        template <typename Fn>
        bool for_each(Fn &&fn) const
        {
            const LeafNode *leaf = head_;
            while (leaf != nullptr)
            {
                for (int i = 0; i < leaf->count; ++i)
                {
                    if (!fn(*leaf->entries[i]))
                    {
                        return false;
                    }
                }
                leaf = leaf->next;
            }
            return true;
        }

        template <typename Fn>
        bool for_each_in_range(uint64 start, uint64 end, Fn &&fn)
        {
            return const_cast<const VmaMapleTree *>(this)->for_each_in_range(start, end, static_cast<Fn &&>(fn));
        }

        template <typename Fn>
        bool for_each_in_range(uint64 start, uint64 end, Fn &&fn) const
        {
            if (end <= start)
            {
                return true;
            }

            const vma *entry = find(start);
            if (entry == nullptr)
            {
                entry = lower_bound(start);
            }

            while (entry != nullptr && entry->addr < end)
            {
                if (entry->overlaps(start, end) && !fn(*entry))
                {
                    return false;
                }
                entry = next(entry);
            }
            return true;
        }

    private:
        static constexpr int k_leaf_capacity = 12;
        static constexpr int k_internal_capacity = 12;

        struct Node
        {
            enum class Kind : uint8
            {
                Leaf,
                Internal,
            };

            explicit Node(Kind kind_value) : kind(kind_value), count(0), parent(nullptr) {}
            bool is_leaf() const { return kind == Kind::Leaf; }

            Kind kind;
            int count;
            Node *parent;
        };

        struct LeafNode final : Node
        {
            LeafNode();

            vma *entries[k_leaf_capacity + 1];
            LeafNode *prev;
            LeafNode *next;
        };

        struct InternalNode final : Node
        {
            InternalNode();

            uint64 keys[k_internal_capacity + 1];
            Node *children[k_internal_capacity + 1];
        };

        Node *root_;
        LeafNode *head_;
        LeafNode *tail_;
        int size_;

    private:
        static uint64 entry_key(const vma *entry) { return entry != nullptr ? entry->addr : 0; }
        static uint64 align_up(uint64 value, uint64 alignment);

        static int find_entry_slot(const LeafNode *leaf, uint64 key);
        static int upper_bound_slot(const LeafNode *leaf, uint64 key);
        static int child_slot(const InternalNode *node, uint64 key);
        static int find_child_index(const InternalNode *node, const Node *child);

        static uint64 first_key(const Node *node);
        static vma *last_entry_mut(LeafNode *leaf);
        static const vma *last_entry(const LeafNode *leaf);

        LeafNode *find_leaf(uint64 key);
        const LeafNode *find_leaf(uint64 key) const;

        LeafNode *locate_leaf_for_entry(const vma *entry, uint64 key_hint);
        const LeafNode *locate_leaf_for_entry(const vma *entry, uint64 key_hint) const;

        void refresh_keys(InternalNode *node);
        void refresh_ancestors(Node *node);

        void split_leaf(LeafNode *leaf);
        void split_internal(InternalNode *node);
        void insert_right_sibling(Node *left, Node *right);

        void unlink_leaf(LeafNode *leaf);
        void remove_child(Node *child);
        void compress_root();

        void destroy_subtree(Node *node);
    };
} // namespace proc
