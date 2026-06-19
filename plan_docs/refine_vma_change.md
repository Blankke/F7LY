========= START entry-static.exe fscanf ==========
src/functional/fscanf.c:45: write(p[1], "hello, world\n", 13) failed (write error -1!=13 (Bad address))
FAIL fscanf [timed out]

========== START entry-static.exe tgmath ==========
src/functional/tgmath.c:32: pow(2.0,0.5) want 0x1.6a09e667f3bcdp+0 got 0x1.6a09e667f3bcdp+0
FAIL tgmath [status 1]


========== START entry-static.exe daemon_failure ==========
src/regression/daemon-failure.c:57: write failed: Bad address
src/regression/daemon-failure.c:66: child exited with 1
src/regression/daemon-failure.c:71: read failed: child did not send its exit status
FAIL daemon_failure [status 1]

========== END entry-static.exe fgets_eof ==========
========== START entry-static.exe fgetwc_buffering ==========
src/regression/fgetwc-buffering.c:18: write(p[1], "x\340\240", 3) == 3 failed
FAIL fgetwc_buffering [timed out]

========== START entry-static.exe ftello_unflushed_append ==========
src/regression/ftello-unflushed-append.c:25: write(fd, "abcd", 4) == 4 failed (errno: Bad address)
src/regression/ftello-unflushed-append.c:34: ftello is broken before flush: got 3, want 7
src/regression/ftello-unflushed-append.c:38: ftello is broken after flush: got 3, want 7
FAIL ftello_unflushed_append [status 1]

有这样几个libctest现在无法通过了。需要修复。
并且libcbench的#### OS COMP TEST GROUP START libcbench-musl ####
b_malloc_sparse (0)
  time: 7.598519990, virt: 0, res: 0, dirty: 0

b_malloc_bubble (0)
  time: 8.624256760, virt: 0, res: 0, dirty: 0

这个性能也有待提高。虽然glibc的版本这两个测试就非常快了。是否是cache的作用

收起

9:24






我先把当前工作区状态、最近提交和这几个失败点涉及的 syscall/VMA 路径重新对齐一下。这个现象里 write(..., "literal", ...) = EFAULT 很像用户只读段或静态 ELF 段的 copy_in/缺页权限判断出了问题，先不急着猜。




Read 2 files已运行 3 条命令


当前 initcode-la.cc 里 LTP 已被注释，这看起来是你正在缩入口跑 libc 的现场改动，我会保留它，不回滚。接下来我先读 libc_test 调度和 write/copy_in/pipe 路径，再跑一轮 LoongArch 日志把失败稳定复现出来。




Read 4 files和searched code已运行 1 条命令


我会直接跑一轮 LoongArch 当前入口。因为 libc_test("/musl/") 已经排在最前面，5 分钟日志应该足够覆盖你列出的 fscanf/daemon/fgetwc/ftello 这些失败。




已运行 ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs
log="logs/output_l_${ts}_libctest-eFAULT-repro_timeout-6m.txt"
{
  echo "run_at=${ts}"
  echo "arch=loongarch"
  echo "scope=libctest-efault-repro"
  echo "cmd=timeout 6m make run l QEMU_MEM=1G"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 6m make run l QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"







QEMU 已经在跑，等它进入 libc 输出后我会抽关键失败和内核侧 resolve_user_read_pa 打印。




还在运行中。LoongArch 构建和启动都慢一点，我继续等完整日志；不把长输出贴出来。




这轮已经超过一分半。我先旁路看日志尾部，确认是正常推进还是卡在某个具体用例。




已运行 2 条命令


复现稳定：这些 Bad address 都集中在 entry-static.exe 对字符串常量做 write() 的场景，动态版本同样出现对应失败；日志里没有 resolve_user_read_pa 打印，说明失败可能发生在更早的用户指针范围/地址空间归属判断。下一步我会定位 sys_write -> pipe/normal_file -> copy_in 的具体分支。




Read 6 files


失败发生在 pipe/普通文件写入从用户缓冲 copy_in 这条路；当前日志没有打印 copy_in 的缺页诊断，说明需要加一个很小的临时定位点，先确认失败地址属于哪个 VMA、PTE 是不是已存在但权限不对。这个探针我会在修完后撤掉。




Read 3 files和searched code


我看到一个可能的直接原因：Pipe::write_from_user() 在持有 pipe 锁时调用 copy_in，如果这次 copy_in 触发缺页/磁盘读，会带锁进入更深路径；但 Bad address 更像权限/映射判断。先把 copy_in 和 fault_page 条件读完整，再下探。




Read 2 files


allocate_vma_page() 对只读 ELF 段本身允许读缺页；下一步我需要确认 copy_in() 实际进入的是哪条分支，以及失败地址是正常低地址文字常量还是被 LoongArch 符号扩展成高地址。




Read 2 files和searched code已运行 1 条命令


copy_in 定义位置找到了，我刚才读偏了。继续读正确区间。




Read 4 files


关键线索来了：Bad address 日志里没有 copy_in 失败打印，说明 write() 很可能还没走到真正 copy 用户缓冲，而是在 validate_direct_io_request() 或通用地址校验里提前返回了 EFAULT。我现在查这一层。




Read 3 files和searched code


根因已经很清楚：sys_write() 在真正 copy_in() 前先调用了 is_bad_addr()，而 is_bad_addr() 只查当前 PTE，不会按 VMA 懒缺页补页。静态/动态 ELF 的只读字符串页如果第一次被 write() 读取，就会被提前误判成 EFAULT。我要移除这道预检，让 copy_in() 负责懒分配和权限校验。




已编辑 1 个文件


write() 的 EFAULT 预检已经改掉。现在先结束/确认上一轮 QEMU 状态，再重新构建 LoongArch 跑 libctest 验证。




上一轮旧内核还在跑，会被外层 6 分钟 timeout 收口。我等它退出后再跑新内核，避免两个 QEMU 同时抢资源。




旧内核仍在完整入口里往后跑，距离 6 分钟窗口应该快结束了。再等一段；如果还不收口，我会主动中断这轮旧日志。




旧日志已收口。现在构建 LoongArch 新内核，然后跑同样入口，看这几个 write Bad address 是否消失。




已运行 make build ARCH=loongarch








上下文已自动压缩



我先接着把已经定位到的 write() 误报 EFAULT 修复跑一轮 LoongArch 验证，确认这些 libctest 失败是不是同一根因；如果 tgmath 还单独失败，再继续拆它。性能这边我会先看 musl malloc 两个 bench 触发的是 brk 还是 mmap/munmap 路径，再决定优化点。




已列出文件已运行 2 条命令


工作区里目前只有我刚才动过的 sys_write() 预检，以及用户自己在 initcode-la.cc 里注释 LTP 的缩小入口。我会保留这个入口不动，直接用它跑 libctest 验证。




已运行 ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs
log="logs/output_l_${ts}_libctest-write-lazy-fix_timeout-6m.txt"
{
  printf 'run_at=%s\n' "$ts"
  printf 'arch=loongarch\n'
  printf 'scope=libctest-write-lazy-fix\n'
  printf 'cmd=timeout 6m make run l QEMU_MEM=1G\n'
  printf 'git_head=%s\n' "$(git rev-parse --short HEAD 2>/dev/null || true)"
  printf '%s\n' '---- output ----'
  timeout 6m make run l QEMU_MEM=1G
  printf 'exit_code=%s\n' "$?"
} > "$log" 2>&1
printf '%s\n' "$log"







QEMU 已经在后台跑，输出写到 logs/output_l_*_libctest-write-lazy-fix_timeout-6m.txt，不会把长日志刷进聊天。等这一轮结束后我会只抽取失败点和 Bad address 相关行。




这一轮还在跑。因为现在入口是完整 libc_test 加 basic_test，6 分钟窗口里如果前面几个失败消失，通常日志会继续推进到后续 case；我先等到进程自然结束或超时，再看结果。




还在运行，说明至少没有立刻卡在最早的 fscanf 写入错误上。下一步我会重点看同名 case 的 START/FAIL 区间，以及 tgmath 是否仍然独立报错。




QEMU 仍在推进，我会再给它一点时间；如果 6 分钟超时，我也能从日志判断是修复后继续跑到哪里、是否还卡在同一类 EFAULT。




这轮时间比之前复现日志长，倾向于 write() Bad address 的主问题已经松动了。等它退出后我会用 rg 拉出精确证据，不靠肉眼翻整份日志。




还在跑。libctest 的动态段本来会比较久，如果这次能越过 static/dynamic 那几个 pipe/write case，后面再转向 malloc bench 会更稳。




现在接近超时窗口的后半段了。我不会中途杀它，等 timeout 统一收尾，这样日志尾部会保留可复现的退出状态。




这轮还没有结束，说明完整 libc/basic 入口本身比较长。结束后如果只是跑到了更后面的 bench 或 basic 段超时，我会把验证范围再缩窄到用户列出的 case 和 malloc bench，避免每轮等全量。




QEMU 这一轮已经收尾了。我现在抽取 Bad address、用户列出的失败名和退出状态，先确认修复面，再处理剩下的。




Searched code已运行 1 条命令


结果很清楚：用户列出的 fscanf/daemon_failure/fgetwc_buffering/ftello_unflushed_append 已经都通过了，日志里也没有 Bad address。剩下的是 static/dynamic 两个 tgmath 仍然 status 1，我继续单独看这个。




Read a file已运行 1 条命令


tgmath 不是 VMA 这条线：static 是 pow(2.0,0.5) 按精确比较差了极小尾数，dynamic 是 lrint(123456789.1f) 返回 0。这更像 LoongArch 浮点上下文保存/恢复或 FPU 使能路径的问题，我先读对应源码和内核浮点处理。




Read a file和searched code已运行 1 条命令


目前看到 LoongArch 启动时只是全局打开了 FPU 扩展，但 trapframe/uservec 没明显保存浮点寄存器。这会让用户态浮点值在 syscall/调度后被破坏，lrint 返回 0 就很吻合。我继续确认 trapframe 布局和 uservec 保存范围。




Read 6 files和searched code


有个细节：static 的 pow 失败可能是 musl/LoongArch libm 一位尾数差；但 dynamic 的 lrint 直接 0 更像浮点状态或寄存器污染。因为 scoreboard 里这个 case 以前在 LoongArch/musl 是 PASS，所以我按“内核回归”继续查，而不是把它当测试误差。




Read 2 files和searched code已运行 1 条命令


现在确认到调度上下文切换只保存 GPR，FPU 完全裸奔。这个对 libctest 数学 case 很危险，但如果在每次 syscall 都保存 32 个 FPR 又会伤性能；更合理的是先在进程切换边界保存/恢复浮点上下文。




Read 5 files和searched code


我准备给 LoongArch 加一个很小的 FPU 上下文保存结构，挂在 Pcb 上，只在调度切换时保存/恢复，不碰 syscall 快路径。这样能修 tgmath 这种用户态浮点状态被其他任务覆盖的问题，也尽量不把 VMA/libcbench 的系统调用性能拉下去。




已运行 3 条命令


先用交叉编译器快速确认 LoongArch 汇编语法，避免直接把 FCSR/FPR 指令写进内核后才撞编译错误。这个只在 /tmp 里做临时语法探针，不改项目文件。




已运行 3 条命令


语法确认好了：fst.d/fld.d 和 movfcsr2gr/movgr2fcsr 都能由现有交叉编译器接受。接下来我会加 LoongArch 专用的 FpuContext 与保存/恢复汇编，并在调度器切换进出进程时调用。




Read 6 files


这里还有一个 exec 细节：新程序不是通过调度切回来，而是从 execve syscall 直接返回用户态，所以只在 swtch 里恢复还不够。成功 exec 时也要把当前硬件 FPU 和该进程保存上下文清成默认状态，否则新 ELF 会继承上一个用户程序的 FCSR。




Read 2 files和searched code


我会先做两个动作：swtch 保存/恢复 FPU，execve 成功后清零 FPU。fork 继承父 FPU 可以后续再补精细语义；当前这几个 libctest 都是 exec 新测试程序，先修最直接的污染源。




Read a file


我会同时补 fork/clone 的浮点继承：在 fork syscall 里先把当前硬件 FPU 状态落到父进程上下文，再拷给子进程。这样不是只为 tgmath 打补丁，而是把 LoongArch 用户态 FPU 语义补完整一点。




已编辑 1 个文件


Context 已经扩展了 LoongArch 的 FPU 保存区。现在改 swtch.S，让它在真正切换上下文时顺手保存/恢复 FPR 和 FCSR。




已编辑 1 个文件read a file


execve 和 fork 现在还不知道这些汇编 helper，我会把声明接上，并在成功 exec/fork 的关键位置维护 FPU 状态。




已编辑 1 个文件


代码已经接好了。现在先跑 LoongArch 构建，主要验证汇编偏移、FCSR 指令和 C++ 声明是否都正确。




已运行 make build ARCH=loongarch







LoongArch 构建通过，汇编和链接都没问题。现在重新跑同一个 libctest 入口，抽查 tgmath 和前面 EFAULT 修复是否一起稳定。




已运行 ts=$(date +%Y%m%d-%H%M%S)
mkdir -p logs
log="logs/output_l_${ts}_libctest-fpu-write-fix_timeout-6m.txt"
{
  printf 'run_at=%s\n' "$ts"
  printf 'arch=loongarch\n'
  printf 'scope=libctest-fpu-write-fix\n'
  printf 'cmd=timeout 6m make run l QEMU_MEM=1G\n'
  printf 'git_head=%s\n' "$(git rev-parse --short HEAD 2>/dev/null || true)"
  printf '%s\n' '---- output ----'
  timeout 6m make run l QEMU_MEM=1G
  printf 'exit_code=%s\n' "$?"
} > "$log" 2>&1
printf '%s\n' "$log"







新内核已经启动运行，日志写到 logs/output_l_*_libctest-fpu-write-fix_timeout-6m.txt。我等它结束后会只抽取 tgmath 和 Bad address 相关证据。




这一轮仍在推进，没有早期崩溃。FPU 保存/恢复加在 context switch 上，若有性能影响，主要会体现在进程/线程切换类测试；malloc bench 应该不直接吃这部分成本。




还在跑。等这轮确认后，我会把入口缩到 bench 或直接利用已有 libcbench-only 入口，开始看 b_malloc_sparse/b_malloc_bubble 的慢点。




这轮已经越过前面的短 case 时间点了。我会让 6 分钟窗口走完，确保不是修了一个 case 又在后续基础测试里引入新异常。




还在运行，状态正常。FPU 修复如果生效，日志里 static/dynamic tgmath 应该只剩 START/END，不再有 FAIL 行。




接近超时窗口后半段了。这个入口后面会进入 iozone/basic，6 分钟超时本身不代表 libctest 失败，我会看具体推进点。




仍在跑。上一轮也是跑到 iozone 后被 timeout 截断，所以这里我重点比较前 600 行 libctest 段，不会被后面长 bench 干扰判断。




快到 6 分钟了。结束后我会马上抽日志；如果 tgmath 还失败，就说明还不是 FPU 上下文污染，需要继续看 LoongArch libm/指令模拟或测试镜像差异。




QEMU 收尾了。现在抽取 tgmath、Bad address、FAIL 和尾部退出状态。




Searched code已运行 1 条命令


FPU 上下文保存没有改变 tgmath，所以它不是这两个 tgmath 失败的主因。为了不无谓增加 context switch 成本，我会把这组 FPU 调度保存改动撤掉，只保留有明确收益的 write() 懒缺页修复，然后继续定位 tgmath 的真实来源。




先收回刚才无效的 FPU 试探改动，避免把“为了修测试”变成“凭空拉低调度性能”。我只删自己刚加的片段，不碰已有 VMA 优化和用户入口改动。




已编辑 1 个文件


Context 已恢复原大小。接着删掉 swtch.S 里刚加的 FPR/FCSR 保存和 helper。




已编辑 1 个文件


swtch.S 已恢复到只保存内核上下文寄存器。最后删掉 proc_manager.cc 里的 FPU helper 声明和 fork/exec 调用。




已编辑 1 个文件


无效的 FPU 改动已经撤干净。tgmath 继续失败，所以我会改看测试镜像/二进制和 LoongArch 用户态 ABI：尤其是 lrint 返回 0 这种更像指令、动态链接或测试二进制本身差异。




Searched code已运行 4 条命令


测试程序都在 images/sdcard-la.img 里，不在工作区普通目录。我会用 debugfs 只读列目录/抽文件，不挂载也不改镜像。




已运行 3 条命令


镜像结构比较简单：entry-static.exe、entry-dynamic.exe、runtest.exe 直接在 /musl 下，case 名由 runtest 选择，不是每个 case 一个独立文件。我会把这几个只读导出到 /tmp，用 Linux 的 qemu-user 跑同一个 tgmath，判断是内核问题还是镜像自带二进制行为。




已运行 rm -rf /tmp/f7ly-la-musl && mkdir -p /tmp/f7ly-la-musl/lib
printf '%s\n' \
  'dump /musl/runtest.exe /tmp/f7ly-la-musl/runtest.exe' \
  'dump /musl/entry-static.exe /tmp/f7ly-la-musl/entry-static.exe' \
  'dump /musl/entry-dynamic.exe /tmp/f7ly-la-musl/entry-dynamic.exe' \
  'dump /musl/lib/libc.so /tmp/f7ly-la-musl/lib/libc.so' \
  'dump /musl/lib/dlopen_dso.so /tmp/f7ly-la-musl/lib/dlopen_dso.so' \
  'dump /musl/lib/tls_align_dso.so /tmp/f7ly-la-musl/lib/tls_align_dso.so' \
  'dump /musl/lib/tls_init_dso.so /tmp/f7ly-la-musl/lib/tls_init_dso.so' \
  'dump /musl/lib/tls_get_new-dtv_dso.so /tmp/f7ly-la-musl/lib/tls_get_new-dtv_dso.so' \
  | debugfs -f - images/sdcard-la.img >/tmp/f7ly-debugfs-dump.log 2>&1
chmod +x /tmp/f7ly-la-musl/runtest.exe /tmp/f7ly-la-musl/entry-static.exe /tmp/f7ly-la-musl/entry-dynamic.exe /tmp/f7ly-la-musl/lib/libc.so
find /tmp/f7ly-la-musl -maxdepth 3 -type f -ls
printf '%s\n' '--- debugfs ---'
sed -n '1,120p' /tmp/f7ly-debugfs-dump.log







现在用同一份镜像里的程序，在宿主机 qemu-user 下跑 runtest.exe -w entry-*.exe tgmath。如果这里也失败，tgmath 就不是这次 VMA 移植引入的内核回归；如果宿主通过，才继续查我们的 LoongArch 内核 ABI。




已运行 3 条命令


宿主 qemu-user 下，镜像里的 static tgmath 是 Pass 的；说明 static 的 pow 一位差是我们内核运行环境触发的，不是二进制天然失败。dynamic 通过 runtest 子 exec 时 qemu-user 没吃到 -L，我会再直接跑 entry 来确认动态版本。




已运行 3 条命令


两个 entry 在宿主 qemu-user 下都 Pass，说明内核侧确实还在影响浮点结果。下一个怀疑点是：内核 C/C++ 是否在 trap 期间悄悄使用了 FPR，或者某些用户态 trap/页故障路径改变了浮点 CSR。先看内核反汇编里有没有非我们手写的浮点指令。




已运行 3 条命令


关键线索出来了：内核镜像里确实有浮点指令，不只是用户程序。也就是说用户态进入内核后，内核某些 C/C++ 代码会改 FPR/FCSR，而 LoongArch trap 没保存用户 FPU；这会污染 tgmath。我先定位这些浮点指令来自哪些内核函数，再做局部修复，避免全局重保存。




已运行 loongarch64-linux-gnu-addr2line -e kernel-la -f -C 0x9000000000201c84 0x9000000000201cb0 0x900000000023ecb8 0x900000000023edc4 0x90000000002c6730 0x90000000002ca598 0x90000000002cbd14 | sed -n '1,80p'







其中一大串浮点指令其实是被 incbin 到内核里的用户 initcode，反汇编会看见但不是内核执行路径。真正的内核浮点目前集中在 kernel/libs/klib.cc::ceilf 和 shm::print_memory_status()；我会看这些是否在 libctest 前被调用。




Read 2 files和searched code已运行 1 条命令


ceilf/shm 的内核浮点目前看起来没有被这些 case 直接调用，所以还不能收口。dynamic 的 lrintf 是第一个 PLT math 调用，失败为 0 也可能是 LoongArch 动态重定位/GOT 或 ELF 装载细节；我会对比 static/dynamic 的 tgmath_main 和重定位项。




已运行 4 条命令


现在更像 VMA/ELF 懒加载读入的某些代码或常量页内容不精确：同一二进制宿主 Pass，但内核里 static/dynamic 都在 math 常量/函数路径异常。接下来我会看 execve 注册懒文件段和 allocate_vma_page() 的 offset/零填充逻辑，尤其是 LoongArch 0x4000 ELF 对齐但内核 4K 页的组合。




Read 5 files和searched code


FileVmObject 的 offset 逻辑表面上是对的：按 area.page_offset + page_index*PGSIZE 读，并且先清零短页。下一步我会用更小的运行入口只跑 tgmath，加一点临时诊断去核对动态 lrintf 的 PLT/GOT 是否被解析到 libc 的真实地址、以及相关页内容是否按预期读入。




Read 3 files和searched code已运行 1 条命令


为了判断“污染还是首跑就坏”，我会临时把 LoongArch initcode 缩成只跑 static/dynamic tgmath 两条，跑完会恢复入口，不把这个调试改动留在最终工作区。




已编辑 1 个文件


入口已经临时缩小。现在构建并跑一个短窗口，只看 tgmath 首跑结果。




已运行 2 条命令


短入口已经跑完了，我抽日志看首跑结果。




Read a file和searched code


首跑也失败，所以不是前面 case 污染。static 内联/静态 lrint 能过，但 dynamic 第一个 lrintf@plt 返回 0，这把 dynamic 问题指向 PLT/动态解析或装载；static 的 pow 则像 FPU 初始模式/硬件特性或静态 libm 细节。先把 dynamic PLT 看清楚。




Read a file已运行 3 条命令



上下文已自动压缩



我先接着上一轮现场做一次差异盘点，尤其确认哪些是有效修复、哪些只是临时调试入口。然后我会把 tgmath 的问题缩到可解释的根因，再去看 malloc bench 的慢路径。




Read 2 files已运行 3 条命令


差异里有两类东西：sys_write 的懒缺页修复是要保留的；initcode-la.cc 现在是我上一轮为了锁定 tgmath 做的短入口，后面会恢复成你原来的长回归入口并保留 LTP 注释。现在我先读一下 plan 文档和相关实现，把下一步诊断点落在最小范围。




Read 4 files和searched code


我重新接上后先做一个状态快照：确认上一轮压缩前那次短 bench 是否已经落日志、当前还有哪些探针/入口没收。然后我会先把可明确保留的 bugfix 固化，把临时东西逐项清掉。




已列出文件已运行 3 条命令


当前还有不少 VMA 性能改动已经在树里，不全是临时探针；我不会粗暴回滚。先把探针标记搜出来，分清“优化代码”和“调试输出”，再继续 tgmath。




Searched code已运行 2 条命令


Bad address 的根因已经很清楚，保留 sys_write 懒缺页修复即可。tgmath 现在是独立首跑就失败，我会转到 LoongArch 浮点/ELF 装载路径查；先看 trapframe、用户态入口、ELF auxv 和动态链接相关代码。




Read 4 files和searched code


检索里出现 LoongArch trap 里还有针对 entry-static 的 debug panic 区间，这类不是 tgmath 直接原因，但属于调试残留风险。我先看完整上下文，确认是否会影响当前 case。




Read 5 files


这里有一个更合理的根因：LoongArch 的 user trap 只保存了通用寄存器，没保存用户 FPU 寄存器。tgmath 的 lrint/pow 全靠 FPR 传参/中间值，任何 timer trap 或调度都会把用户浮点现场裸露出来；上一轮试过“只在内核线程 context switch 保存 FPU”无效，说明应该补在 uservec/trapframe 层。




Searched code已运行 2 条命令


我会把 FPU 状态放进 LoongArch 的 TrapFrame，并在 uservec/userret 每次进出内核保存/恢复 32 个 FPR、FCSR 和 FCC0-7。这样 syscall、timer、调度都不会破坏用户态浮点中间值，这比在内核 context switch 里补更贴近问题位置。




Read 4 files和searched code


alloc_proc() 里 trapframe 是一整页，空间足够加 FPU 保存区。我会顺手在分配后清零，避免新进程的 FCSR/FCC/FPR 从旧 PCB 残留继承。




Read a file和searched code


开始改 LoongArch trapframe/FPU 保存。这里是架构层通用修复，不是 tgmath 特判：只要用户态使用浮点，系统调用和时钟中断都应该保住 FPU 上下文。




已编辑 1 个文件


TrapFrame 结构已扩展。接下来改 uservec.S，让每次 trap 入口保存 FPR/FCSR/FCC，返回用户态前恢复它们。




正在编辑 1 个文件正在编辑 1 个文件


FPU 保存/恢复代码已经接上。现在先编 LoongArch，主要看汇编语法、TrapFrame 大小和信号相关编译有没有被影响。




已运行 make build ARCH=loongarch







LoongArch 构建通过。现在用当前 tgmath-only 短入口跑一次，直接看 static/dynamic tgmath 是否转 PASS。




已运行 ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_tgmath-fpu-trapframe_timeout-120s.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=tgmath-fpu-trapframe"; echo "cmd=timeout 120s make run l QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 120s make run l QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







这轮很快结束，不像正常进到用户态测试；我先抽日志头尾确认是不是启动早期炸了。




Read a file和searched code


这次日志里 QEMU 被启动后立刻返回且没有内核输出，像是 make run l 的目标组合没有按预期跑到新构建产物。我检查 Makefile 的 run 目标，用明确目标重跑，避免被 l 这个构建目标吞掉判断。




Read a file和searched code已运行 1 条命令


奇怪点确认了：QEMU 这次是“无任何 guest 输出就正常退出”。我用 run-loongarch 明确目标再跑 5 秒窗口，确认这是新内核启动早退，还是 make 目标组合造成的假象。




已运行 ts=$(date +%Y%m%d-%H%M%S); log="logs/output_l_${ts}_direct-runloongarch-smoke_timeout-5s.txt"; { echo "run_at=${ts}"; echo "cmd=timeout 5s make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "---- output ----"; timeout 5s make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







直接目标也立即退出。我会打开日志尾部和必要时直接执行一次 QEMU 看返回码，优先排除“启动参数/镜像打开失败”这类外部因素。




Read a file已运行 2 条命令


QEMU 直接执行新 kernel-la 也是 0.13 秒退出，说明不是 make 假象。这个现象很像早期启动走到 firmware/机器关机路径前没输出。我先对比上一份可运行日志和当前内核入口，看看是否是构建产物/启动参数本身异常。




Read 2 files和searched code已运行 2 条命令


找到一个很具体的错误：FPU restore 放在 GPR restore 之后，又拿 $t0 当 scratch，等于每次返回用户态都把用户 $t0 改坏了。这个会让用户态控制流/测试输出直接乱掉。我把 $t0 在 FPU restore 后再恢复一次，保持 trap 对 GPR 的透明性。




已编辑 1 个文件


修正后重新编译 LoongArch，再跑 tgmath-only 短入口。这个点如果对了，日志应该重新出现 START/END，并且 tgmath 结果会改变。




已运行 make build ARCH=loongarch







构建过了。现在重跑 120 秒 tgmath-only。




已运行 ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_tgmath-fpu-trapframe-t0fix_timeout-120s.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=tgmath-fpu-trapframe-t0fix"; echo "cmd=timeout 120s make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 120s make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







还是立即退出。我会看日志和用 objdump 检查 userret 实际顺序；如果用户态仍能 shutdown 但没有测试输出，说明 GPR 之外还有被返回路径破坏的状态。




Read a file已运行 2 条命令


好，tgmath 这次 actually 已经转成 static/dynamic 都 Pass! 了，上一句“立即退出”只是我先看到运行时间短，日志里确认是正常跑完并 shutdown。接下来我临时把入口切成“这 5 个 libctest 定向用例”，验证 Bad address 修复和 FPU 修复能同时成立。




Read 2 files和searched code


我临时把 LA initcode 改成五个问题 libctest 的 static/dynamic 定向列表：fscanf/daemon_failure/fgetwc_buffering/ftello_unflushed_append/tgmath。跑完会恢复，不把这个短入口留下。




已编辑 1 个文件


入口已切好。现在构建并跑 3 分钟窗口，预期这 10 个组合都应只有 START/END 或 Pass，不再出现 Bad address/tgmath FAIL。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libctest-targeted-write-fpu_timeout-180s.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libctest-targeted-write-fpu"; echo "cmd=timeout 180s make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 180s make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







定向 libctest 跑完了，我抽关键行看是否还有 FAIL/Bad address。




Read a file和searched code


功能问题已经闭环：五个 libctest 的 static/dynamic 都 Pass。下面转性能：我会先读 libcbench 脚本和源码，确认 b_malloc_sparse/bubble 的行为是 brk 还是 mmap/munmap 压力，再跑一个 musl-only bench 得到当前数值。




Read 3 files已运行 3 条命令


b_malloc_sparse/bubble 是 10000 次 malloc(4000)+memset 再释放大部分块，确实主要打在 musl malloc 的 brk/mmap、缺页和释放路径上；glibc 快很多很可能有 allocator 策略/cache 差异，但内核不能把 musl 的 4K 分配放大到秒级。现在我切入口跑 libcbench-musl，看当前优化后的真实数值。




已编辑 1 个文件


入口已切到 musl libcbench。构建并跑 8 分钟窗口，先看 malloc 两项和整体是否异常。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libcbench-musl-after-vma-fpu_timeout-8m.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libcbench-musl-after-vma-fpu"; echo "cmd=timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







musl libcbench 跑完了，开始抽 malloc 两项和后面的 pthread 数字，看看当前性能有没有回落到可接受区间。




Read a file和searched code


当前已经从你给的 7.6/8.6 秒降到 6.1/7.3 秒，但还不够。pthread 已经很快，说明这次慢点集中在 4K malloc 触发的页级路径。为了回答“是不是 cache”，我再跑 glibc 对照；如果 glibc 仍快很多，就更像 allocator 策略差异叠加内核 brk/缺页慢路径。




已编辑 1 个文件


入口切到 glibc libcbench，跑一轮对照。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libcbench-glibc-after-vma-fpu_timeout-8m.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libcbench-glibc-after-vma-fpu"; echo "cmd=timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







glibc 对照跑完了，我抽 malloc 段。




Searched code


glibc 的两个 4K malloc 项大约 0.10s，而 musl 仍是 6-7s，所以“cache/allocator 策略”确实是差异的一部分；但内核侧还可能把 musl 的 syscall/缺页模式放大了。我用 qemu-user -strace 跑同一个 libc-bench，先确认 musl 对 4000 字节到底走多少 brk/mmap/munmap。




已运行 rm -rf /tmp/f7ly-la-libcbench && mkdir -p /tmp/f7ly-la-libcbench/musl/lib /tmp/f7ly-la-libcbench/glibc/lib
printf '%s\n' \
  'dump /musl/libc-bench /tmp/f7ly-la-libcbench/musl/libc-bench' \
  'dump /musl/lib/libc.so /tmp/f7ly-la-libcbench/musl/lib/libc.so' \
  'dump /glibc/libc-bench /tmp/f7ly-la-libcbench/glibc/libc-bench' \
  'dump /glibc/lib/ld-linux-loongarch-lp64d.so.1 /tmp/f7ly-la-libcbench/glibc/lib/ld-linux-loongarch-lp64d.so.1' \
  'dump /glibc/lib/libc.so.6 /tmp/f7ly-la-libcbench/glibc/lib/libc.so.6' \
  | debugfs -f - images/sdcard-la.img >/tmp/f7ly-debugfs-libcbench.log 2>&1
chmod +x /tmp/f7ly-la-libcbench/musl/libc-bench /tmp/f7ly-la-libcbench/musl/lib/libc.so /tmp/f7ly-la-libcbench/glibc/libc-bench /tmp/f7ly-la-libcbench/glibc/lib/ld-linux-loongarch-lp64d.so.1 /tmp/f7ly-la-libcbench/glibc/lib/libc.so.6
file /tmp/f7ly-la-libcbench/musl/libc-bench /tmp/f7ly-la-libcbench/glibc/libc-bench
loongarch64-linux-gnu-readelf -l /tmp/f7ly-la-libcbench/musl/libc-bench | rg 'interpreter|Requesting|INTERP' -n || true
loongarch64-linux-gnu-readelf -l /tmp/f7ly-la-libcbench/glibc/libc-bench | rg 'interpreter|Requesting|INTERP' -n || true







两个 libc-bench 都是静态 ELF，不涉及动态链接器。接下来跑 qemu-user strace 到 /tmp，只统计 brk/mmap/munmap/mremap 等调用次数，不把长 trace 打进聊天。




已运行 cd /tmp/f7ly-la-libcbench/musl && timeout 30s qemu-loongarch64-static -strace ./libc-bench > /tmp/f7ly-musl-libcbench.out 2> /tmp/f7ly-musl-libcbench.strace; printf 'musl_exit=%s\n' "$?"
cd /tmp/f7ly-la-libcbench/glibc && timeout 30s qemu-loongarch64-static -strace ./libc-bench > /tmp/f7ly-glibc-libcbench.out 2> /tmp/f7ly-glibc-libcbench.strace; printf 'glibc_exit=%s\n' "$?"
printf 'musl syscall counts:\n'; rg -o '^[0-9]+ [a-zA-Z0-9_]+' /tmp/f7ly-musl-libcbench.strace | awk '{print $2}' | sort | uniq -c | sort -nr | head -n 30
printf 'glibc syscall counts:\n'; rg -o '^[0-9]+ [a-zA-Z0-9_]+' /tmp/f7ly-glibc-libcbench.strace | awk '{print $2}' | sort | uniq -c | sort -nr | head -n 30
printf 'musl first output:\n'; sed -n '1,30p' /tmp/f7ly-musl-libcbench.out
printf 'glibc first output:\n'; sed -n '1,30p' /tmp/f7ly-glibc-libcbench.out







qemu-user 上 musl 的 sparse/bubble 只有 0.02s 量级，说明二进制/算法本身没问题；F7LY 里 6-7s 是内核路径放大。下一步我加临时计数，不逐次打印，只在 libc-bench 子进程退出时汇总 brk/mmap/munmap/缺页次数和耗时，找真正热段。




Read 3 files和searched code


代码里有个 LA 专属重成本点：mmap() 对映射范围逐页 walk(va, true) 预建页表层级。musl 对 4K malloc 会非常频繁 mmap，小映射多时这个循环和 VMA 查找会被放大。先用计数确认是不是 mmap/munmap/fault，而不是盲改。




Searched code


为了避免污染长期代码，我只加临时 stats helper，并且只在 _name == "libc-bench" 时计数/打印。跑出数据后立刻撤。




Read 2 files和searched code


我先只计 syscall 层：每个 libc-bench 子进程的 brk/mmap/munmap 次数、字节数和 rdtime 周期。若这三项解释不了 6 秒，再往缺页/清零路径加第二层计数。




已编辑 1 个文件


stats helper 已加。继续把 exit 打印和 brk/mmap/munmap 计数接进去。




正在编辑 1 个文件正在编辑 1 个文件


patch 上下文偏了，我分小块接，避免误伤 syscall 周边的大文件。




Read a file和searched code


定位到准确行号了，继续接计数。




已编辑 1 个文件


syscall 计数已接上。我把入口切回 musl libcbench，重跑一轮会多出 [LIBCBENCH_SYS] 汇总行。




已编辑 1 个文件


开始构建并跑带 syscall 统计的 musl libcbench。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libcbench-musl-sysstats_timeout-8m.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libcbench-musl-sysstats"; echo "cmd=timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







编译只差命名空间：num_process 在 proc:: 里。我修一下再跑。




已编辑 1 个文件


命名空间修好，重新构建运行。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libcbench-musl-sysstats_timeout-8m.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libcbench-musl-sysstats"; echo "cmd=timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







带统计的跑完了，我抽 [LIBCBENCH_SYS] 和 malloc 时间。




Searched code


没有统计行，说明名字过滤没命中或 libcbench 的进程名不是我预期的 libc-bench。我把临时计数放宽到所有进程，但只有 brk/mmap/munmap 非零才打印，这轮日志会多一些，不过仍然可控。




已编辑 1 个文件


放宽后重跑一次；这次应该能看到每个子进程退出前的 mmap/munmap 画像。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libcbench-musl-sysstats-allproc_timeout-8m.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libcbench-musl-sysstats-allproc"; echo "cmd=timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







再抽统计行。




Searched code


仍没有统计，说明不是名字过滤问题，而是计数本身为 0 或退出路径没经过这两个 syscall 包装。加一行临时 exit 画像确认：进程退出时到底走没走 sys_exit/sys_exit_group，进程名是什么。




已编辑 1 个文件


跑一个短窗口确认退出路径和名字。这轮日志会有临时 [EXIT_DBG]，后面会删。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libcbench-musl-exitdbg_timeout-8m.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libcbench-musl-exitdbg"; echo "cmd=timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







抽 exit debug。




Searched code


exit hook 没被触发，说明这些静态程序的退出路径可能不是走 sys_exit/sys_exit_group 这两个 C++包装，或者 syscall 表绑定到了别的实现。这个方向先停，我改用更底层的 syscall 分发点按 syscall number 计数，那里一定会经过。




Read 3 files和searched code


内核普通 printf 在这个运行模式下基本不出现在串口日志（之前 boot printf 也没有），所以临时统计不是没执行，而是看不到。改用 shutdown() 前 panic 打一行聚合 summary；panic 输出能进日志，跑完就撤。




Read a file和searched code


我在 shutdown 前临时 panic 汇总全局计数，这样即使普通 kernel printf 不出串口，也能拿到数据。




正在编辑 1 个文件read a file正在编辑 1 个文件read a file


上下文确认了，插入 summary helper。




已编辑 1 个文件


接到 LoongArch shutdown 前。




已编辑 1 个文件


现在跑一轮，预期最后 panic 打 [LIBCBENCH_SYS_SUM]。




已运行 make build ARCH=loongarch && ts=$(date +%Y%m%d-%H%M%S); mkdir -p logs; log="logs/output_l_${ts}_libcbench-musl-sysstats-panic_timeout-8m.txt"; { echo "run_at=${ts}"; echo "arch=loongarch"; echo "scope=libcbench-musl-sysstats-panic"; echo "cmd=timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G"; echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"; echo "---- output ----"; timeout 8m make run-loongarch ARCH=loongarch QEMU_MEM=1G; echo "exit_code=$?"; } > "$log" 2>&1; echo "$log"







已处理 1h 4m 45s




抽 panic summary。

---

2026-06-19 已完成待验收：libctest-musl 全量通过，日志为 `logs/output_l_20260619-105156_libctest-full-clean_timeout-8m.txt`；最终入口窗口确认 libctest 分组完整结束，日志为 `logs/output_l_20260619-105351_final-entry-libctest-window_timeout-3m.txt`。
