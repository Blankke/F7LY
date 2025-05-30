# 关于系统调用号220
文档中写的是sys_clone
- sys_clone是sys_fork的超集
- 我们一般只会使用sys_fork
# 当前逻辑
- 基于上述逻辑, 我将在sys_clone中调用fork的处理逻辑
- sys_clone的其余逻辑暂未编写