    .globl _main
    .align 4

#include "stalker_cache.h"
#include "stalker_table.h"
#include "svc_stalker_ctl.h"

; This is the system call we replaced the first enosys sysent entry
; with. It manages the list of PIDs we're intercepting syscalls/Mach traps for.
;
; Actual return value of this function gets set to errno later.
; retval, the second parameter, is the return value of this function.
_main:
    sub sp, sp, STACK
    stp x28, x27, [sp, STACK-0x60]
    stp x26, x25, [sp, STACK-0x50]
    stp x24, x23, [sp, STACK-0x40]
    stp x22, x21, [sp, STACK-0x30]
    stp x20, x19, [sp, STACK-0x20]
    stp x29, x30, [sp, STACK-0x10]
    add x29, sp, STACK-0x10

    mov x19, x0
    mov x20, x1
    mov x21, x2

    adr x22, STALKER_CACHE_PTR_PTR
    ; XXX from now on, X28 == stalker cache pointer, do not modify X28
    ldr x28, [x22]

    ldr w22, [x20, FLAVOR_ARG]
    cmp w22, PID_MANAGE
    ; first, let's see if the user wants to check if this syscall was
    ; patched correctly
    b.eq check_if_patched
    cmp w22, CALL_LIST_MANAGE
    b.eq call_manage
    ; if you're interested in checking out the stalker table in userland,
    ; uncomment this stuff and out_givetablekaddr
    ;cmp w22, 2
    ;b.eq out_givetablekaddr
    b out_einval

check_if_patched:
    ldr w22, [x20, PID_ARG]
    cmp w22, -1
    b.eq out_patched
    ; user doesn't want to see if this syscall was patched correctly
    ; if less than -1, pid doesn't make sense
    b.lt out_einval
    ; for this flavor, arg2 controls whether we're intercepting or not
    ; intercepting system calls for this pid
    ldr w23, [x20, ARG2]
    cbnz w23, add_pid
    b delete_pid

add_pid:
    ; figure out if the user is already intercepting system calls for this pid
    ldr x0, [x28, STALKER_TABLE_PTR]
    mov w1, w22
    ldr x22, [x28, STALKER_CTL_FROM_TABLE]
    blr x22
    ; if already added, do nothing
    cbnz x0, success
    ; otherwise, create a new stalker_ctl entry in the stalker table
    ldr x0, [x28, STALKER_TABLE_PTR]
    ldr x22, [x28, GET_NEXT_FREE_STALKER_CTL]
    blr x22
    ; table at capacity?
    cbz x0, out_einval
    ; at this point, we have a free stalker_ctl entry
    ; it's no longer free
    str wzr, [x0, STALKER_CTL_FREE_OFF]
    ; it belongs to the pid argument
    ldr w22, [x20, PID_ARG]
    str w22, [x0, STALKER_CTL_PID_OFF]

    ; call_list is freed/NULL'ed out upon deletion, no need to do anything
    ; with it until the user adds a system call to intercept

    ; increment stalker table size
    ldr x22, [x28, STALKER_TABLE_PTR]
    ldr w23, [x22, STALKER_TABLE_NUM_PIDS_OFF]
    add w23, w23, 1
    str w23, [x22, STALKER_TABLE_NUM_PIDS_OFF]

    b success

delete_pid:
    ; get stalker_ctl pointer for this pid
    ldr x0, [x28, STALKER_TABLE_PTR]
    mov w1, w22
    ldr x22, [x28, STALKER_CTL_FROM_TABLE]
    blr x22
    ; can't delete something that doesn't exist
    cbz x0, out_einval
    ; at this point we have the stalker_ctl entry that belongs to pid
    ; it's now free
    mov w22, 1
    str w22, [x0, STALKER_CTL_FREE_OFF]
    ; it belongs to no pids
    str wzr, [x0, STALKER_CTL_PID_OFF]

    ; decrement stalker table size
    ldr x22, [x28, STALKER_TABLE_PTR]
    ldr w23, [x22, STALKER_TABLE_NUM_PIDS_OFF]
    sub w23, w23, 1
    str w23, [x22, STALKER_TABLE_NUM_PIDS_OFF]

    ; free call_list if it isn't NULL
    ldr x22, [x0, STALKER_CTL_CALL_LIST_OFF]
    cbz x22, success

    mov x23, x0
    mov x0, x22
    ldr x22, [x28, KFREE_ADDR]
    blr x22
    str xzr, [x23, STALKER_CTL_CALL_LIST_OFF]

    b success

call_manage:
    ; get stalker_ctl pointer for this pid
    ldr x0, [x28, STALKER_TABLE_PTR]
    ldr w1, [x20, PID_ARG]
    ldr x22, [x28, STALKER_CTL_FROM_TABLE]
    blr x22
    ; pid hasn't been added to stalker list?
    cbz x0, out_einval
    ; at this point we have the stalker_ctl entry that belongs to pid
    str x0, [sp, CUR_STALKER_CTL]
    ldr w22, [x20, ARG3]
    cbz w22, delete_call

    ; if non-NULL, the call list for this pid already exists
    ldr x22, [x0, STALKER_CTL_CALL_LIST_OFF]
    cbnz x22, add_call

    ; this stalker_ctl's call list is NULL, kalloc a new one
    mov x0, CALL_LIST_MAX
    add x0, xzr, x0, lsl 2                     ; CALL_LIST_MAX*sizeof(int32_t)
    str x0, [sp, CALL_LIST_KALLOC_SZ]
    ; kalloc_canblock expects a pointer for size
    add x0, sp, CALL_LIST_KALLOC_SZ
    ; don't want to block
    mov w1, wzr
    ; no allocation site
    mov x2, xzr
    ldr x22, [x28, KALLOC_CANBLOCK]
    blr x22
    cbz x0, out_enomem

    ldr x22, [sp, CUR_STALKER_CTL]
    str x0, [x22, STALKER_CTL_CALL_LIST_OFF]

    mov x23, CALL_LIST_FREE_SLOT
    mov x24, x0
    mov w25, CALL_LIST_MAX
    add x25, x24, w25, lsl 2

    ; this new call list has all its elems free
call_list_init_loop:
    str w23, [x24], 0x4
    subs x26, x25, x24
    cbnz x26, call_list_init_loop

add_call:
    ; TODO check if this system call is already present and do nothing if it is
    ldr x22, [sp, CUR_STALKER_CTL]
    ldr x0, [x22, STALKER_CTL_CALL_LIST_OFF]
    ldr x22, [x28, GET_CALL_LIST_FREE_SLOT]
    blr x22
    ; no free slots in call list?
    cbz x0, out_einval
    ; X0 = pointer to free slot in call list
    ldr w22, [x20, ARG2]
    ; X22 = system call user wants to intercept
    str w22, [x0]

    b success

delete_call:
    ldr x22, [sp, CUR_STALKER_CTL]
    ldr x0, [x22, STALKER_CTL_CALL_LIST_OFF]
    ldr w1, [x20, ARG2]
    ldr x22, [x28, GET_CALL_LIST_SLOT]
    blr x22
    ; this system call was never added?
    cbz x0, out_einval
    ; X0 = pointer to slot in call list which this system call occupies
    ; this slot is now free
    mov x22, CALL_LIST_FREE_SLOT
    str x22, [x0]

    b success

out_einval:
    mov w0, -1
    str w0, [x21]
    mov w0, 22
    b done

out_enomem:
    mov w0, -1
    str w0, [x21]
    mov w0, 12
    b done

out_patched:
    mov w0, 999
    str w0, [x21]
    mov w0, 0
    b done

; out_givetablekaddr:
;     ldr x0, [x28, STALKER_TABLE]
;     str x0, [x21]
;     mov w0, 0
;     b done

success:
    mov w0, 0
    str w0, [x21]

done:
    ldp x29, x30, [sp, STACK-0x10]
    ldp x20, x19, [sp, STACK-0x20]
    ldp x22, x21, [sp, STACK-0x30]
    ldp x24, x23, [sp, STACK-0x40]
    ldp x26, x25, [sp, STACK-0x50]
    ldp x28, x27, [sp, STACK-0x60]
    add sp, sp, STACK
    ret
