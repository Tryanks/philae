// Entry points for the C library's variadic functions, as Android code calls them.
//
// The two platforms pass variable arguments differently: Android (the standard arm64
// convention) continues in the argument registers after the named arguments and then
// on the stack, while iOS puts all of them on the stack. A C function built for iOS
// therefore cannot read arguments passed the Android way. Each stub here saves the
// argument registers, describes where the variable arguments are in the form of
// Android's own va_list, and calls an ordinary C function taking a pointer to that as
// the argument after the named ones (bionic_v_<name>, see stdio.c). That same C function
// serves the v-variant (vprintf for printf), since Android passes a va_list by reference.

// Layout of the frame, from sp: x29/x30, x0-x7, q0-q7, the va_list (32 bytes).
.macro VARIADIC name, named
    .globl _bionic_\name
    .p2align 2
_bionic_\name:
    sub sp, sp, #240
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    add x9, sp, #240                // __stack: the caller's stack arguments
    str x9, [sp, #208]
    add x9, sp, #80                 // __gr_top: the end of the saved x registers
    str x9, [sp, #216]
    add x9, sp, #208                // __vr_top: the end of the saved q registers
    str x9, [sp, #224]
    mov w9, #-((8 - \named) * 8)    // __gr_offs: the first variable argument's register
    str w9, [sp, #232]
    mov w9, #-128                   // __vr_offs: no named floating-point arguments
    str w9, [sp, #236]
    add x\named, sp, #208
    bl _bionic_v_\name
    ldp x29, x30, [sp]
    add sp, sp, #240
    ret
.endm

.text
VARIADIC printf, 1
VARIADIC fprintf, 2
VARIADIC dprintf, 2
VARIADIC sprintf, 2
VARIADIC snprintf, 3
VARIADIC asprintf, 2
VARIADIC __sprintf_chk, 4
VARIADIC __snprintf_chk, 5
VARIADIC swprintf, 3
VARIADIC scanf, 1
VARIADIC sscanf, 2
VARIADIC fscanf, 2
VARIADIC syslog, 2
VARIADIC __android_log_print, 3
VARIADIC __android_log_buf_print, 4
VARIADIC __android_log_assert, 3
VARIADIC open, 2
VARIADIC open64, 2
VARIADIC openat, 3
VARIADIC fcntl, 2
VARIADIC ioctl, 2
VARIADIC syscall, 1
VARIADIC execl, 2
VARIADIC execlp, 2
