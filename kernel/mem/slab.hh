#pragma once

#include "types.hh"
#include "param.h"
#include "libs/list.hh"  // 包含我们自定义的链表实现

namespace mem {

class Slab {
public:
    void* pa_start;
    uint64 free_objs_count;
    uint64 max_objs_count;
    LinkedList<Slab*> list;  // 替换为自定义链表
    uint64 first_obj;
    bool is_empty;
    
    Slab() = default; 
    Slab(uint32 obj_size);
    void init_free_objects(uint32 obj_size);
    void* alloc();
    void free(void* obj);
};

class SlabCache {
private:
    static constexpr uint32 DEFAULT_MAX_FREE_SLABS_ALLOWED = 5;
    uint32 obj_size_;
    uint32 free_slabs_count_;
    LinkedList<Slab> free_slabs_;       //完全空闲的slab列表
    LinkedList<Slab> partial_slabs_;    //部分空闲的slab列表
    LinkedList<Slab> full_slabs_;        //完全满的slab列表

    Slab* create_slab();
    void destroy_slab(Slab* slab);
    void memory_recycle();

public:
    SlabCache(uint32 size);
    void* alloc();
    void dealloc(void* pa);
    uint32 get_obj_size() const { return obj_size_; }
};

class SlabAllocator {
private:
    static SlabCache* caches[5];
    
public:
    static void init();
    static void* alloc(size_t size);
    static void dealloc(void* p, size_t size);
    static int get_cache_index(uint64 size);
};

}