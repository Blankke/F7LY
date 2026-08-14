	#
        # code to switch between user and kernel space.
        #
        # this code is mapped at the same virtual address
        # (TRAMPOLINE) in user and kernel space so that
        # it continues to work when it switches page tables.
	#
	# kernel.ld causes this to be aligned
        # to a page boundary.
        #
	.section trampsec
.globl trampoline
trampoline:
.align 4
.globl uservec
uservec:
	#
        # trap.c sets stvec to point here, so
        # traps from user space start here,
        # in supervisor mode, but with a
        # user page table.
        #
        # sscratch points to where the process's p->trapframe is
        # mapped into user space. usertrapret passes the per-thread VA via sscratch.
        #

	# swap a0 and sscratch
        # so that a0 is the current thread's trapframe VA
        csrrw a0, sscratch, a0

        # save the user registers in the current thread's trapframe
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

        # 只有 FS=Dirty 才表示用户自上次恢复后改写了 F/D 现场。Clean 状态
        # 与 trapframe 中的镜像相同，Off/Initial 也无需保存；这让整数任务和
        # 只做 syscall 的热路径跳过 32 次浮点存储。
        csrr t0, sstatus
        srli t1, t0, 13
        andi t1, t1, 3
        li t2, 3
        bne t1, t2, .Lsave_user_fp_done
        fsd f0, 288(a0)
        fsd f1, 296(a0)
        fsd f2, 304(a0)
        fsd f3, 312(a0)
        fsd f4, 320(a0)
        fsd f5, 328(a0)
        fsd f6, 336(a0)
        fsd f7, 344(a0)
        fsd f8, 352(a0)
        fsd f9, 360(a0)
        fsd f10, 368(a0)
        fsd f11, 376(a0)
        fsd f12, 384(a0)
        fsd f13, 392(a0)
        fsd f14, 400(a0)
        fsd f15, 408(a0)
        fsd f16, 416(a0)
        fsd f17, 424(a0)
        fsd f18, 432(a0)
        fsd f19, 440(a0)
        fsd f20, 448(a0)
        fsd f21, 456(a0)
        fsd f22, 464(a0)
        fsd f23, 472(a0)
        fsd f24, 480(a0)
        fsd f25, 488(a0)
        fsd f26, 496(a0)
        fsd f27, 504(a0)
        fsd f28, 512(a0)
        fsd f29, 520(a0)
        fsd f30, 528(a0)
        fsd f31, 536(a0)
        frcsr t0
        sd t0, 544(a0)
.Lsave_user_fp_done:
        # 内核按 soft-float 构建且通过 no-FP 链接检查。进入 C++ 前关闭 FS，
        # 防止任何意外内核浮点指令静默污染尚未交还给用户的现场。
        csrr t0, sstatus
        li t1, -24577               # ~(3 << 13)
        and t0, t0, t1
        csrw sstatus, t0

	# save the user a0 in p->trapframe->a0
        csrr t0, sscratch
        sd t0, 112(a0)

        # restore kernel stack pointer from p->trapframe->kernel_sp
        ld sp, 8(a0)

        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, 32(a0)

        # load the address of usertrap(), p->trapframe->kernel_trap
        ld t0, 16(a0)

        # restore kernel page table from p->trapframe->kernel_satp
        ld t1, 0(a0)
        # 用户页表使用隔离的非零 ASID，内核页表固定使用 ASID 0。
        # 地址空间复用前由 ASID 回收池执行全核 sfence，无需在每次 trap
        # 往返时清空本 hart 全部 TLB。
        csrw satp, t1

        # a0 is no longer valid, since the kernel page
        # table does not specially map p->tf.

        # jump to usertrap(), which does not return
        jr t0

.globl userret
userret:
        # userret(trapframe_va, pagetable, used_fpu)
        # switch from kernel to user.
        # usertrapret() calls here.
        # a0: current thread's trapframe VA, in user page table.
        # a1: user page table, for satp.
        # a2: whether this thread has enabled user FPU state.

        # switch to the user page table and its isolated non-zero ASID.
        csrw satp, a1

        # put the saved user a0 in sscratch, so we
        # can swap it with our a0 (trapframe VA) in the last step.
        ld t0, 112(a0)
        csrw sscratch, t0

        # 未使用 FPU 的线程保持 FS=Off；使用过的线程临时启用 FS，恢复保存
        # 现场后显式标为 Clean。后续只有用户真正执行浮点指令才会变为 Dirty。
        beqz a2, .Ldisable_user_fp
        csrr t0, sstatus
        li t1, -24577               # ~(3 << 13)
        and t0, t0, t1
        li t1, 8192                 # FS=Initial
        or t0, t0, t1
        csrw sstatus, t0

        fld f0, 288(a0)
        fld f1, 296(a0)
        fld f2, 304(a0)
        fld f3, 312(a0)
        fld f4, 320(a0)
        fld f5, 328(a0)
        fld f6, 336(a0)
        fld f7, 344(a0)
        fld f8, 352(a0)
        fld f9, 360(a0)
        fld f10, 368(a0)
        fld f11, 376(a0)
        fld f12, 384(a0)
        fld f13, 392(a0)
        fld f14, 400(a0)
        fld f15, 408(a0)
        fld f16, 416(a0)
        fld f17, 424(a0)
        fld f18, 432(a0)
        fld f19, 440(a0)
        fld f20, 448(a0)
        fld f21, 456(a0)
        fld f22, 464(a0)
        fld f23, 472(a0)
        fld f24, 480(a0)
        fld f25, 488(a0)
        fld f26, 496(a0)
        fld f27, 504(a0)
        fld f28, 512(a0)
        fld f29, 520(a0)
        fld f30, 528(a0)
        fld f31, 536(a0)
        ld t0, 544(a0)
        fscsr t0
        csrr t0, sstatus
        li t1, -24577               # ~(3 << 13)
        and t0, t0, t1
        li t1, 16384                # FS=Clean
        or t0, t0, t1
        csrw sstatus, t0
        j .Lrestore_user_fp_done

.Ldisable_user_fp:
        csrr t0, sstatus
        li t1, -24577               # ~(3 << 13), FS=Off
        and t0, t0, t1
        csrw sstatus, t0
.Lrestore_user_fp_done:

        # restore all but a0 from the current thread's trapframe
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# restore user a0, and save the trapframe VA in sscratch
        csrrw a0, sscratch, a0

        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        sret
