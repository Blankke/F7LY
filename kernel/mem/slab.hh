#pragma once

#include "types.hh"

#include "param.h"
#include <EASTL/list.h>

namespace mem{

class Slab {
public:
    void* pa_start;
    uint64 free_objs_count;
    uint64 max_objs_count;
    eastl::list<Slab*> list;
    uint64 first_obj;
    bool is_empty;

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
    eastl::list<Slab> free_slabs_;
    eastl::list<Slab> partial_slabs_;
    eastl::list<Slab> full_slabs_;

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