#ifdef LOONGARCH
.section merrentry
.globl machine_trap
.globl handle_merr
.align 0x4
handle_merr:
    bl wrap_machine_trap
    ertn
#endif