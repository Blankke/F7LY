
# ltp提升目标

## 修复步骤
在usertest.cc的struct ltp_testcase ltp_testcases[]中有约定，在第一个 {NULL, false, false, false, false} 的测例，是我们当前已测试，但是还没有测试通过的测例，中间没有注释的测例，是我们已经测试通过的测例，后面注释掉的测例，是我们还未进行测试的测例。
```
    {"fchownat01", false, true, false, true}, //pass但是没summary
```
类似这个测例这种，如果输出中没用summary，按照测评机计分要求，只有glibc能够得分，所以在musl部分我们保持关闭。
- 每一次开发者会手动到后面未测试的测例中寻找一批测例进行测试，然后发现未通过的问题便放在ltp_testcases的开头处第一个NULL的前面意为我们即将修复这一批测例。NULL 之前“已注释”的表项需要你自行启用，最终是要把null前注释掉的都全部通过
- 这一批测例后面都会紧跟注释信息为什么不通过，开发者和AGENT需要根据这些信息来分析问题所在，进行修复，直到这一批测例全部通过。
- 修复中，需要在对应的系统调用中修改行为，或实现新的系统调用，并在你的所有修复中留下注释。
- 通过后，开发者需要将这一批测例移动到ltp_testcases的中间部分（接在所有没有注释的测例的尾部），并且更换注释信息为pass的数量，以及是否有summary，表示这一批测例已经通过了。
- 然后需要验证这一批测例没有影响到其余测例的通过情况，验证方法是通过注释掉第一个NULL，运行双架构的glibc和musl，打开所有测试过的测例（不仅仅是每次修复的当轮测例，而是所有未注释状态的测例），进行一次完整的测试并输出日志到output中，通过搜索TFAIL和TBROK来确认是否有未标注原因的测例不通过（有个别测例后面有备注错误原因是已知的错误），如果有的话需要分析原因并修复，直到没有期望之外的测例不通过为止。
- ltp需要更新scoreboard
- 你可以按照null这个获取subset的方式，或任意你喜欢的方式，但是达成目标后应该恢复到原来的样子，删除所有调试的函数语句。

## 可借鉴代码
- Falcore
    - https://gitlab.eduxiji.net/T2026104869910452/oskernel2026-falcores.git
    - 这个LTP有4000分，我们当前只有3000分，差距还是比较大的，需要学习这个项目LTP跑了什么测试用例比我们多1000分，然后我们慢慢追上
- A20
    - https://gitlab.eduxiji.net/T202610486999803/oskernel2025-a20.git
    - 这个队伍的LTP实现似乎有问题，排行榜上出现了26000分，这应该是共享内存爆了让测例出错。但是他们的netperf是可以跑的，iperf也能跑，并且他们的libcbench分数也不低。我们需要参考他们我提到的这几个测例是如何跑的，尽量达到他们的水平。

# 任务板
这里会记录每次手工评测后未通过的事项，下面的所有修复任务需要根据修复步骤一节提到的方式解决，完成后将“待完成”字样改为“待验收”。
## 待完成 2026.5.29 12:16：
```c
    // {"shm_comm", true, true, true, true},//TFAIL: shared memory leak between namespaces
    // {"shm_test", true, true, true, true},//啥比
    {"shmat02", true, true, true, true},  //pass3
    // {"shmat1", true, true, true, true},//也是啥比
    {"shmctl01", true, true, true, true}, //pass 12
    {"shmctl03", true, true, true, true}, //pass4  TODO曾经有隐患待验证
    {"shmctl04", true, true, true, true}, //kernel doesn't support SHM_STAT_ANY
    {"shmctl05", true, true, true, true}, // remap_file_pages未实现
    // {"shmctl06", true, true, true, true}, //test requires struct shmid64_ds to have the time_high fields
    {"shmem_2nstest", true, true, true, true}, //pass 1
    // {"shmget02", true, true, true, true}, //TBROK: Failed to close '/proc/sys/kernel/shmmax': EBADF (9)
    // {"shmget03", true, true, true, true},  //TBROK: fopen(/proc/sysvipc/shm,r) failed: ENOENT (2)
    {"shmget04", true, true, true, true}, //passed   3
    {"shmget05", true, true, true, true}, //.config
    {"shmget06", true, true, true, true}, //.config
    {"shmnstest", true, true, true, true}, //pass 1
    {"shmt02", false, true, false, true}, //pass 无summary
    {"shmt03", false, true, false, true}, //pass 无summary
    {"shmt04", false, true, false, true}, //pass 无summary
    {"shmt05", false, true, false, true}, //pass 无summary
    {"shmt06", false, true, false, true}, //pass 无summary
    {"shmt07", false, true, false, true}, //pass 无summary
    {"shmt08", false, true, false, true}, //pass 无summary
    {"shmt09", false, true, false, true}, //sbrk 无summary  TFAIL  :  shmt09.c:173: Error: sbrk succeeded!  ret = 0xf0180, curbrk = 0xffffffffffffffff, 
    {"shmt10", false, true, false, true}, //pass 无summary
```
上述为新增的测试未解决测例部分，其中shm_test和shmat1不作要求，保持注释状态，其余注释状态的测例均书写了fail原因，需要自行复现上述测例并完成修复