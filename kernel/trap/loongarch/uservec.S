# code to switch between user and kernel space.

	# kernel.ld causes this to be aligned
        # to a page boundary.
        #
        #ifdef LOONGARCH
#include "asm.hh"

.section u_eentry
.globl uservec
.align 4

uservec:
	#
        # trap.c sets eentry to point here, so
        # traps from user space start here,
        # in privilege0, but with a
        # user page table.
        #
        # CSR.SAVE points to where the process's p->trapframe is
        # mapped into user space at a per-thread virtual address.
        #

	# swap a0 and SAVE
        # so that a0 is the current thread's trapframe address
        csrwr  $a0, LOONGARCH_CSR_SAVE0

        # save the user registers in the current thread's trapframe
        st.d   $ra, $a0, 0
        st.d   $tp, $a0, 8
        st.d   $sp, $a0, 16
        st.d   $a1, $a0, 32
        st.d   $a2, $a0, 40
        st.d   $a3, $a0, 48
        st.d   $a4, $a0, 56
        st.d   $a5, $a0, 64
        st.d   $a6, $a0, 72
        st.d   $a7, $a0, 80
        st.d   $t0, $a0, 88
        st.d   $t1, $a0, 96
        st.d   $t2, $a0, 104
        st.d   $t3, $a0, 112
        st.d   $t4, $a0, 120
        st.d   $t5, $a0, 128
        st.d   $t6, $a0, 136
        st.d   $t7, $a0, 144
        st.d   $t8, $a0, 152
        st.d   $r21, $a0,160
        st.d   $fp, $a0, 168
        st.d   $s0, $a0, 176
        st.d   $s1, $a0, 184
        st.d   $s2, $a0, 192
        st.d   $s3, $a0, 200
        st.d   $s4, $a0, 208
        st.d   $s5, $a0, 216
        st.d   $s6, $a0, 224
        st.d   $s7, $a0, 232
        st.d   $s8, $a0, 240

        # 懒 FPU：只有当前线程已经启用 EUEN.FPE 时才保存浮点现场。
        # 纯整数/内存类程序频繁 syscall/page fault 时不能为 32 个 FPR 付固定成本。
        csrrd  $t0, LOONGARCH_CSR_EUEN
        andi   $t0, $t0, 0x1
        beqz   $t0, 1f

        fst.d  $f0,  $a0, 288
        fst.d  $f1,  $a0, 296
        fst.d  $f2,  $a0, 304
        fst.d  $f3,  $a0, 312
        fst.d  $f4,  $a0, 320
        fst.d  $f5,  $a0, 328
        fst.d  $f6,  $a0, 336
        fst.d  $f7,  $a0, 344
        fst.d  $f8,  $a0, 352
        fst.d  $f9,  $a0, 360
        fst.d  $f10, $a0, 368
        fst.d  $f11, $a0, 376
        fst.d  $f12, $a0, 384
        fst.d  $f13, $a0, 392
        fst.d  $f14, $a0, 400
        fst.d  $f15, $a0, 408
        fst.d  $f16, $a0, 416
        fst.d  $f17, $a0, 424
        fst.d  $f18, $a0, 432
        fst.d  $f19, $a0, 440
        fst.d  $f20, $a0, 448
        fst.d  $f21, $a0, 456
        fst.d  $f22, $a0, 464
        fst.d  $f23, $a0, 472
        fst.d  $f24, $a0, 480
        fst.d  $f25, $a0, 488
        fst.d  $f26, $a0, 496
        fst.d  $f27, $a0, 504
        fst.d  $f28, $a0, 512
        fst.d  $f29, $a0, 520
        fst.d  $f30, $a0, 528
        fst.d  $f31, $a0, 536
        movfcsr2gr $t0, $fcsr0
        st.d   $t0, $a0, 544
        movcf2gr $t0, $fcc0
        st.d   $t0, $a0, 552
        movcf2gr $t0, $fcc1
        st.d   $t0, $a0, 560
        movcf2gr $t0, $fcc2
        st.d   $t0, $a0, 568
        movcf2gr $t0, $fcc3
        st.d   $t0, $a0, 576
        movcf2gr $t0, $fcc4
        st.d   $t0, $a0, 584
        movcf2gr $t0, $fcc5
        st.d   $t0, $a0, 592
        movcf2gr $t0, $fcc6
        st.d   $t0, $a0, 600
        movcf2gr $t0, $fcc7
        st.d   $t0, $a0, 608
1:
        # 从这里开始要运行内核 C/C++ 代码。内核以 lp64d ABI 编译，不能把 FPE
        # 长期保持为用户态的禁用状态；否则 syscall 路径里的浮点指令会在内核态 FPD。
        csrrd  $t0, LOONGARCH_CSR_EUEN
        ori    $t0, $t0, 0x1
        csrwr  $t0, LOONGARCH_CSR_EUEN

	# save the user a0 in p->trapframe->a0
        csrrd  $t0, LOONGARCH_CSR_SAVE0
        st.d   $t0, $a0, 24

        # restore kernel stack pointer from p->trapframe->kernel_sp
        ld.d   $sp, $a0, 248

        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld.d   $tp, $a0, 272

        # load the address of usertrap(), p->trapframe->kernel_trap
        ld.d   $t0, $a0, 256

        # restore kernel page table from p->trapframe->kernel_pgdl
        ld.d   $t1, $a0, 280
        csrwr  $t1, LOONGARCH_CSR_PGDL
        invtlb 0x0,$zero,$zero
	    li.d   $fp, 0

	    # jump to usertrap(), which does not return
	    jirl   $zero ,$t0, 0

.globl userret
userret:
        # userret(trapframe_va, pagetable, used_fpu, kernel_trapframe)
        # switch from kernel to user.
        # usertrapret() calls here.
        # a0: current thread's trapframe VA, in user page table.
        # a1: user page table, for pgdl.
        # a2: whether this thread has enabled user FPU state.
        # a3: current thread trapframe's kernel direct-map address.
        #
        # CLONE_VM 线程共享用户 PGDL。即便每个线程有专属 trapframe VA，
        # 在切换 PGDL 与 TLB refill 的极窄窗口里，也不应让“恢复用户寄存器”
        # 依赖这个用户虚拟翻译：一旦命中历史翻译，a0/s 寄存器会一起恢复为
        # 另一个现场。a0 仍只用于保存下一次 uservec 所需的专属 VA；所有实际
        # trapframe 读取均通过 a3 的内核直映地址完成。

        # switch to the user page table.
        csrwr  $a1, LOONGARCH_CSR_PGDL
        invtlb 0x0,$zero,$zero

        # 内核态始终保留 FPE 可用；临近 ertn 时才按当前线程状态设置
        # 用户可见的 EUEN.FPE。未使用浮点的线程让第一条浮点指令触发 FPD。
        csrrd  $t0, LOONGARCH_CSR_EUEN
        beqz   $a2, 3f
        ori    $t0, $t0, 0x1
        b      4f
3:
        li.w   $t1, -2
        and    $t0, $t0, $t1
4:
        csrwr  $t0, LOONGARCH_CSR_EUEN

        # a0 进入时是本线程的 user trapframe VA。下一次从用户态陷入时，
        # uservec 需要在 SAVE0 中拿到这个地址；但系统调用返回值不能再绕经
        # SAVE0 暂存：调度期间该 CSR 会被同核其它任务用于自己的 trap 入口，
        # 读取旧值再交换回来会把不属于当前线程的 a0 带回用户态。
        #
        # 先从当前 trapframe 读取用户 a0 到 t0，再把 VA 写入 SAVE0，随后把
        # a0 直接恢复为 t0。t0 保留到所有其它寄存器恢复完成后才还原，因此
        # 这里不依赖 SAVE0 先前保存的任何值。
        ld.d   $t0, $a3, 24
        csrwr  $a0, LOONGARCH_CSR_SAVE0
        addi.d $a0, $t0, 0

        # restore all but a0 from the current thread's trapframe
        ld.d   $ra, $a3, 0
        ld.d   $tp, $a3, 8
        ld.d   $sp, $a3, 16
        ld.d   $a1, $a3, 32
        ld.d   $a2, $a3, 40
        # a3 同时是内核直映 trapframe 基址，必须最后恢复用户 a3。
        ld.d   $a4, $a3, 56
        ld.d   $a5, $a3, 64
        ld.d   $a6, $a3, 72
        ld.d   $a7, $a3, 80
        ld.d   $t0, $a3, 88
        ld.d   $t1, $a3, 96
        ld.d   $t2, $a3, 104
        ld.d   $t3, $a3, 112
        ld.d   $t4, $a3, 120
        ld.d   $t5, $a3, 128
        ld.d   $t6, $a3, 136
        ld.d   $t7, $a3, 144
        ld.d   $t8, $a3, 152
        ld.d   $r21, $a3,160
        ld.d   $fp, $a3, 168
        ld.d   $s0, $a3, 176
        ld.d   $s1, $a3, 184
        ld.d   $s2, $a3, 192
        ld.d   $s3, $a3, 200
        ld.d   $s4, $a3, 208
        ld.d   $s5, $a3, 216
        ld.d   $s6, $a3, 224
        ld.d   $s7, $a3, 232
        ld.d   $s8, $a3, 240

        # 懒 FPU：usertrapret() 已经按当前线程的 _used_fpu 设置 EUEN.FPE。
        # 没用过浮点的线程直接跳过恢复，让首次浮点指令触发 FPD 异常后再启用。
        csrrd  $t0, LOONGARCH_CSR_EUEN
        andi   $t0, $t0, 0x1
        beqz   $t0, 2f

        fld.d  $f0,  $a3, 288
        fld.d  $f1,  $a3, 296
        fld.d  $f2,  $a3, 304
        fld.d  $f3,  $a3, 312
        fld.d  $f4,  $a3, 320
        fld.d  $f5,  $a3, 328
        fld.d  $f6,  $a3, 336
        fld.d  $f7,  $a3, 344
        fld.d  $f8,  $a3, 352
        fld.d  $f9,  $a3, 360
        fld.d  $f10, $a3, 368
        fld.d  $f11, $a3, 376
        fld.d  $f12, $a3, 384
        fld.d  $f13, $a3, 392
        fld.d  $f14, $a3, 400
        fld.d  $f15, $a3, 408
        fld.d  $f16, $a3, 416
        fld.d  $f17, $a3, 424
        fld.d  $f18, $a3, 432
        fld.d  $f19, $a3, 440
        fld.d  $f20, $a3, 448
        fld.d  $f21, $a3, 456
        fld.d  $f22, $a3, 464
        fld.d  $f23, $a3, 472
        fld.d  $f24, $a3, 480
        fld.d  $f25, $a3, 488
        fld.d  $f26, $a3, 496
        fld.d  $f27, $a3, 504
        fld.d  $f28, $a3, 512
        fld.d  $f29, $a3, 520
        fld.d  $f30, $a3, 528
        fld.d  $f31, $a3, 536
        ld.d   $t0, $a3, 544
        movgr2fcsr $fcsr0, $t0
        ld.d   $t0, $a3, 552
        movgr2cf $fcc0, $t0
        ld.d   $t0, $a3, 560
        movgr2cf $fcc1, $t0
        ld.d   $t0, $a3, 568
        movgr2cf $fcc2, $t0
        ld.d   $t0, $a3, 576
        movgr2cf $fcc3, $t0
        ld.d   $t0, $a3, 584
        movgr2cf $fcc4, $t0
        ld.d   $t0, $a3, 592
        movgr2cf $fcc5, $t0
        ld.d   $t0, $a3, 600
        movgr2cf $fcc6, $t0
        ld.d   $t0, $a3, 608
        movgr2cf $fcc7, $t0
2:
        # 上面的 EUEN 检查和 FCSR/FCC 恢复使用了 $t0 作为 scratch；
        # 返回前必须还原用户 $t0。
        ld.d   $t0, $a3, 88

        # 最后才恢复用户 a3，之后不再需要内核直映 trapframe 基址。
        ld.d   $a3, $a3, 48

        # return to user mode and user pc.
        # usertrapret() set up prmd and era.
        ertn
#endif
