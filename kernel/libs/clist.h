#pragma once

#include "types.hh"
#include "spinlock.hh"


// 给定一个结构体变量指针，算出结构体的首地址
// 一个给定变量偏移
#ifndef container_of
#define container_of(ptr, type, member)                                                                                \
    ({                                                                                                                 \
        const typeof(((type *) 0)->member) *__mptr = (ptr);                                                            \
        (type *) ((char *) __mptr - offsetof(type, member));                                                           \
    })
#endif

// 返回包含list_head父类型的结构体
/**
 * list_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 */
#define list_entry(ptr, type, member) container_of(ptr, type, member)


struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

typedef struct list_head list_head_t;

//将结点初始化为链表头
static inline void list_init(list_head_t *list) {
    list->next = list;
    list->prev = list;
}
//链表加 头插法
static inline void list_add(list_head_t *lnew, list_head_t *head) {
    head->next->prev = lnew;
    lnew->prev = head;
    lnew->next = head->next;
    head->next = lnew;
}
//移除出去当前的结点
static inline void list_del(list_head_t *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->prev = entry->next = NULL;
}

static inline void list_del_reinit(struct list_head *entry) {
    list_del(entry);
    list_init(entry);
}


//把节点从A Move to B
static inline void list_move(list_head_t *list, list_head_t *head) {
    list_del(list);
    list_add(list, head);
}

//判断链表是否空
static inline int list_empty(list_head_t *head) {
    return head->next == head;
}

//合并链表
static inline void list_splice(list_head_t *list, list_head_t *head) {
    list_head_t *first = list->next;
    list_head_t *last = list->prev;
    list_head_t *at = head->next;

    first->prev = head;
    head->next = first;

    last->next = at;
    at->prev = last;
}

// 遍历链表
/**
 * list_first_entry - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
/**
 * list_next_entry - get the next element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define list_next_entry(pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * list_last_entry - get the last element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
/**
 * list_prev_entry - get the prev element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define list_prev_entry(pos, member) list_entry((pos)->member.prev, typeof(*(pos)), member)

/**
 * list_for_each	-	iterate over a list
 * @pos:	the &struct list_head to use as a loop cursor.
 * @head:	the head for your list.
 */
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * list_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 */
#define list_entry(ptr, type, member) container_of(ptr, type, member)
/**
 * list_for_each_entry	-	iterate over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
#define list_for_each_entry(pos, head, member)                                                                         \
    for (pos = list_first_entry(head, typeof(*pos), member); &pos->member != (head); pos = list_next_entry(pos, member))

// given first
#define list_for_each_entry_given_first(pos, head_f, member, flag)                                                     \
    for (pos = head_f; flag || &pos->member != (&head_f->member); pos = list_next_entry(pos, member), flag = 0)

// 反向遍历链表
/**
 * list_for_each_entry_reverse - iterate backwards over list of given type.
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
#define list_for_each_entry_reverse(pos, head, member)                                                                 \
    for (pos = list_last_entry(head, typeof(*pos), member); &pos->member != (head); pos = list_prev_entry(pos, member))

// 正向安全遍历链表（遍历的同时删除节点）
/**
 * list_for_each_entry_safe - iterate over list of given type safe against removal of list entry
 * @pos:	the type * to use as a loop cursor.
 * @n:		another type * to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
#define list_for_each_entry_safe(pos, n, head, member)                                                                 \
    for (pos = list_first_entry(head, typeof(*pos), member), n = list_next_entry(pos, member); &pos->member != (head); \
         pos = n, n = list_next_entry(n, member))

// given first head
#define list_for_each_entry_safe_given_first(pos, n, head_f, member, flag)                                             \
    for (pos = head_f, n = list_next_entry(pos, member); flag || &pos->member != (&head_f->member);                    \
         pos = n, n = list_next_entry(n, member), flag = 0)

// 反向安全遍历链表（反向遍历的同时删除节点）
#define list_for_each_entry_safe_reverse(pos, n, head, member)                                                         \
    for (pos = list_last_entry(head, typeof(*pos), member), n = list_prev_entry(pos, member); &pos->member != (head);  \
         pos = n, n = list_prev_entry(n, member))

/**
 * list_for_each_entry_safe_condition - iterate over list of given type safe against removal of list entry
 * @pos:	the type to use as a loop cursor.
 * @n:		another type to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 * @condition: the condition to check
 * Note: the condition should be a boolean expression
 */
#define list_for_each_entry_safe_condition(pos, n, head, member, condition)                                            \
    for (pos = list_first_entry(head, typeof(*pos), member), n = list_next_entry(pos, member);                         \
         &pos->member != (head) && (condition); pos = n, n = list_next_entry(n, member))

