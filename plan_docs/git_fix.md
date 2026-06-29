## git无法扫描到新增/修改文件
状态：已完成，待验收

### 情况描述
`make shell r`后在`~/git_test`文件夹中`git init`并创建`hello.c`文件后，使用git status显示没有需要提交的文件
```sh
F7LY:~/git_test$ ls -la
total 31
drwxrwxrwx    3 root     root          8192 Jan  1  1970 .
drwx------    5 root     root          4096 Jan 27 21:19 ..
drwxrwxrwx    6 root     root          8192 Jan  1  1970 .git
-rwxrwxrwx    1 root     root         10744 Jan  1  1970 hello
-rw-rw-rw-    1 root     root            60 Jan  1  1970 hello.c
F7LY:~/git_test$ git log
fatal: your current branch 'master' does not have any commits yet
F7LY:~/git_test$ git status
On branch master

No commits yet

nothing to commit (create/copy files and use "git add" to track)

```

怀疑文件系统和相关系统调用的行为存在问题。
请在文件系统相关pipeline中添加注释，排查该问题。
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码

### 原因
getdents64 返回的目录项类型不符合 Linux ABI。
