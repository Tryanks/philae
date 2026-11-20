// Assembly half of the JNI layer for Android-ABI native code; see android_abi_jni.h.

// --- Forwarding entries ------------------------------------------------------------------
//
// One two-instruction entry per slot of the JNI function table: it notes its slot and
// joins common code, which swaps the Android-facing JNIEnv (x0: function table, then a
// pointer to the real JNIEnv) for the real one and jumps to the real function in that
// slot. Used for every JNI function whose calling convention is the same on both
// platforms. x16 and x17 are free at function entry.

    .text
    .p2align 2
    .globl _a2i_android_jni_forwarders
_a2i_android_jni_forwarders:
    .set slot, 0
    .rept 240
    mov x17, #slot
    b Lforward
    .set slot, slot + 1
    .endr
Lforward:
    ldr x0, [x0, #8]
    ldr x16, [x0]
    ldr x16, [x16, x17, lsl #3]
    br x16

// --- Variadic entries --------------------------------------------------------------------
//
// `Call<Type>Method(env, obj, mid, ...)` and friends. Android passes the variable
// arguments in the remaining argument registers and then on the stack. The entry saves
// the argument registers, describes the variable arguments as an Android va_list, and
// calls the C++ function for the corresponding `...V` JNI function, which takes that
// va_list by reference (as Android passes one) after the named arguments.

// Frame, from sp: x29/x30, x0-x7, q0-q7, the va_list (32 bytes).
.macro JNI_VARIADIC name, named
    .globl _a2i_android_jni_\name
    .p2align 2
_a2i_android_jni_\name:
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
    add x9, sp, #240                // __stack
    str x9, [sp, #208]
    add x9, sp, #80                 // __gr_top
    str x9, [sp, #216]
    add x9, sp, #208                // __vr_top
    str x9, [sp, #224]
    mov w9, #-((8 - \named) * 8)    // __gr_offs
    str w9, [sp, #232]
    mov w9, #-128                   // __vr_offs
    str w9, [sp, #236]
    add x\named, sp, #208
    bl _a2i_android_jni_\name\()V
    ldp x29, x30, [sp]
    add sp, sp, #240
    ret
.endm

.macro JNI_VARIADIC_CALLS type
    JNI_VARIADIC Call\type\()Method, 3
    JNI_VARIADIC CallNonvirtual\type\()Method, 4
    JNI_VARIADIC CallStatic\type\()Method, 3
.endm

    JNI_VARIADIC NewObject, 3
    JNI_VARIADIC_CALLS Object
    JNI_VARIADIC_CALLS Boolean
    JNI_VARIADIC_CALLS Byte
    JNI_VARIADIC_CALLS Char
    JNI_VARIADIC_CALLS Short
    JNI_VARIADIC_CALLS Int
    JNI_VARIADIC_CALLS Long
    JNI_VARIADIC_CALLS Float
    JNI_VARIADIC_CALLS Double
    JNI_VARIADIC_CALLS Void
