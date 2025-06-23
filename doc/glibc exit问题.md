

```c
void
attribute_hidden
__run_exit_handlers (int status, struct exit_function_list **listp,
		     bool run_list_atexit, bool run_dtors)
{
  /* First, call the TLS destructors.  */
  if (run_dtors)
    call_function_static_weak (__call_tls_dtors);
  __libc_lock_lock (__exit_funcs_lock);
  /* We do it this way to handle recursive calls to exit () made by
     the functions registered with `atexit' and `on_exit'. We call
     everyone on the list and use the status value in the last
     exit (). */
  while (true)
    {
      struct exit_function_list *cur;
    restart:
      cur = *listp;
      if (cur == NULL)
	{
	  /* Exit processing complete.  We will not allow any more
	     atexit/on_exit registrations.  */
	  __exit_funcs_done = true;
	  break;
	}
      while (cur->idx > 0)
	{
	  struct exit_function *const f = &cur->fns[--cur->idx];
	  const uint64_t new_exitfn_called = __new_exitfn_called;
	  switch (f->flavor)
	    {
	      void (*atfct) (void);
	      void (*onfct) (int status, void *arg);
	      void (*cxafct) (void *arg, int status);
	      void *arg;
	    case ef_free:
	    case ef_us:
	      break;
	    case ef_on:
	      onfct = f->func.on.fn;
	      arg = f->func.on.arg;
	      PTR_DEMANGLE (onfct);
	      /* Unlock the list while we call a foreign function.  */
	      __libc_lock_unlock (__exit_funcs_lock);
	      onfct (status, arg);
	      __libc_lock_lock (__exit_funcs_lock);
	      break;
	    case ef_at:
	      atfct = f->func.at;
	      PTR_DEMANGLE (atfct);
	      /* Unlock the list while we call a foreign function.  */
	      __libc_lock_unlock (__exit_funcs_lock);
	      atfct ();
	      __libc_lock_lock (__exit_funcs_lock);
	      break;
	    case ef_cxa:
	      /* To avoid dlclose/exit race calling cxafct twice (BZ 22180),
		 we must mark this function as ef_free.  */
	      f->flavor = ef_free;
	      cxafct = f->func.cxa.fn;
	      arg = f->func.cxa.arg;
	      PTR_DEMANGLE (cxafct);
	      /* Unlock the list while we call a foreign function.  */
	      __libc_lock_unlock (__exit_funcs_lock);
	      cxafct (arg, status);
	      __libc_lock_lock (__exit_funcs_lock);
	      break;
	    }
	  if (__glibc_unlikely (new_exitfn_called != __new_exitfn_called))
	    /* The last exit function, or another thread, has registered
	       more exit functions.  Start the loop over.  */
	    goto restart;
	}
      *listp = cur->next;
      if (*listp != NULL)
	/* Don't free the last element in the chain, this is the statically
	   allocate element.  */
	free (cur);
    }
  __libc_lock_unlock (__exit_funcs_lock);
  if (run_list_atexit)
    call_function_static_weak (_IO_cleanup);
  _exit (status);
}

```

```asm
00000000000d56ee <__run_exit_handlers>:
   d56ee:	7119                	addi	sp,sp,-128
   d56f0:	e0da                	sd	s6,64(sp)
   d56f2:	fc5e                	sd	s7,56(sp)
   d56f4:	f862                	sd	s8,48(sp)
   d56f6:	fc86                	sd	ra,120(sp)
   d56f8:	f8a2                	sd	s0,112(sp)
   d56fa:	f4a6                	sd	s1,104(sp)
   d56fc:	f0ca                	sd	s2,96(sp)
   d56fe:	ecce                	sd	s3,88(sp)
   d5700:	e8d2                	sd	s4,80(sp)
   d5702:	e4d6                	sd	s5,72(sp)
   d5704:	f466                	sd	s9,40(sp)
   d5706:	f06a                	sd	s10,32(sp)
   d5708:	ec6e                	sd	s11,24(sp)
   d570a:	000e5797          	auipc	a5,0xe5
   d570e:	ede7b783          	ld	a5,-290(a5) # 1ba5e8 <_GLOBAL_OFFSET_TABLE_+0xc8>
   d5712:	8b2a                	mv	s6,a0
   d5714:	8bae                	mv	s7,a1
   d5716:	8c32                	mv	s8,a2
   d5718:	c399                	beqz	a5,d571e <__run_exit_handlers+0x30>
   d571a:	18069e63          	bnez	a3,d58b6 <__run_exit_handlers+0x1c8>
   d571e:	000e5417          	auipc	s0,0xe5
   d5722:	07a43403          	ld	s0,122(s0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d5726:	4705                	li	a4,1
   d5728:	100427af          	lr.w	a5,(s0)
   d572c:	e781                	bnez	a5,d5734 <__run_exit_handlers+0x46>
   d572e:	1ce426af          	sc.w.aq	a3,a4,(s0)
   d5732:	fafd                	bnez	a3,d5728 <__run_exit_handlers+0x3a>
   d5734:	2781                	sext.w	a5,a5
   d5736:	18079963          	bnez	a5,d58c8 <__run_exit_handlers+0x1da>
   d573a:	498d                	li	s3,3
   d573c:	4911                	li	s2,4
   d573e:	4489                	li	s1,2
   d5740:	000bbc83          	ld	s9,0(s7)
   d5744:	020c8d63          	beqz	s9,d577e <__run_exit_handlers+0x90>
   d5748:	4a81                	li	s5,0
   d574a:	4a05                	li	s4,1
   d574c:	008cb783          	ld	a5,8(s9)
   d5750:	00579713          	slli	a4,a5,0x5
   d5754:	9766                	add	a4,a4,s9
   d5756:	cf91                	beqz	a5,d5772 <__run_exit_handlers+0x84>
   d5758:	17fd                	addi	a5,a5,-1
   d575a:	00fcb423          	sd	a5,8(s9)
   d575e:	ff073683          	ld	a3,-16(a4) # ff0 <__h_errno+0xf5c>
   d5762:	05368f63          	beq	a3,s3,d57c0 <__run_exit_handlers+0xd2>
   d5766:	0d268963          	beq	a3,s2,d5838 <__run_exit_handlers+0x14a>
   d576a:	08968863          	beq	a3,s1,d57fa <__run_exit_handlers+0x10c>
   d576e:	1701                	addi	a4,a4,-32
   d5770:	f7e5                	bnez	a5,d5758 <__run_exit_handlers+0x6a>
   d5772:	000cb783          	ld	a5,0(s9)
   d5776:	00fbb023          	sd	a5,0(s7)
   d577a:	12079a63          	bnez	a5,d58ae <__run_exit_handlers+0x1c0>
   d577e:	4705                	li	a4,1
   d5780:	000ec797          	auipc	a5,0xec
   d5784:	18e78ca3          	sb	a4,409(a5) # 1c1919 <__exit_funcs_done>
   d5788:	4781                	li	a5,0
   d578a:	0f50000f          	fence	iorw,ow
   d578e:	08f427af          	amoswap.w	a5,a5,(s0)
   d5792:	2781                	sext.w	a5,a5
   d5794:	12f74363          	blt	a4,a5,d58ba <__run_exit_handlers+0x1cc>
   d5798:	020c0163          	beqz	s8,d57ba <__run_exit_handlers+0xcc>
   d579c:	000e5417          	auipc	s0,0xe5
   d57a0:	21443403          	ld	s0,532(s0) # 1ba9b0 <_GLOBAL_OFFSET_TABLE_+0x490>
   d57a4:	000e5497          	auipc	s1,0xe5
   d57a8:	0744b483          	ld	s1,116(s1) # 1ba818 <_GLOBAL_OFFSET_TABLE_+0x2f8>
   d57ac:	00947763          	bgeu	s0,s1,d57ba <__run_exit_handlers+0xcc>
   d57b0:	601c                	ld	a5,0(s0)
   d57b2:	0421                	addi	s0,s0,8
   d57b4:	9782                	jalr	a5
   d57b6:	fe946de3          	bltu	s0,s1,d57b0 <__run_exit_handlers+0xc2>
   d57ba:	855a                	mv	a0,s6
   d57bc:	3161f0ef          	jal	ra,f4ad2 <_exit>
   d57c0:	0796                	slli	a5,a5,0x5
   d57c2:	97e6                	add	a5,a5,s9
   d57c4:	0187bd03          	ld	s10,24(a5)
   d57c8:	87d6                	mv	a5,s5
   d57ca:	0f50000f          	fence	iorw,ow
   d57ce:	08f427af          	amoswap.w	a5,a5,(s0)
   d57d2:	2781                	sext.w	a5,a5
   d57d4:	0afa4663          	blt	s4,a5,d5880 <__run_exit_handlers+0x192>
   d57d8:	9d02                	jalr	s10
   d57da:	4705                	li	a4,1
   d57dc:	100427af          	lr.w	a5,(s0)
   d57e0:	e781                	bnez	a5,d57e8 <__run_exit_handlers+0xfa>
   d57e2:	1ce426af          	sc.w.aq	a3,a4,(s0)
   d57e6:	fafd                	bnez	a3,d57dc <__run_exit_handlers+0xee>
   d57e8:	2781                	sext.w	a5,a5
   d57ea:	d3ad                	beqz	a5,d574c <__run_exit_handlers+0x5e>
   d57ec:	000e5517          	auipc	a0,0xe5
   d57f0:	fac53503          	ld	a0,-84(a0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d57f4:	3610f0ef          	jal	ra,e5354 <__lll_lock_wait_private>
   d57f8:	bf91                	j	d574c <__run_exit_handlers+0x5e>
   d57fa:	0796                	slli	a5,a5,0x5
   d57fc:	97e6                	add	a5,a5,s9
   d57fe:	0187bd03          	ld	s10,24(a5)
   d5802:	738c                	ld	a1,32(a5)
   d5804:	87d6                	mv	a5,s5
   d5806:	0f50000f          	fence	iorw,ow
   d580a:	08f427af          	amoswap.w	a5,a5,(s0)
   d580e:	2781                	sext.w	a5,a5
   d5810:	08fa4663          	blt	s4,a5,d589c <__run_exit_handlers+0x1ae>
   d5814:	855a                	mv	a0,s6
   d5816:	9d02                	jalr	s10
   d5818:	4705                	li	a4,1
   d581a:	100427af          	lr.w	a5,(s0)
   d581e:	e781                	bnez	a5,d5826 <__run_exit_handlers+0x138>
   d5820:	1ce426af          	sc.w.aq	a3,a4,(s0)
   d5824:	fafd                	bnez	a3,d581a <__run_exit_handlers+0x12c>
   d5826:	2781                	sext.w	a5,a5
   d5828:	d395                	beqz	a5,d574c <__run_exit_handlers+0x5e>
   d582a:	000e5517          	auipc	a0,0xe5
   d582e:	f6e53503          	ld	a0,-146(a0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d5832:	3230f0ef          	jal	ra,e5354 <__lll_lock_wait_private>
   d5836:	bf19                	j	d574c <__run_exit_handlers+0x5e>
   d5838:	0796                	slli	a5,a5,0x5
   d583a:	97e6                	add	a5,a5,s9
   d583c:	0187bd03          	ld	s10,24(a5)
   d5840:	0207bd83          	ld	s11,32(a5)
   d5844:	0007b823          	sd	zero,16(a5)
   d5848:	87d6                	mv	a5,s5
   d584a:	0f50000f          	fence	iorw,ow
   d584e:	08f427af          	amoswap.w	a5,a5,(s0)
   d5852:	2781                	sext.w	a5,a5
   d5854:	02fa4d63          	blt	s4,a5,d588e <__run_exit_handlers+0x1a0>
   d5858:	85da                	mv	a1,s6
   d585a:	856e                	mv	a0,s11
   d585c:	9d02                	jalr	s10
   d585e:	4705                	li	a4,1
   d5860:	100427af          	lr.w	a5,(s0)
   d5864:	e781                	bnez	a5,d586c <__run_exit_handlers+0x17e>
   d5866:	1ce426af          	sc.w.aq	a3,a4,(s0)
   d586a:	fafd                	bnez	a3,d5860 <__run_exit_handlers+0x172>
   d586c:	2781                	sext.w	a5,a5
   d586e:	ec078fe3          	beqz	a5,d574c <__run_exit_handlers+0x5e>
   d5872:	000e5517          	auipc	a0,0xe5
   d5876:	f2653503          	ld	a0,-218(a0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d587a:	2db0f0ef          	jal	ra,e5354 <__lll_lock_wait_private>
   d587e:	b5f9                	j	d574c <__run_exit_handlers+0x5e>
   d5880:	000e5517          	auipc	a0,0xe5
   d5884:	f1853503          	ld	a0,-232(a0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d5888:	3770f0ef          	jal	ra,e53fe <__lll_lock_wake_private>
   d588c:	b7b1                	j	d57d8 <__run_exit_handlers+0xea>
   d588e:	000e5517          	auipc	a0,0xe5
   d5892:	f0a53503          	ld	a0,-246(a0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d5896:	3690f0ef          	jal	ra,e53fe <__lll_lock_wake_private>
   d589a:	bf7d                	j	d5858 <__run_exit_handlers+0x16a>
   d589c:	000e5517          	auipc	a0,0xe5
   d58a0:	efc53503          	ld	a0,-260(a0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d58a4:	e42e                	sd	a1,8(sp)
   d58a6:	3590f0ef          	jal	ra,e53fe <__lll_lock_wake_private>
   d58aa:	65a2                	ld	a1,8(sp)
   d58ac:	b7a5                	j	d5814 <__run_exit_handlers+0x126>
   d58ae:	8566                	mv	a0,s9
   d58b0:	50b130ef          	jal	ra,e95ba <__free>
   d58b4:	b571                	j	d5740 <__run_exit_handlers+0x52>
   d58b6:	9782                	jalr	a5
   d58b8:	b59d                	j	d571e <__run_exit_handlers+0x30>
   d58ba:	000e5517          	auipc	a0,0xe5
   d58be:	ede53503          	ld	a0,-290(a0) # 1ba798 <_GLOBAL_OFFSET_TABLE_+0x278>
   d58c2:	33d0f0ef          	jal	ra,e53fe <__lll_lock_wake_private>
   d58c6:	bdc9                	j	d5798 <__run_exit_handlers+0xaa>
   d58c8:	8522                	mv	a0,s0
   d58ca:	28b0f0ef          	jal	ra,e5354 <__lll_lock_wait_private>
   d58ce:	b5b5                	j	d573a <__run_exit_handlers+0x4c>
```


# 这段代码的trap原因
cxafct = f->func.cxa.fn;
在退出第二个退出函数的时候爆炸。
即第二个退出函数的函数指针不对，我们要找到是谁注册的，为什么注册错了。


# 注册退出函数的函数__cxa_atexit 
```c
int
__cxa_atexit (void (*func) (void *), void *arg, void *d)
{
  return __internal_atexit (func, arg, d, &__exit_funcs);
}
libc_hidden_def (__cxa_atexit)
```

```c
int
attribute_hidden
__internal_atexit (void (*func) (void *), void *arg, void *d,
		   struct exit_function_list **listp)
{
  struct exit_function *new;
  /* As a QoI issue we detect NULL early with an assertion instead
     of a SIGSEGV at program exit when the handler is run (bug 20544).  */
  assert (func != NULL);
  __libc_lock_lock (__exit_funcs_lock);
  new = __new_exitfn (listp);
  if (new == NULL)
    {
      __libc_lock_unlock (__exit_funcs_lock);
      return -1;
    }
  PTR_MANGLE (func);
  new->func.cxa.fn = (void (*) (void *, int)) func;
  new->func.cxa.arg = arg;
  new->func.cxa.dso_handle = d;
  new->flavor = ef_cxa;
  __libc_lock_unlock (__exit_funcs_lock);
  return 0;
}
```


```gdb
(gdb) bt
#0  0x000000000000d5a6c in __cxa_atexit ()
#1  0x000000000000d0e82 in __libc_start_main_impl ()
#2  0x000000000000105c0 in _start ()
```


```c
int
__libc_start_main_impl (int argc, char **argv,
			char **ev,
			ElfW (auxv_t) * auxvec,
			void (*rtld_fini) (void),
			struct startup_info *stinfo,
			char **stack_on_entry)
{
  /* the PPC SVR4 ABI says that the top thing on the stack will
     be a NULL pointer, so if not we assume that we're being called
     as a statically-linked program by Linux...  */
  if (*stack_on_entry != NULL)
    {
      char **temp;
      /* ...in which case, we have argc as the top thing on the
         stack, followed by argv (NULL-terminated), envp (likewise),
         and the auxiliary vector.  */
      /* 32/64-bit agnostic load from stack */
      argc = *(long int *) stack_on_entry;
      argv = stack_on_entry + 1;
      ev = argv + argc + 1;
#ifdef HAVE_AUX_VECTOR
      temp = ev;
      while (*temp != NULL)
	++temp;
      auxvec = (ElfW (auxv_t) *)++ temp;
#endif
      rtld_fini = NULL;
    }
  for (ElfW (auxv_t) * av = auxvec; av->a_type != AT_NULL; ++av)
    switch (av->a_type)
      {
      /* For the static case, we also need _dl_hwcap, _dl_hwcap2 and
         _dl_platform, so we can call
         __tcb_parse_hwcap_and_convert_at_platform ().  */
#ifndef SHARED
      case AT_HWCAP:
	_dl_hwcap = (unsigned long int) av->a_un.a_val;
	break;
      case AT_HWCAP2:
	_dl_hwcap2 = (unsigned long int) av->a_un.a_val;
	break;
      case AT_HWCAP3:
	_dl_hwcap3 = (unsigned long int) av->a_un.a_val;
	break;
      case AT_HWCAP4:
	_dl_hwcap4 = (unsigned long int) av->a_un.a_val;
	break;
      case AT_PLATFORM:
	_dl_platform = (void *) av->a_un.a_val;
	break;
#endif
      }
  /* Initialize hwcap/hwcap2 and platform data so it can be copied to
     the TCB later in __libc_setup_tls (). (static case only).  */
#ifndef SHARED
  __tcb_parse_hwcap_and_convert_at_platform ();
#endif
  void *stmain = stinfo->main;
#if ENABLE_STATIC_PIE && !defined SHARED
  struct link_map *map = _dl_get_dl_main_map ();
  if (!map->l_relocated)
    stmain = (char *) stmain + elf_machine_load_address ();
#endif
  return generic_start_main (stmain, argc, argv, auxvec,
			     NULL, NULL, rtld_fini,
			     stack_on_entry);
}
DEFINE_LIBC_START_MAIN_VERSION
```



```asm
00000000000d0cae <__libc_start_main>:
   d0cae:	00158713          	addi	a4,a1,1
   d0cb2:	7159                	addi	sp,sp,-112
   d0cb4:	070e                	slli	a4,a4,0x3
   d0cb6:	eca6                	sd	s1,88(sp)
   d0cb8:	e8ca                	sd	s2,80(sp)
   d0cba:	e0d2                	sd	s4,64(sp)
   d0cbc:	fc56                	sd	s5,56(sp)
   d0cbe:	f85a                	sd	s6,48(sp)
   d0cc0:	f486                	sd	ra,104(sp)
   d0cc2:	f0a2                	sd	s0,96(sp)
   d0cc4:	e4ce                	sd	s3,72(sp)
   d0cc6:	f45e                	sd	s7,40(sp)
   d0cc8:	f062                	sd	s8,32(sp)
   d0cca:	ec66                	sd	s9,24(sp)
   d0ccc:	e86a                	sd	s10,16(sp)
   d0cce:	9732                	add	a4,a4,a2
   d0cd0:	000f0b17          	auipc	s6,0xf0
   d0cd4:	da8b0b13          	addi	s6,s6,-600 # 1c0a78 <_environ>
   d0cd8:	8a3e                	mv	s4,a5
   d0cda:	84ae                	mv	s1,a1
   d0cdc:	8932                	mv	s2,a2
   d0cde:	8aaa                	mv	s5,a0
   d0ce0:	00eb3023          	sd	a4,0(s6)
   d0ce4:	000e7797          	auipc	a5,0xe7
   d0ce8:	f107b623          	sd	a6,-244(a5) # 1b7bf0 <__libc_stack_end>
   d0cec:	6314                	ld	a3,0(a4)
   d0cee:	0721                	addi	a4,a4,8
   d0cf0:	fef5                	bnez	a3,d0cec <__libc_start_main+0x3e>
   d0cf2:	853a                	mv	a0,a4
   d0cf4:	4f44c0ef          	jal	ra,11d1e8 <_dl_aux_init>
   d0cf8:	000f0797          	auipc	a5,0xf0
   d0cfc:	20078793          	addi	a5,a5,512 # 1c0ef8 <_dl_phdr>
   d0d00:	6398                	ld	a4,0(a5)
   d0d02:	c335                	beqz	a4,d0d66 <__libc_start_main+0xb8>
   d0d04:	73d4c0ef          	jal	ra,11dc40 <__libc_init_secure>
   d0d08:	000b3503          	ld	a0,0(s6)
   d0d0c:	000eab97          	auipc	s7,0xea
   d0d10:	d84bbb83          	ld	s7,-636(s7) # 1baa90 <_GLOBAL_OFFSET_TABLE_+0x570>
   d0d14:	000ea417          	auipc	s0,0xea
   d0d18:	a8c43403          	ld	s0,-1396(s0) # 1ba7a0 <_GLOBAL_OFFSET_TABLE_+0x280>
   d0d1c:	1574b0ef          	jal	ra,11c672 <__tunables_init>
   d0d20:	07747d63          	bgeu	s0,s7,d0d9a <__libc_start_main+0xec>
   d0d24:	59fd                	li	s3,-1
   d0d26:	0209d993          	srli	s3,s3,0x20
   d0d2a:	03a00c13          	li	s8,58
   d0d2e:	000f0c97          	auipc	s9,0xf0
   d0d32:	1b2c8c93          	addi	s9,s9,434 # 1c0ee0 <_dl_hwcap>
   d0d36:	a819                	j	d0d4c <__libc_start_main+0x9e>
   d0d38:	681c                	ld	a5,16(s0)
   d0d3a:	000cb503          	ld	a0,0(s9)
   d0d3e:	4581                	li	a1,0
   d0d40:	0461                	addi	s0,s0,24
   d0d42:	9782                	jalr	a5
   d0d44:	00ad3023          	sd	a0,0(s10)
   d0d48:	05747963          	bgeu	s0,s7,d0d9a <__libc_start_main+0xec>
   d0d4c:	6418                	ld	a4,8(s0)
   d0d4e:	00043d03          	ld	s10,0(s0)
   d0d52:	01377733          	and	a4,a4,s3
   d0d56:	ff8701e3          	beq	a4,s8,d0d38 <__libc_start_main+0x8a>
   d0d5a:	000a9517          	auipc	a0,0xa9
   d0d5e:	21650513          	addi	a0,a0,534 # 179f70 <_ns_flagdata+0xc0>
   d0d62:	375100ef          	jal	ra,e18d6 <__libc_fatal>
   d0d66:	fff3f717          	auipc	a4,0xfff3f
   d0d6a:	29a70713          	addi	a4,a4,666 # 10000 <__ehdr_start>
   d0d6e:	03675603          	lhu	a2,54(a4)
   d0d72:	03800693          	li	a3,56
   d0d76:	1ad60763          	beq	a2,a3,d0f24 <__libc_start_main+0x276>
   d0d7a:	000a9697          	auipc	a3,0xa9
   d0d7e:	26668693          	addi	a3,a3,614 # 179fe0 <__PRETTY_FUNCTION__.0>
   d0d82:	11a00613          	li	a2,282
   d0d86:	000a9597          	auipc	a1,0xa9
   d0d8a:	1aa58593          	addi	a1,a1,426 # 179f30 <_ns_flagdata+0x80>
   d0d8e:	000a9517          	auipc	a0,0xa9
   d0d92:	1b250513          	addi	a0,a0,434 # 179f40 <_ns_flagdata+0x90>
   d0d96:	54a000ef          	jal	ra,d12e0 <__assert_fail>
   d0d9a:	26a000ef          	jal	ra,d1004 <__libc_setup_tls>
   d0d9e:	000e7997          	auipc	s3,0xe7
   d0da2:	e2298993          	addi	s3,s3,-478 # 1b7bc0 <_dl_random>
   d0da6:	0009b783          	ld	a5,0(s3)
   d0daa:	000e6417          	auipc	s0,0xe6
   d0dae:	16640413          	addi	s0,s0,358 # 1b6f10 <__stack_chk_guard>
   d0db2:	0017c803          	lbu	a6,1(a5)
   d0db6:	0007c703          	lbu	a4,0(a5)
   d0dba:	0027c503          	lbu	a0,2(a5)
   d0dbe:	0037c583          	lbu	a1,3(a5)
   d0dc2:	0047c603          	lbu	a2,4(a5)
   d0dc6:	0822                	slli	a6,a6,0x8
   d0dc8:	0057c683          	lbu	a3,5(a5)
   d0dcc:	00e86833          	or	a6,a6,a4
   d0dd0:	0542                	slli	a0,a0,0x10
   d0dd2:	0067c703          	lbu	a4,6(a5)
   d0dd6:	01056533          	or	a0,a0,a6
   d0dda:	05e2                	slli	a1,a1,0x18
   d0ddc:	0077c783          	lbu	a5,7(a5)
   d0de0:	8dc9                	or	a1,a1,a0
   d0de2:	1602                	slli	a2,a2,0x20
   d0de4:	8e4d                	or	a2,a2,a1
   d0de6:	16a2                	slli	a3,a3,0x28
   d0de8:	8ed1                	or	a3,a3,a2
   d0dea:	1742                	slli	a4,a4,0x30
   d0dec:	8f55                	or	a4,a4,a3
   d0dee:	17e2                	slli	a5,a5,0x38
   d0df0:	8fd9                	or	a5,a5,a4
   d0df2:	f007f793          	andi	a5,a5,-256
   d0df6:	e01c                	sd	a5,0(s0)
   d0df8:	54b4c0ef          	jal	ra,11db42 <_dl_discover_osversion>
   d0dfc:	14054763          	bltz	a0,d0f4a <__libc_start_main+0x29c>
   d0e00:	000f0797          	auipc	a5,0xf0
   d0e04:	14078793          	addi	a5,a5,320 # 1c0f40 <_dl_osversion>
   d0e08:	4398                	lw	a4,0(a5)
   d0e0a:	0005069b          	sext.w	a3,a0
   d0e0e:	12071b63          	bnez	a4,d0f44 <__libc_start_main+0x296>
   d0e12:	c394                	sw	a3,0(a5)
   d0e14:	000417b7          	lui	a5,0x41
   d0e18:	eff78793          	addi	a5,a5,-257 # 40eff <tc_main+0x1d7>
   d0e1c:	10a7de63          	bge	a5,a0,d0f38 <__libc_start_main+0x28a>
   d0e20:	000ea797          	auipc	a5,0xea
   d0e24:	cb87b783          	ld	a5,-840(a5) # 1baad8 <_GLOBAL_OFFSET_TABLE_+0x5b8>
   d0e28:	c391                	beqz	a5,d0e2c <__libc_start_main+0x17e>
   d0e2a:	9782                	jalr	a5
   d0e2c:	0009b783          	ld	a5,0(s3)
   d0e30:	0087c303          	lbu	t1,8(a5)
   d0e34:	0097c883          	lbu	a7,9(a5)
   d0e38:	00a7c803          	lbu	a6,10(a5)
   d0e3c:	00b7c503          	lbu	a0,11(a5)
   d0e40:	00c7c583          	lbu	a1,12(a5)
   d0e44:	00d7c603          	lbu	a2,13(a5)
   d0e48:	00e7c683          	lbu	a3,14(a5)
   d0e4c:	00f7c703          	lbu	a4,15(a5)
   d0e50:	00610423          	sb	t1,8(sp)
   d0e54:	011104a3          	sb	a7,9(sp)
   d0e58:	01010523          	sb	a6,10(sp)
   d0e5c:	00a105a3          	sb	a0,11(sp)
   d0e60:	00b10623          	sb	a1,12(sp)
   d0e64:	00c106a3          	sb	a2,13(sp)
   d0e68:	00d10723          	sb	a3,14(sp)
   d0e6c:	00e107a3          	sb	a4,15(sp)
   d0e70:	67a2                	ld	a5,8(sp)
   d0e72:	e41c                	sd	a5,8(s0)
   d0e74:	000a0763          	beqz	s4,d0e82 <__libc_start_main+0x1d4>
   d0e78:	4601                	li	a2,0
   d0e7a:	4581                	li	a1,0
   d0e7c:	8552                	mv	a0,s4
   d0e7e:	3ef040ef          	jal	ra,d5a6c <__cxa_atexit>
   d0e82:	4505                	li	a0,1
   d0e84:	61b4c0ef          	jal	ra,11dc9e <__libc_early_init>
   d0e88:	000b3603          	ld	a2,0(s6)
   d0e8c:	85ca                	mv	a1,s2
   d0e8e:	8526                	mv	a0,s1
   d0e90:	6c74c0ef          	jal	ra,11dd56 <__libc_init_first>
   d0e94:	4601                	li	a2,0
   d0e96:	4581                	li	a1,0
   d0e98:	00000517          	auipc	a0,0x0
   d0e9c:	d7050513          	addi	a0,a0,-656 # d0c08 <call_fini>
   d0ea0:	3cd040ef          	jal	ra,d5a6c <__cxa_atexit>
   d0ea4:	000e7797          	auipc	a5,0xe7
   d0ea8:	1547a783          	lw	a5,340(a5) # 1b7ff8 <__libc_enable_secure>
   d0eac:	e7cd                	bnez	a5,d0f56 <__libc_start_main+0x2a8>
   d0eae:	000e1417          	auipc	s0,0xe1
   d0eb2:	bda40413          	addi	s0,s0,-1062 # 1b1a88 <__preinit_array_start>
   d0eb6:	000e1b97          	auipc	s7,0xe1
   d0eba:	bdab8b93          	addi	s7,s7,-1062 # 1b1a90 <__frame_dummy_init_array_entry>
   d0ebe:	408b8bb3          	sub	s7,s7,s0
   d0ec2:	403bdb93          	srai	s7,s7,0x3
   d0ec6:	000b3a03          	ld	s4,0(s6)
   d0eca:	4981                	li	s3,0
   d0ecc:	000b8b63          	beqz	s7,d0ee2 <__libc_start_main+0x234>
   d0ed0:	601c                	ld	a5,0(s0)
   d0ed2:	8652                	mv	a2,s4
   d0ed4:	85ca                	mv	a1,s2
   d0ed6:	8526                	mv	a0,s1
   d0ed8:	0985                	addi	s3,s3,1
   d0eda:	9782                	jalr	a5
   d0edc:	0421                	addi	s0,s0,8
   d0ede:	ff3b99e3          	bne	s7,s3,d0ed0 <__libc_start_main+0x222>
   d0ee2:	000e1417          	auipc	s0,0xe1
   d0ee6:	bae40413          	addi	s0,s0,-1106 # 1b1a90 <__frame_dummy_init_array_entry>
   d0eea:	000e1b17          	auipc	s6,0xe1
   d0eee:	baeb0b13          	addi	s6,s6,-1106 # 1b1a98 <__do_global_dtors_aux_fini_array_entry>
   d0ef2:	408b0b33          	sub	s6,s6,s0
   d0ef6:	403b5b13          	srai	s6,s6,0x3
   d0efa:	4981                	li	s3,0
   d0efc:	000b0b63          	beqz	s6,d0f12 <__libc_start_main+0x264>
   d0f00:	601c                	ld	a5,0(s0)
   d0f02:	8652                	mv	a2,s4
   d0f04:	85ca                	mv	a1,s2
   d0f06:	8526                	mv	a0,s1
   d0f08:	0985                	addi	s3,s3,1
   d0f0a:	9782                	jalr	a5
   d0f0c:	0421                	addi	s0,s0,8
   d0f0e:	ff3b19e3          	bne	s6,s3,d0f00 <__libc_start_main+0x252>
   d0f12:	4581                	li	a1,0
   d0f14:	4501                	li	a0,0
   d0f16:	5db4a0ef          	jal	ra,11bcf0 <_dl_debug_initialize>
   d0f1a:	864a                	mv	a2,s2
   d0f1c:	85a6                	mv	a1,s1
   d0f1e:	8556                	mv	a0,s5
   d0f20:	d21ff0ef          	jal	ra,d0c40 <__libc_start_call_main>
   d0f24:	7314                	ld	a3,32(a4)
   d0f26:	03875603          	lhu	a2,56(a4)
   d0f2a:	9736                	add	a4,a4,a3
   d0f2c:	e398                	sd	a4,0(a5)
   d0f2e:	000f0797          	auipc	a5,0xf0
   d0f32:	fcc7b123          	sd	a2,-62(a5) # 1c0ef0 <_dl_phnum>
   d0f36:	b3f9                	j	d0d04 <__libc_start_main+0x56>
   d0f38:	000a9517          	auipc	a0,0xa9
   d0f3c:	06850513          	addi	a0,a0,104 # 179fa0 <_ns_flagdata+0xf0>
   d0f40:	197100ef          	jal	ra,e18d6 <__libc_fatal>
   d0f44:	ece6f8e3          	bgeu	a3,a4,d0e14 <__libc_start_main+0x166>
   d0f48:	b5e9                	j	d0e12 <__libc_start_main+0x164>
   d0f4a:	000a9517          	auipc	a0,0xa9
   d0f4e:	06e50513          	addi	a0,a0,110 # 179fb8 <_ns_flagdata+0x108>
   d0f52:	185100ef          	jal	ra,e18d6 <__libc_fatal>
   d0f56:	088000ef          	jal	ra,d0fde <__libc_check_standard_fds>
   d0f5a:	bf91                	j	d0eae <__libc_start_main+0x200>
```


generic_start_main:
```c
STATIC int LIBC_START_MAIN(int (*main)(int, char **, char **MAIN_AUXVEC_DECL),
                           int argc,
                           char **argv,
#ifdef LIBC_START_MAIN_AUXVEC_ARG
                           ElfW(auxv_t) * auxvec,
#endif
                           __typeof(main) init,
                           void (*fini)(void),
                           void (*rtld_fini)(void),
                           void *stack_end)
    __attribute__((noreturn));
/* Note: The init and fini parameters are no longer used.  fini is
completely unused, init is still called if not NULL, but the
current startup code always passes NULL.  (In the future, it would
be possible to use fini to pass a version code if init is NULL, to
indicate the link-time glibc without introducing a hard
incompatibility for new programs with older glibc versions.)
For dynamically linked executables, the dynamic segment is used to
locate constructors and destructors.  For statically linked
executables, the relevant symbols are access directly.  */
STATIC int
LIBC_START_MAIN(int (*main)(int, char **, char **MAIN_AUXVEC_DECL),
                int argc, char **argv,
#ifdef LIBC_START_MAIN_AUXVEC_ARG
                ElfW(auxv_t) * auxvec,
#endif
                __typeof(main) init,
                void (*fini)(void),
                void (*rtld_fini)(void), void *stack_end)
{
#ifndef SHARED
    char **ev = &argv[argc + 1];
    __environ = ev;
    /* Store the lowest stack address.  This is done in ld.so if this is
    the code for the DSO.  */
    __libc_stack_end = stack_end;
#ifdef HAVE_AUX_VECTOR
/* First process the auxiliary vector since we need to find the
program header to locate an eventually present PT_TLS entry.  */
#ifndef LIBC_START_MAIN_AUXVEC_ARG
    ElfW(auxv_t) * auxvec;
    {
        char **evp = ev;
        while (*evp++ != NULL)
            ;
        auxvec = (ElfW(auxv_t) *)evp;
    }
#endif
    _dl_aux_init(auxvec);
#endif
    __tunables_init(__environ);
    ARCH_INIT_CPU_FEATURES();
    /* Do static pie self relocation after tunables and cpu features
    are setup for ifunc resolvers. Before this point relocations
    must be avoided.  */
    _dl_relocate_static_pie();
    /* Perform IREL{,A} relocations.  */
    ARCH_SETUP_IREL();
    /* The stack guard goes into the TCB, so initialize it early.  */
    ARCH_SETUP_TLS();
    /* In some architectures, IREL{,A} relocations happen after TLS setup in
    order to let IFUNC resolvers benefit from TCB information, e.g. powerpc's
    hwcap and platform fields available in the TCB.  */
    ARCH_APPLY_IREL();
    /* Set up the stack checker's canary.  */
    uintptr_t stack_chk_guard = _dl_setup_stack_chk_guard(_dl_random);
#ifdef THREAD_SET_STACK_GUARD
    THREAD_SET_STACK_GUARD(stack_chk_guard);
#else
    __stack_chk_guard = stack_chk_guard;
#endif
    /* Initialize libpthread if linked in.  */
    if (__pthread_initialize_minimal != NULL)
        __pthread_initialize_minimal();
    /* Set up the pointer guard value.  */
    uintptr_t pointer_chk_guard = _dl_setup_pointer_guard(_dl_random,
                                                          stack_chk_guard);
#ifdef THREAD_SET_POINTER_GUARD
    THREAD_SET_POINTER_GUARD(pointer_chk_guard);
#else
    __pointer_chk_guard_local = pointer_chk_guard;
#endif
#endif /* !SHARED  */
    /* Register the destructor of the dynamic linker if there is any.  */
    if (__glibc_likely(rtld_fini != NULL))
        __cxa_atexit((void (*)(void *))rtld_fini, NULL, NULL);
#ifndef SHARED
    /* Perform early initialization.  In the shared case, this function
    is called from the dynamic loader as early as possible.  */
    __libc_early_init(true);
    /* Call the initializer of the libc.  This is only needed here if we
    are compiling for the static library in which case we haven't
    run the constructors in `_dl_start_user'.  */
    __libc_init_first(argc, argv, __environ);
    /* Register the destructor of the statically-linked program.  */
    __cxa_atexit(call_fini, NULL, NULL);
    /* Some security at this point.  Prevent starting a SUID binary where
    the standard file descriptors are not opened.  We have to do this
    only for statically linked applications since otherwise the dynamic
    loader did the work already.  */
    if (__builtin_expect(__libc_enable_secure, 0))
        __libc_check_standard_fds();
#endif /* !SHARED */
/* Call the initializer of the program, if any.  */
#ifdef SHARED
    if (__builtin_expect(GLRO(dl_debug_mask) & DL_DEBUG_IMPCALLS, 0))
        GLRO(dl_debug_printf)("\ninitialize program: %s\n\n", argv[0]);
    if (init != NULL)
        /* This is a legacy program which supplied its own init
        routine.  */
        (*init)(argc, argv, __environ MAIN_AUXVEC_PARAM);
    else
        /* This is a current program.  Use the dynamic segment to find
        constructors.  */
        call_init(argc, argv, __environ);
    /* Auditing checkpoint: we have a new object.  */
    _dl_audit_preinit(GL(dl_ns)[LM_ID_BASE]._ns_loaded);
    if (__glibc_unlikely(GLRO(dl_debug_mask) & DL_DEBUG_IMPCALLS))
        GLRO(dl_debug_printf)("\ntransferring control: %s\n\n", argv[0]);
#else /* !SHARED */
    call_init(argc, argv, __environ);
    _dl_debug_initialize(0, LM_ID_BASE);
#endif
    __libc_start_call_main(main, argc, argv MAIN_AUXVEC_PARAM);
}
```


# 这里有个关键
注册了rtld_fini，刚好是倒数第二个退出函数

这里有两次注册
第一次
```asm
   d0e7e:	3ef040ef          	jal	ra,d5a6c <__cxa_atexit>
```

```gdb
i r a0
a0 0x3
```
第二次
```asm
   d0ea0:	3cd040ef          	jal	ra,d5a6c <__cxa_atexit>
```

其中，第一次注册是不对的

| 汇编位置    | 注册的析构函数             | 对应源码                                                     |
| ------- | ------------------- | -------------------------------------------------------- |
| `d0e7e` | `rtld_fini` (动态链接器) | `__cxa_atexit((void (*)(void *))rtld_fini, NULL, NULL);` |
| `d0ea0` | `call_fini` (主程序析构) | `__cxa_atexit(call_fini, NULL, NULL);`                   |

检查又发现，`__libc_start_main`一开始a5也就是rtld_fini传的就是不对的0x3



也就是mv a5 a0这里。
a0好像不用传argc。



debug完毕。