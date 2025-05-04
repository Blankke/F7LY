#pragma once



namespace mem {

class KAlloc {
public:
    static void init();
    static void* alloc(); //分配单个物理页
    static void free(void* pa);
    static void* kmalloc(uint64 size); //分配任意大小的内存块
    static void* kcalloc(uint n, uint64 size);

private:
    static uint64 pa_start;
    static class SpinLock memlock;

    static uint64 pa2pgnm(void* pa);
    static void* pgnm2pa(int pgnm);
    static int size_to_page_num(uint64 size);
};

}