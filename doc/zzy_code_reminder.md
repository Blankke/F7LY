# 程序段加载(该思想已废弃)
```cpp
		program_section_desc _prog_sections[max_program_section_num];
		int					 _prog_section_cnt = 0;
```
需要修改的函数
- user_init
- fork
- execve
- free_proc
# 对于sched函数多次调用的解释
![alt text](image-1.png)
在第一次进入fork_ret时, 会设置磁盘, 因此会发生多次中断, 每次中断都会重新进入sched调度器进行调度