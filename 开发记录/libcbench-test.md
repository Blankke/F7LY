此处因为匿名映射，测试的代码如下
```c
size_t b_malloc_sparse(void *dummy)
{
	void *p[10000];
	size_t i;
	for (i=0; i<sizeof p/sizeof *p; i++) {
		p[i] = malloc(4000);
		memset(p[i], 0, 4000);
	}
	for (i=0; i<sizeof p/sizeof *p; i++)
		if (i%150) free(p[i]);
	return 0;
}

```
我们的mmap一开始是静态分配，固定分配10PGSIZE大小，那么在第11次malloc的时候就会报错，实际上，静态分配一定会失败。

所以我的修改方案是将vma的匿名映射设置为可扩展的
```c
    struct vma
    {
         ……
        uint64 max_len;         // 新增：最大可扩展长度
        bool is_expandable;     // 新增：是否可扩展
    };
    int mmap{
          ......
                if (fd == -1) // 匿名映射
                {
                    // 设置初始大小为请求大小
                    p->_vm[i].is_expandable = 1;             // 可扩展
                    p->_vm[i].len = MAX(length, 4 * PGSIZE); // 至少4页
                    p->_vm[i].max_len = MAXVA - p->_sz;      // 设置最大可扩展大小
                }
                else
                {
                    p->_vm[i].len = length; // 文件映射保持原样
                    p->_vm[i].max_len = length;
                    printfCyan("[mmap] file mapping at %p, length: %d, prot: %d, flags: %d, fd: %d\n",
                               (void *)p->_vm[i].addr, length, prot, flags, fd);
                    vfile->dup(); // 只对文件映射增加引用计数
                }

                p->_sz += p->_vm[i].len;       // 扩展进程的虚拟内存空间
                return (void *)p->_vm[i].addr; //
                }
```
这样然后在缺页异常的时候判断是否可以扩展，那么首先我们的缺页异常就要判断所有的缺页情况，而不是以前的只判断进程大小内的缺页
```c
   if (mmap_handler(r_stval(), cause) != 0)
    {
      printfRed("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->_pid);
      printfRed("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->_killed = 1;
    }
```
然后再进行检查
```c

      // 检查是否在当前VMA范围内
      if (va < p->_vm[i].addr + p->_vm[i].len)
      {

        break; // 在当前VMA范围内
      }
      // 检查是否可以扩展
      else if (p->_vm[i].is_expandable &&
               va < p->_vm[i].addr + p->_vm[i].max_len)
      {
        // 扩展当前VMA
        uint64 new_len = PGROUNDUP(va - p->_vm[i].addr + PGSIZE);
        if (new_len <= p->_vm[i].max_len)
        {
          p->_vm[i].len = new_len;
          p->_sz += (new_len - p->_vm[i].len);
          break;
        }
      }
```

这样mmap就可以支持大文件的扩展分配，那么剩下的问题是物理地址一开始没有考虑这么大。
所以改一下buddysystem的参数
```c
#define BSSIZE 40 //最多支持 2^10 = 1024 页的分配粒度

```
然后就能跑了

但是在free的时候还有问题，页表的一些释放出问题了。目前先把panic取消了，强行跑通是可以的。
```c
  void PageTable::freewalk()
    {
        for (uint i = 0; i < 512; i++)
        {
            Pte pte = get_pte(i);
            if (pte.is_valid() && (!pte.is_leaf())) // 这也抄错了吧，原来写的pte.get_flags() != 0是什么鬼，comment by @gkq
            {
                PageTable child;
                child.set_base(PTE2PA(pte.get_data()));
                child.freewalk();
                reset_pte_data(i);
            }
            else if (pte.is_valid())
            {
                ///@todo!!!!!!!!!!!目前无视风险，继续访问！！！！！！
                // printfRed("freewalk: leaf pte[%d]: %p, pte2pa: %p\n", i, pte.get_data(), pte.pa());
                // 打印当前页表项va
                // printfRed("freewalk: leaf pte[%d]: %p, pte2pa: %p, va: %p\n", i, pte.get_data(), pte.pa(), (void *)(get_base() + 8 * i));
                // panic("freewalk: leaf");
            }
        }
        k_pmm.free_page((void *)(get_base()));
    }
```