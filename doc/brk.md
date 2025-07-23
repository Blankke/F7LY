这是因为brk01这个test很奇怪
```c
	for (tst_variant = 0; tst_variant < test_variants; tst_variant++) {
		if (tst_test->all_filesystems)
			ret |= run_tcases_per_fs();
		else
			ret |= fork_testrun();

		if (ret & ~(TCONF))
			goto exit;
	}
```
当第一次跑的时候tst_variant会被设置成0，然后跑的是第二个分支
```c
	if (tst_variant) {
		tst_res(TINFO, "Testing syscall variant");
		cur_brk = (void *)tst_syscall(__NR_brk, 0);
	} else {
		tst_res(TINFO, "Testing libc variant");
		cur_brk = (void *)sbrk(0);

		if (cur_brk == (void *)-1)
			tst_brk(TCONF, "sbrk() not implemented");

		/*
		 * Check if brk itself is implemented: updating to the current break
		 * should be a no-op.
		 */
		if (brk(cur_brk) != 0)
			tst_brk(TCONF, "brk() not implemented");
	}
```
第二个分支跟man的描述一样，需要有sbrk和brk，brk成功返回0，sbrk返回堆顶。
第一个分支就用的是syscall，然后是那种brk返回堆顶的实现方法。wsl就是这种实现。

拿wsl跑了之后就会发现其实wsl是第二个分支没过，然后系统是退出后第二次进，进了的时候tst_variant会被设成1，然后就可以跑第一个分支。

我们这边很奇怪我发现cur_brk就极霸不是sbrk在调用，sbrk这个函数还是进入了brk，所以传入的东西就不对。
`	cur_brk = (void *)sbrk(0);`根本就没有成功，这个是怎么进的？