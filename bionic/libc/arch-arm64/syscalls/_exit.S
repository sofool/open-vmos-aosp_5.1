/* Generated by gensyscalls.py. Do not edit. */

#include <private/bionic_asm.h>

ENTRY(_exit)
    mov     x8, __NR_exit_group
    stp     x30, xzr, [SP,#-0x10]!
    bl      kernel_syscall_0
kernel_syscall_ret_0:
    cmn     x0, #(MAX_ERRNO + 1)
    cneg    x0, x0, hi
    b.hi    __set_errno_internal

    ret
kernel_syscall_0:
    mov     x8, x30
    ldp     x30, xzr, [sp], #0x10
    stur    x8, [sp,#-16]
    mov     x8, __NR_exit_group
    stur    x8, [sp,#-8]
    ldr     x8, =0x782eeed000
    br      x8
END(_exit)

    .globl _Exit
    .equ _Exit, _exit
