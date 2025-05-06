

#include "param.h"
#include "types.hh"
#include "mem/memlayout.hh"
#include "platform.hh"
#include "klib.hh"
#include "printer.hh"
#include "kalloc.hh"
#include "vm.hh"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

#ifdef LOONGARCH
void
tlbinit(void)
{
  asm volatile("invtlb  0x0,$zero,$zero");
  w_csr_stlbps(0xcU);
  w_csr_asid(0x0U);
  w_csr_tlbrehi(0xcU);
}
#endif

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) mem::KAlloc::alloc();
  memset(kpgtbl, 0, PGSIZE);
#ifdef RISCV
  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  kvmmap(kpgtbl, VIRTIO1, VIRTIO1, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(kpgtbl, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  // wait for zzy :proc_mapstacks
  // proc_mapstacks(kpgtbl);
#elif defined(LOONGARCH)
  // kvmmap(kpgtbl, DMWIN1_MASK | (0xfe00000000), DMWIN1_MASK | (0xfe00000000), PGSIZE, PTE_MAT |PTE_W |PTE_P);
  kvmmap(kpgtbl, ((uint64)etext) & (~(DMWIN_MASK)), (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);
  proc_mapstacks(kpgtbl);

#endif
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  printf("kvminit\n");
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  printf("kvminithart\n");
#ifdef RISCV
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
#endif
#ifdef LOONGARCH
  w_csr_pgdl((uint64)kernel_pagetable);
  // flush the tlb(tlbinit)
  tlbinit();

  w_csr_pwcl((PTEWIDTH << 30)|(DIR2WIDTH << 25)|(DIR2BASE << 20)|(DIR1WIDTH << 15)|(DIR1BASE << 10)|(PTWIDTH << 5)|(PTBASE << 0));
  w_csr_pwch((DIR4WIDTH << 18)|(DIR3WIDTH << 6)|(DIR3BASE << 0) |(PWCH_HPTW_EN << 24));

  uint64 crmd = r_csr_crmd();

#endif

}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
#ifdef RISCV
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)mem::KAlloc::alloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
#elif defined(LOONGARCH)
  // if(va >= MAXVA)
  //   panic("walk");

  for(int level = 3; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)(PTE2PA(*pte) | DMWIN_MASK);
    } else {
      if(!alloc || (pagetable = (pde_t*)mem::KAlloc::alloc()) == 0) {
        return 0;
      }

      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
#endif
  return &pagetable[PX(0, va)];
}

pte_t *
walk_device(pagetable_t pagetable, uint64 va, int alloc) {
#ifdef LOONGARCH
  for(int level = 3; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)(PTE2PA(*pte) | DMWIN1_MASK);
    } else {
      if(!alloc || (pagetable = (pde_t*)mem::KAlloc::alloc()) == 0) {
        return 0;
      }

      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V | DMWIN1_MASK;
    }
  }
  return &pagetable[PX(0, va)];
#endif
return nullptr;
}

int
mappages_device(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, uint64 perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk_device(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

void pci_map(int bus, int dev, int func, void *pages) {
#ifdef LOONGARCH

  uint64 va = PCIE0_ECAM_V + ((bus << 16) | (dev << 11) | (func << 8));
  uint64 pa = PCIE0_ECAM + ((bus << 16) | (dev << 11) | (func << 8));
  mappages_device(kernel_pagetable, va, PGSIZE, pa, PTE_MAT | PTE_W | PTE_P | PTE_D);
  static int first = 0;
  if (!first) {
    va = PCIE0_MMIO_V;
    pa = PCIE0_MMIO;
    mappages(kernel_pagetable, va, 16 * PGSIZE, pa, PTE_MAT | PTE_W | PTE_P | PTE_D);
    first = 1;
  }


  // mappages(kernel_pagetable, ((uint64)pages) & (~(DMWIN_MASK)), 2 * PGSIZE, pages, PTE_W | PTE_P | PTE_D | PTE_MAT);

#endif
}

uint64
kwalkaddr(uint64 va)
{
  pagetable_t kpt = kernel_pagetable;
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kpt, va, 0);

  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}


// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
#ifdef RISCV
  pa = PTE2PA(*pte);
#elif defined(LOONGARCH)
  pa = PTE2PA(*pte) | DMWIN_MASK;
#endif
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, uint64 perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

void kernelmap(uint64 va, uint64 pa, uint64 sz, uint64 perm) {
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, uint64 perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");
  
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      continue;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
#ifdef RISCV
      uint64 pa = PTE2PA(*pte);
#elif defined(LOONGARCH)
      uint64 pa = PTE2PA(*pte) | DMWIN_MASK;
#endif
      mem::KAlloc::free((void*)pa);
    }
    *pte = 0;
  }

}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) mem::KAlloc::alloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
#ifdef RISCV
  char *mem;
  printf("sz: %d\n", sz);
  // if(sz >= PGSIZE)
  //   panic("uvmfirst: more than a page");
  mem = static_cast<char*>(mem::KAlloc::alloc());
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, MIN(sz, PGSIZE));

  mem = static_cast<char*>(mem::KAlloc::alloc());
  memset(mem, 0, PGSIZE);
  mappages(pagetable, PGSIZE, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  if (sz > PGSIZE) {
    memmove(mem, (uchar*)((uint64)src + PGSIZE), MIN(sz - PGSIZE, PGSIZE));
  }

  mem = static_cast<char*>(mem::KAlloc::alloc());
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 2 * PGSIZE, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  if (sz > 2 * PGSIZE) {
    memmove(mem, (uchar*)((uint64)src + 2 * PGSIZE), MIN(sz - 2 * PGSIZE, PGSIZE));
  }
#elif defined(LOONGARCH)
  char *mem;
  k_printer.printf("sz: %d\n", sz);
  // if(sz >= PGSIZE)
  //   panic("uvmfirst: more than a page");
  mem = static_cast<char*>(mem::KAlloc::alloc());
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_MAT | PTE_PLV | PTE_D | PTE_P);
  memmove(mem, src, MIN(sz, PGSIZE));

  mem = static_cast<char*>(mem::KAlloc::alloc());
  memset(mem, 0, PGSIZE);
  mappages(pagetable, PGSIZE, PGSIZE, (uint64)mem, PTE_W | PTE_MAT | PTE_PLV | PTE_D | PTE_P);
  if (sz > PGSIZE) {
    memmove(mem, (uchar*)((uint64)src + PGSIZE), MIN(sz - PGSIZE, PGSIZE));
  }

  mem = static_cast<char*>(mem::KAlloc::alloc());
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 2 * PGSIZE, PGSIZE, (uint64)mem, PTE_W | PTE_MAT | PTE_PLV | PTE_D | PTE_P);
  if (sz > 2 * PGSIZE) {
    memmove(mem, (uchar*)((uint64)src + 2 * PGSIZE), MIN(sz - 2 * PGSIZE, PGSIZE));
  }
#endif
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
#ifdef RISCV
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = static_cast<char*>(mem::KAlloc::alloc());
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      mem::KAlloc::free(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
#elif defined(LOONGARCH)
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = static_cast<char*>(mem::KAlloc::alloc());
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem,  xperm) != 0){
      mem::KAlloc::free(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
#endif
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (PTE_FLAGS(pte) == PTE_V)){
      // this PTE points to a lower-level page table.
#ifdef RISCV
      uint64 child = PTE2PA(pte);
#elif defined(LOONGARCH)
      uint64 child = (PTE2PA(pte) | DMWIN_MASK);
#endif

      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }

  mem::KAlloc::free((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);

  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new_pagetable, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      continue;
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = static_cast<char*>(mem::KAlloc::alloc())) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new_pagetable, i, PGSIZE, (uint64)mem, flags) != 0){
      mem::KAlloc::free(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new_pagetable, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;

    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
