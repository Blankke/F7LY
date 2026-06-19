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
        # mapped into user space, at TRAPFRAME.
        #

	# swap a0 and SAVE
        # so that a0 is TRAPFRAME
        csrwr  $a0, LOONGARCH_CSR_SAVE0

        # save the user registers in TRAPFRAME
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

        # 保存用户浮点现场。LoongArch ABI 使用 FPR 传递/返回浮点参数；
        # trap 不保存它会破坏 libm、动态链接器和线程切换中的浮点中间值。
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

DBG_BREAK_READ_ESTAT:
	    # csrrd  $a0, 0x5		# read estat 

            # 正常用户态 trap 一旦成功落到 uservec，就把返回窗口残留的 scratch CSR 清掉。
            # 后面如果还在 kernelvec 上炸，并且 SAVE1/SAVE2 仍然不是 0，
            # 就说明这次根本没经过 uservec。
            li.d   $t1, 0
            li.d   $t2, 0
            csrwr  $t1, LOONGARCH_CSR_SAVE1
            csrwr  $t2, LOONGARCH_CSR_SAVE2

	    # jump to usertrap(), which does not return
	    jirl   $zero ,$t0, 0

.globl userret
userret:
        # userret(TRAPFRAME, pagetable)
        # switch from kernel to user.
        # usertrapret() calls here.
        # a0: TRAPFRAME, in user page table.
        # a1: user page table, for pgdl.

        # switch to the user page table.
        csrwr  $a1, LOONGARCH_CSR_PGDL
        invtlb 0x0,$zero,$zero

        # put the saved user a0 in SAVE0, so we
        # can swap it with our a0 (TRAPFRAME) in the last step.
        ld.d   $t0, $a0, 24
        csrwr  $t0, LOONGARCH_CSR_SAVE0

        # restore all but a0 from TRAPFRAME
        ld.d   $ra, $a0, 0
        ld.d   $tp, $a0, 8
        ld.d   $sp, $a0, 16
        ld.d   $a1, $a0, 32
        ld.d   $a2, $a0, 40
        ld.d   $a3, $a0, 48
        ld.d   $a4, $a0, 56
        ld.d   $a5, $a0, 64
        ld.d   $a6, $a0, 72
        ld.d   $a7, $a0, 80
        ld.d   $t0, $a0, 88
        ld.d   $t1, $a0, 96
        ld.d   $t2, $a0, 104
        ld.d   $t3, $a0, 112
        ld.d   $t4, $a0, 120
        ld.d   $t5, $a0, 128
        ld.d   $t6, $a0, 136
        ld.d   $t7, $a0, 144
        ld.d   $t8, $a0, 152
        ld.d   $r21, $a0,160
        ld.d   $fp, $a0, 168
        ld.d   $s0, $a0, 176
        ld.d   $s1, $a0, 184
        ld.d   $s2, $a0, 192
        ld.d   $s3, $a0, 200
        ld.d   $s4, $a0, 208
        ld.d   $s5, $a0, 216
        ld.d   $s6, $a0, 224
        ld.d   $s7, $a0, 232
        ld.d   $s8, $a0, 240

        # 恢复用户浮点现场，必须在 ertn 前完成。
        fld.d  $f0,  $a0, 288
        fld.d  $f1,  $a0, 296
        fld.d  $f2,  $a0, 304
        fld.d  $f3,  $a0, 312
        fld.d  $f4,  $a0, 320
        fld.d  $f5,  $a0, 328
        fld.d  $f6,  $a0, 336
        fld.d  $f7,  $a0, 344
        fld.d  $f8,  $a0, 352
        fld.d  $f9,  $a0, 360
        fld.d  $f10, $a0, 368
        fld.d  $f11, $a0, 376
        fld.d  $f12, $a0, 384
        fld.d  $f13, $a0, 392
        fld.d  $f14, $a0, 400
        fld.d  $f15, $a0, 408
        fld.d  $f16, $a0, 416
        fld.d  $f17, $a0, 424
        fld.d  $f18, $a0, 432
        fld.d  $f19, $a0, 440
        fld.d  $f20, $a0, 448
        fld.d  $f21, $a0, 456
        fld.d  $f22, $a0, 464
        fld.d  $f23, $a0, 472
        fld.d  $f24, $a0, 480
        fld.d  $f25, $a0, 488
        fld.d  $f26, $a0, 496
        fld.d  $f27, $a0, 504
        fld.d  $f28, $a0, 512
        fld.d  $f29, $a0, 520
        fld.d  $f30, $a0, 528
        fld.d  $f31, $a0, 536
        ld.d   $t0, $a0, 544
        movgr2fcsr $fcsr0, $t0
        ld.d   $t0, $a0, 552
        movgr2cf $fcc0, $t0
        ld.d   $t0, $a0, 560
        movgr2cf $fcc1, $t0
        ld.d   $t0, $a0, 568
        movgr2cf $fcc2, $t0
        ld.d   $t0, $a0, 576
        movgr2cf $fcc3, $t0
        ld.d   $t0, $a0, 584
        movgr2cf $fcc4, $t0
        ld.d   $t0, $a0, 592
        movgr2cf $fcc5, $t0
        ld.d   $t0, $a0, 600
        movgr2cf $fcc6, $t0
        ld.d   $t0, $a0, 608
        movgr2cf $fcc7, $t0
        # 上面的 FCSR/FCC 恢复使用了 $t0 作为 scratch；返回前必须还原用户 $t0。
        ld.d   $t0, $a0, 88

	# restore user a0, and save TRAPFRAME in SAVE0
        csrwr  $a0, LOONGARCH_CSR_SAVE0

        # return to user mode and user pc.
        # usertrapret() set up prmd and era.
        ertn
#endif
