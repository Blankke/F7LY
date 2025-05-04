#pragma once
#include "types.hh"
#include "platform.h"

void            kvminit(void);
void            kvminithart(void);
void            kvmmap(pagetable_t, uint64, uint64, uint64, uint64);
int             mappages(pagetable_t, uint64, uint64, uint64, uint64);
pagetable_t     uvmcreate(void);
void            uvmfirst(pagetable_t, uchar *, uint);
uint64          uvmalloc(pagetable_t, uint64, uint64, int);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
int             uvmcopy(pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
pte_t *         walk(pagetable_t, uint64, int);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
void kernelmap(uint64 va, uint64 pa, uint64 sz, uint64 perm);
uint64          kwalkaddr(uint64 va);
void pci_map(int bus, int dev, int func, void *pages);

uint64 sys_shutdown(void);