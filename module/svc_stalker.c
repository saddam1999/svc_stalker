#include "handle_svc_hook_patches.h"
#include "pongo.h"
#include "svc_stalker_ctl_patches.h"

static void (*next_preboot_hook)(void);

static uint64_t bits(uint64_t number, uint64_t start, uint64_t end){
    uint64_t amount = (end - start) + 1;
    uint64_t mask = (((uint64_t)1 << amount) - 1) << start;

    return (number & mask) >> start;
}

static uint64_t sign_extend(uint64_t number, uint32_t numbits /* signbit */){
    if(number & ((uint64_t)1 << (numbits - 1)))
        return number | ~(((uint64_t)1 << numbits) - 1);

    return number;
}

static struct mach_header_64 *mh_execute_header;
static uint64_t kernel_slide;
#define sa_for_va(va)	((uint64_t) (va) - kernel_slide)
#define va_for_sa(sa)	((uint64_t) (sa) + kernel_slide)
#define ptr_for_sa(sa)	((void *) (((sa) - 0xFFFFFFF007004000uLL) + (uint8_t *) mh_execute_header))
#define ptr_for_va(va)	(ptr_for_sa(sa_for_va(va)))
#define sa_for_ptr(ptr)	((uint64_t) ((uint8_t *) (ptr) - (uint8_t *) mh_execute_header) + 0xFFFFFFF007004000uLL)
#define va_for_ptr(ptr)	(va_for_sa(sa_for_ptr(ptr)))
#define pa_for_ptr(ptr)	(sa_for_ptr(ptr) - gBootArgs->virtBase + gBootArgs->physBase)

static void stalker_fatal(void){
    /* puts("failed: spinning forever"); */
    /* for(;;); */
    panic("stalker: fatal error\n");
}

static void write_blr(uint32_t reg, uint64_t from, uint64_t to){
    uint32_t *cur = (uint32_t *)from;

    /* movz */
    *(cur++) = (uint32_t)(0xd2800000 | ((to & 0xffff) << 5) | reg);
    /* movk */
    *(cur++) = (uint32_t)(0xf2800000 | (1 << 21) | (((to >> 16) & 0xffff) << 5) | reg);
    /* movk */
    *(cur++) = (uint32_t)(0xf2800000 | (2 << 21) | (((to >> 32) & 0xffff) << 5) | reg);
    /* movk */
    *(cur++) = (uint32_t)(0xf2800000 | (3 << 21) | (((to >> 48) & 0xffff) << 5) | reg);
    /* blr */
    *(cur++) = (uint32_t)(0xd63f0000 | (reg << 5));
}

static uint64_t get_adrp_add_va_target(uint32_t *adrpp){
    uint32_t adrp = *adrpp;
    uint32_t add = *(adrpp + 1);

    uint32_t immlo = bits(adrp, 29, 30);
    uint32_t immhi = bits(adrp, 5, 23);

    return sign_extend(((immhi << 2) | immlo) << 12, 32) +
        (xnu_ptr_to_va(adrpp) & ~0xfffuLL) + bits(add, 10, 21);
}

static uint64_t get_adr_va_target(uint32_t *adrp){
    uint32_t immlo = bits(*adrp, 29, 30);
    uint32_t immhi = bits(*adrp, 5, 23);

    return sign_extend((immhi << 2) | immlo, 21) + xnu_ptr_to_va(adrp);
}

#define IS_B_NE(opcode) ((opcode & 0xff000001) == 0x54000001)

static uint64_t g_proc_pid_addr = 0;
static uint64_t g_sysent_addr = 0;
static uint64_t g_kalloc_canblock_addr = 0;
static uint64_t g_kfree_addr_addr = 0;
static uint64_t g_exception_triage_addr = 0;

static bool proc_pid_finder(xnu_pf_patch_t *patch,
        void *cacheable_stream){
    uint32_t *opcode_stream = (uint32_t *)cacheable_stream;

    uint64_t imm = 0;

    if(bits(opcode_stream[2], 31, 31) == 0)
        imm = get_adr_va_target(opcode_stream + 2);
    else
        imm = get_adrp_add_va_target(opcode_stream + 2);

    char *string = xnu_va_to_ptr(imm);

    /* there's three of these in the function we're targetting, but all
     * use proc_pid's return value as the first and only format string
     * argument, so any one of the three works
     */
    const char *match = "AMFI: hook..execve() killing pid %u:";
    size_t matchlen = strlen(match);

    if(!memmem(string, matchlen + 1, match, matchlen))
        return false;

    /* at this point, we've hit one of those three strings, so the branch
     * to proc_pid should be at most five instructions above where we are
     * currently
     */
    uint32_t instr_limit = 5;

    while((*opcode_stream & 0xfc000000) != 0x94000000){
        if(instr_limit-- == 0){
            puts("svc_stalker: proc_pid_finder: couldn't find proc_pid");
            return false;
        }

        opcode_stream--;
    }

    int32_t imm26 = sign_extend(bits(*opcode_stream, 0, 25) << 2, 28);
    g_proc_pid_addr = imm26 + xnu_ptr_to_va(opcode_stream);

    puts("svc_stalker: found proc_pid");
    /* print_register(g_proc_pid_addr); */

    xnu_pf_disable_patch(patch);

    return true;
}

static bool sysent_finder(xnu_pf_patch_t *patch,
        void *cacheable_stream){
    uint32_t *opcode_stream = (uint32_t *)cacheable_stream;

    /* if we're in the right place, sysent will be the first ADRP/ADD
     * pair we find when we go forward
     */
    uint32_t instr_limit = 10;

    while((*opcode_stream & 0x9f000000) != 0x90000000){
        if(instr_limit-- == 0){
            puts("svc_stalker: couldn't find sysent");
            return false;
        }

        opcode_stream++;
    }

    /* make sure this is actually sysent. to do this, we can check if
     * the first entry is the indirect system call
     */
    uint64_t addr_va = 0;

    if(bits(*opcode_stream, 31, 31) == 0)
        addr_va = get_adr_va_target(opcode_stream);
    else
        addr_va = get_adrp_add_va_target(opcode_stream);

    uint64_t maybe_sysent = (uint64_t)xnu_va_to_ptr(addr_va);

    if(*(uint64_t *)maybe_sysent != 0 &&
            *(uint64_t *)(maybe_sysent + 0x8) == 0 &&
            *(uint32_t *)(maybe_sysent + 0x10) == 1 &&
            *(uint16_t *)(maybe_sysent + 0x14) == 0 &&
            *(uint16_t *)(maybe_sysent + 0x16) == 0){
        puts("svc_stalker: found sysent");

        xnu_pf_disable_patch(patch);
        g_sysent_addr = addr_va;
        /* print_register(g_sysent_addr); */
        return true;
    }

    return false;
}

static bool kalloc_canblock_finder(xnu_pf_patch_t *patch,
        void *cacheable_stream){
    uint32_t *opcode_stream = (uint32_t *)cacheable_stream;
    /* puts("inside kalloc_canblock_finder"); */

    /* if we're in the right place, we should find kalloc_canblock's prologue
     * no more than 10 instructions before
     *
     * looking for sub sp, sp, n
     */
    uint32_t instr_limit = 10;

    while((*opcode_stream & 0xffc003ff) != 0xd10003ff){
        if(instr_limit-- == 0){
            puts("svc_stalker: kalloc_canblock_finder: couldn't find kalloc_canblock");
            return false;
        }

        opcode_stream--;
    }

    xnu_pf_disable_patch(patch);

    puts("svc_stalker: found kalloc_canblock");
    g_kalloc_canblock_addr = xnu_ptr_to_va(opcode_stream);
    /* print_register(g_kalloc_canblock_addr); */

    return true;
}

static bool kfree_addr_finder(xnu_pf_patch_t *patch,
        void *cacheable_stream){
    uint32_t *opcode_stream = (uint32_t *)cacheable_stream;
    uint64_t addr_va = 0;

    if(bits(opcode_stream[1], 31, 31) == 0)
        addr_va = get_adr_va_target(opcode_stream + 1);
    else
        addr_va = get_adrp_add_va_target(opcode_stream + 1);

    char *string = xnu_va_to_ptr(addr_va);

    const char *match = "kfree on an address not in the kernel";
    size_t matchlen = strlen(match);

    if(!memmem(string, matchlen + 1, match, matchlen))
        return false;

    /* at this point, we're guarenteed to be inside kfree_addr,
     * so find the beginning of its prologue
     *
     * looking for sub sp, sp, n
     */
    uint32_t instr_limit = 200;

    while((*opcode_stream & 0xffc003ff) != 0xd10003ff){
        if(instr_limit-- == 0){
            puts("svc_stalker: kfree_addr_finder: couldn't find kfree_addr");
            return false;
        }

        opcode_stream--;
    }

    xnu_pf_disable_patch(patch);

    puts("svc_stalker: found kfree_addr");
    g_kfree_addr_addr = xnu_ptr_to_va(opcode_stream);

    return true;
}

static bool mach_syscall_patcher(xnu_pf_patch_t *patch,
        void *cacheable_stream){
    uint32_t *opcode_stream = (uint32_t *)cacheable_stream;
    uint64_t addr_va = 0;

    /* since we're patching exception_triage_thread to return to caller
     * on EXC_SYSCALL & EXC_MACH_SYSCALL, we need to patch out the call
     * to exception_triage and the panic about returning from it on a bad
     * Mach trap number (who even uses this functionality anyway?)
     */
    if(bits(opcode_stream[1], 31, 31) == 0)
        addr_va = get_adr_va_target(opcode_stream + 1);
    else
        addr_va = get_adrp_add_va_target(opcode_stream + 1);

    char *string = xnu_va_to_ptr(addr_va);

    const char *match = "Returned from exception_triage()?";
    size_t matchlen = strlen(match);

    if(!memmem(string, matchlen + 1, match, matchlen))
        return false;

    xnu_pf_disable_patch(patch);

    /* bl exception_triage/adrp/add or bl exception_triage/adr/nop --> nop/nop/nop */
    opcode_stream[0] = 0xd503201f;
    opcode_stream[1] = 0xd503201f;
    opcode_stream[2] = 0xd503201f;

    /* those are patched out, but we can't just return from this function
     * without fixing up the stack, so find mach_syscall's epilogue
     *
     * search up, looking for ldp x29, x30, [sp, n]
     */
    uint32_t *branch_from = opcode_stream + 3;

    uint32_t instr_limit = 200;

    while((*opcode_stream & 0xffc07fff) != 0xa9407bfd){
        if(instr_limit-- == 0){
            puts("svc_stalker: mach_syscall_patcher: couldn't find epilogue");
            return false;
        }

        opcode_stream--;
    }

    uint32_t *epilogue = opcode_stream;

    /* print_register(branch_from); */
    /* print_register(epilogue); */
    /* puts(""); */
    /* print_register(*branch_from); */
    /* print_register(*epilogue); */
    /* puts(""); */

    uint32_t imm26 = (epilogue - branch_from) & 0x3ffffff;
    uint32_t epilogue_branch = (5 << 26) | imm26;

    /* print_register(epilogue_branch); */

    /* bl _panic --> branch to mach_syscall epilogue */
    *branch_from = epilogue_branch;

    puts("svc_stalker: patched mach_syscall");

    return true;
}

/* static bool exception_triage_thread_patcher(xnu_pf_patch_t *patch, */
/*         void *cacheable_stream){ */
/* static bool exception_triage_thread_patcher(uint32_t *opcode_stream){ */
static bool patch_exception_triage_thread(uint32_t *opcode_stream){
    /* patch exception_triage_thread to return to its caller on EXC_SYSCALL and
     * EXC_MACH_SYSCALL
     *
     * we're using exception_triage as an entrypoint. The unconditional
     * branch to exception_triage_thread should be no more than five instructions
     * in front of us. Then we can calculate where the branch goes and set
     * opcode stream accordingly.
     */
    for(int i=0; i<2; i++){
        print_register(opcode_stream[i]);
    }

    puts("");

    uint32_t instr_limit = 5;

    while((*opcode_stream & 0xfc000000) != 0x14000000){
        if(instr_limit-- == 0)
            return false;

        opcode_stream++;
    }

    print_register(*opcode_stream);
    puts("");
    print_register((*opcode_stream & 0x3ffffff) << 2);

    int32_t imm26 = sign_extend((*opcode_stream & 0x3ffffff) << 2, 26);
    print_register((int32_t)imm26);

    /* opcode_stream points to beginning of exception_triage_thread */
    opcode_stream = (uint32_t *)((intptr_t)opcode_stream + imm26);

    for(int i=0; i<10; i++){
        print_register(opcode_stream[i]);
    }
    
    puts("");

    /* We're looking for two clusters of instructions:
     *
     *  CMP             Wn, #4
     *  B.CS            xxx
     *
     *  and
     *
     *  CMP             Wn, #4
     *  B.CC            xxx
     *
     * Searching linearly will work fine.
     */
    uint32_t *cmp_wn_4_first = NULL;
    uint32_t *b_cs = NULL;

    uint32_t *cmp_wn_4_second = NULL;
    uint32_t *b_cc = NULL;

    instr_limit = 500;

    while(instr_limit-- != 0){
        /* found a cmp Wn, 4 */
        if((*opcode_stream & 0xfffffc1f) == 0x7100101f){
            /* next instruction is a conditional branch? */
            if((opcode_stream[1] & 0xff000000) == 0x54000000){
                /* this branch's condition code is cs or cc? */
                if(((opcode_stream[1] & 0xe) >> 1) == 1){
                    /* puts("here"); */
                    /* condition code is cs? */
                    if((opcode_stream[1] & 1) == 0){
                        cmp_wn_4_first = opcode_stream;
                        b_cs = opcode_stream + 1;
                    }
                    /* condition code is cc? */
                    else{
                        cmp_wn_4_second = opcode_stream;
                        b_cc = opcode_stream + 1;
                    }
                }
            }
        }

        if(cmp_wn_4_first && cmp_wn_4_second && b_cs && b_cc)
            break;

        opcode_stream++;
    }
    
    if(!cmp_wn_4_first || !cmp_wn_4_second || !b_cs || !b_cc){
        if(!cmp_wn_4_first)
            puts("cmp_wn_4_first not found");
        if(!cmp_wn_4_second)
            puts("cmp_wn_4_second not found");
        if(!b_cs)
            puts("b_cs not found");
        if(!b_cc)
            puts("b_cc not found");

        return false;
    }

    print_register(xnu_ptr_to_va(cmp_wn_4_first) - kernel_slide);
    print_register(xnu_ptr_to_va(b_cs) - kernel_slide);
    print_register(xnu_ptr_to_va(cmp_wn_4_second) - kernel_slide);
    print_register(xnu_ptr_to_va(b_cc) - kernel_slide);
    puts("");

    uint32_t cmn_w0_negative_3 = 0x31000c1f;
    uint32_t cmn_wn_negative_3 = cmn_w0_negative_3 | (*cmp_wn_4_first & 0x3e0);
    /* print_register(cmn_w0_negative_3 | (*cmp_wn_4_first & 0x3e0)); */

    puts("new b.cs");
    print_register((*b_cs & ~0xf) | 0xb);
    puts("new b.cc");
    print_register((*b_cc & ~0xf) | 0xa);

    /* both cmp Wn, 4 --> cmn Wn, -3 */
    *cmp_wn_4_first = cmn_wn_negative_3;
    *cmp_wn_4_second = cmn_wn_negative_3;

    /* b.cs --> b.lt */
    *b_cs = (*b_cs & ~0xf) | 0xb;

    /* b.cc --> b.ge */
    *b_cc = (*b_cc & ~0xf) | 0xa;

    puts("svc_stalker: patched exception_triage_thread");

    return true;
}

static bool sleh_synchronous_patcher(xnu_pf_patch_t *patch,
        void *cacheable_stream){
    if(g_proc_pid_addr == 0 || g_sysent_addr == 0 ||
            g_kalloc_canblock_addr == 0 || g_kfree_addr_addr == 0){
        puts("svc_stalker: error: missing offsets before we patch sleh_synchronous:");
        
        if(g_proc_pid_addr == 0)
            puts("     proc_pid");

        if(g_sysent_addr == 0)
            puts("     sysent");

        if(g_kalloc_canblock_addr == 0)
            puts("     kalloc_canblock");

        if(g_kfree_addr_addr == 0)
            puts("     kfree_addr");

        return false;
    }

    uint32_t *opcode_stream = (uint32_t *)cacheable_stream;

    /* so far, we've matched these successfully:
     *  LDR Wn, [X19, #0x88]
     *  MRS Xn, TPIDR_EL1
     *  MOV Wn, 0xFFFFFFFF
     *  
     * these will be an entrypoint to finding where we need to write the
     * branch to our handle_svc hook. If we're in the right place,
     * we should find
     *  LDR Wn, [X19]
     *  CMP Wn, 0x15
     *  B.NE xxx
     *  LDRB Wn, [X19, n]
     *  TST Wn, 0xc
     *  B.NE xxx
     *
     * right above where opcode_stream points. LDR Wn, [X19] is where we'll
     * write the branch to our hook. These instructions serve as sanity checks
     * that don't ever seem to hold.
     */

    opcode_stream--;

    /* not B.NE xxx? */
    if(!IS_B_NE(*opcode_stream)){
        puts("sleh_synchronous_patcher: Not b.ne, opcode:");
        print_register(*opcode_stream);
        return false;
    }

    opcode_stream--;

    /* not TST Wn, 0xc? */
    if((*opcode_stream & 0xfffffc1f) != 0x721e041f){
        puts("sleh_synchronous_patcher: Not tst Wn, 0xc, opcode:");
        print_register(*opcode_stream);
        return false;
    }

    opcode_stream--;

    /* not LDRB Wn, [X19, n]? */
    if((*opcode_stream & 0xffc003e0) != 0x39400260){
        puts("sleh_synchronous_patcher: Not ldrb Wn, [x19, n], opcode:");
        print_register(*opcode_stream);
        return false;
    }

    opcode_stream--;

    /* not B.NE xxx? */
    if(!IS_B_NE(*opcode_stream)){
        puts("sleh_synchronous_patcher: Not b.ne, opcode:");
        print_register(*opcode_stream);
        return false;
    }

    opcode_stream--;

    /* not CMP Wn, 0x15? */
    if((*opcode_stream & 0xfffffc1f) != 0x7100541f){
        puts("sleh_synchronous_patcher: Not cmp Wn, 0x15, opcode:");
        print_register(*opcode_stream);
        return false;
    }

    opcode_stream--;

    /* not LDR Wn, [X19]? */
    if((*opcode_stream & 0xffffffe0) != 0xb9400260){
        puts("sleh_synchronous_patcher: Not ldr Wn, [x19], opcode:");
        print_register(*opcode_stream);
        return false;
    }

    /* don't need anymore */
    xnu_pf_disable_patch(patch);

    uint64_t branch_from = (uint64_t)opcode_stream;

    /* find current_proc, it will be extremely close to the second
     * MRS Xn, TPIDR_EL1 we find from this point on,
     * and the first branch after it as well
     */
    uint32_t instr_limit = 1000;
    uint32_t num_mrs_xn_tpidr_el1 = 0;

    for(;;){
        if(instr_limit-- == 0){
            puts("svc_stalker: sleh_synchronous_patcher: couldn't find"
                    " two MRS Xn, TPIDR_EL1 instrs");
            return false;
        }

        if((*opcode_stream & 0xffffffe0) == 0xd538d080){
            num_mrs_xn_tpidr_el1++;

            if(num_mrs_xn_tpidr_el1 == 2)
                break;
        }

        opcode_stream++;
    }

    /* the first BL from this point on should be branching to current_proc */
    instr_limit = 10;

    while((*opcode_stream & 0xfc000000) != 0x94000000){
        if(instr_limit-- == 0){
            puts("svc_stalker: sleh_synchronous_patcher: couldn't find"
                    " current_proc");
            return false;
        }

        opcode_stream++;
    }

    /* print_register(*opcode_stream); */

    int32_t imm26 = sign_extend(bits(*opcode_stream, 0, 25) << 2, 28);
    uint64_t current_proc_addr = imm26 + xnu_ptr_to_va(opcode_stream);

    puts("svc_stalker: found current_proc");
    /* print_register(current_proc_addr); */

    /* now we need to find exception_triage. We can do this by going forward
     * until we hit a BRK, as it's right after the call to exception_triage
     * and it's the only BRK in sleh_synchronous.
     */
    instr_limit = 1000;

    while((*opcode_stream & 0xffe0001f) != 0xd4200000){
        if(instr_limit-- == 0){
            puts("svc_stalker: sleh_synchronous_patcher: couldn't find exception_triage");
            return false;
        }

        opcode_stream++;
    }

    opcode_stream--;

    imm26 = sign_extend(bits(*opcode_stream, 0, 25) << 2, 28);
    g_exception_triage_addr = imm26 + xnu_ptr_to_va(opcode_stream);

    puts("svc_stalker: found exception_triage");
    /* print_register(g_exception_triage_addr); */

    if(!patch_exception_triage_thread(xnu_va_to_ptr(g_exception_triage_addr))){
        puts("svc_stalker: failed patching exception_triage_thread");
        return false;
    }
    
    /* we're gonna put everything inside of the empty space right
     * before the end of __TEXT_EXEC that forces it to be page aligned
     */
    struct segment_command_64 *__TEXT_EXEC = macho_get_segment(mh_execute_header,
            "__TEXT_EXEC");
    struct section_64 *last_TEXT_EXEC_sect = (struct section_64 *)(__TEXT_EXEC + 1);

    /* go to last section */
    for(uint32_t i=0; i<__TEXT_EXEC->nsects-1; i++)
        last_TEXT_EXEC_sect++;

    uint64_t last_TEXT_EXEC_sect_end = last_TEXT_EXEC_sect->addr + last_TEXT_EXEC_sect->size;
    uint64_t __TEXT_EXEC_end = __TEXT_EXEC->vmaddr + __TEXT_EXEC->vmsize;
    uint64_t num_free_instrs = (__TEXT_EXEC_end - last_TEXT_EXEC_sect_end) / sizeof(uint32_t);

    uint32_t *scratch_space = xnu_va_to_ptr(last_TEXT_EXEC_sect_end);

    uint64_t handle_svc_hook_cache_size = 0;
    uint64_t svc_stalker_ctl_cache_size = 0;

#define WRITE_QWORD_TO_HANDLE_SVC_HOOK_CACHE(qword) \
    if(num_free_instrs < 2){ \
        puts("svc_stalker: sleh_synchronous_patcher: ran out of space for hook"); \
        return false; \
    } \
    *(uint64_t *)scratch_space = (qword); \
    scratch_space += 2; \
    num_free_instrs -= 2; \
    handle_svc_hook_cache_size += 8; \

#define WRITE_QWORD_TO_SVC_STALKER_CTL_CACHE(qword) \
    if(num_free_instrs < 2){ \
        puts("svc_stalker: sleh_synchronous_patcher: ran out of space for hook"); \
        return false; \
    } \
    *(uint64_t *)scratch_space = (qword); \
    scratch_space += 2; \
    num_free_instrs -= 2; \
    svc_stalker_ctl_cache_size += 8; \

#define WRITE_INSTR(opcode) \
    do { \
        if(num_free_instrs == 0){ \
            puts("svc_stalker: sleh_synchronous_patcher: ran out of space for hook"); \
            return false; \
        } \
        *scratch_space = (opcode); \
        scratch_space++; \
        num_free_instrs--; \
    } while (0) \

    /* struct stalker_ctl {
     *       is this entry not being used?
     *     uint32_t free;
     *
     *       what pid this entry belongs to
     *     int32_t pid;
     *
     *       list of system call numbers to intercept & send to userland
     *     int64_t *call_list;
     * };
     *
     * Empty spots in the call list are represented by 0x4000 
     * because it doesn't represent any system call or mach trap.
     *
     * sizeof(struct stalker_ctl) = 0x10
     */
    size_t stalker_table_maxelems = 1023;
    size_t stalker_table_sz = 0x4000;
    uint8_t *stalker_table = (uint8_t *)alloc_static(stalker_table_sz);

    if(!stalker_table){
        puts("alloc_static returned NULL");
        return false;
    }

    /* the first uint128_t will hold the number of stalker structs that
     * are currently in the table
     */
    *(uint64_t *)stalker_table = 0;
    *(uint64_t *)(stalker_table + 0x8) = 0;

    uint8_t *cur_stalker_ctl = stalker_table + 0x10;

    for(int i=0; i<stalker_table_maxelems; i++){
        /* all initial stalker structs are free */
        *(uint32_t *)cur_stalker_ctl = 1;
        *(int32_t *)(cur_stalker_ctl + 0x4) = -1;
        *(uintptr_t *)(cur_stalker_ctl + 0x8) = 0;

        cur_stalker_ctl += 0x10;
    }

    /* print_register(cur_stalker_ctl); */

    /* stash these pointers so we have them after xnu boot */
    WRITE_QWORD_TO_HANDLE_SVC_HOOK_CACHE(g_exception_triage_addr);
    WRITE_QWORD_TO_HANDLE_SVC_HOOK_CACHE(current_proc_addr);
    WRITE_QWORD_TO_HANDLE_SVC_HOOK_CACHE(g_proc_pid_addr);
    /* WRITE_QWORD_TO_HANDLE_SVC_HOOK_CACHE(xnu_ptr_to_va(pid_table)); */
    WRITE_QWORD_TO_HANDLE_SVC_HOOK_CACHE(xnu_ptr_to_va(stalker_table));

    /* autogenerated by hookgen.pl, see handle_svc_patches.h */
    /* Needs to be done before we patch the sysent entry so scratch_space lies
     * right after the end of the handle_svc hook.
     */
    DO_HANDLE_SVC_HOOK_PATCHES;

    /* write the stalker table pointer so the patched syscall can get it easily
     *
     * needs to be here so opcode_stream points after this when we go
     * to patch the sysent entry
     */
    /* WRITE_QWORD_TO_SVC_STALKER_CTL_CACHE(xnu_ptr_to_va(pid_table)); */
    WRITE_QWORD_TO_SVC_STALKER_CTL_CACHE(xnu_ptr_to_va(stalker_table));

    /* iphone 8 13.6 */
    uint64_t IOLog_addr = 0xFFFFFFF008134654 + kernel_slide;
    WRITE_QWORD_TO_SVC_STALKER_CTL_CACHE(IOLog_addr);
    uint64_t IOMalloc_addr = 0xFFFFFFF008133284 + kernel_slide;
    WRITE_QWORD_TO_SVC_STALKER_CTL_CACHE(IOMalloc_addr);

    WRITE_QWORD_TO_SVC_STALKER_CTL_CACHE(g_kalloc_canblock_addr);
    WRITE_QWORD_TO_SVC_STALKER_CTL_CACHE(g_kfree_addr_addr);

    // XXX
    /* return true; */

    /* now we need to find the first enosys entry in sysent to patch
     * our syscall in.
     *
     * enosys literally just returns ENOSYS, so it will be pretty easy
     * to find.
     */
    uint8_t *sysent_stream = (uint8_t *)xnu_va_to_ptr(g_sysent_addr);
    size_t sizeof_struct_sysent = 0x18;

    uint32_t patched_syscall_num = 0;
    uint8_t *sysent_to_patch = NULL;

    bool tagged_ptr = false;
    uint16_t old_tag = 0;

    uint32_t limit = 1000;

    for(uint32_t i=0; ; i++){
        if(limit-- == 0){
            puts("svc_stalker: didn't find a sysent entry with enosys?");
            return false;
        }

        uint64_t sy_call = *(uint64_t *)sysent_stream;

        /* tagged pointer */
        if((sy_call & 0xffff000000000000) != 0xffff000000000000){
            old_tag = (sy_call >> 48);

            sy_call |= 0xffff000000000000;
            sy_call += kernel_slide;

            tagged_ptr = true;
        }

        /* mov w0, ENOSYS; ret */
        // XXX also check for orr w0, wzr, ENOSYS? */
        if(*(uint64_t *)xnu_va_to_ptr(sy_call) == 0xd65f03c0528009c0){
            sysent_to_patch = sysent_stream;
            patched_syscall_num = i;

            /* sy_call */
            if(!tagged_ptr)
                /* (void)0; */
                // XXX
                *(uint64_t *)sysent_to_patch = (uint64_t)xnu_ptr_to_va(scratch_space);
            else{
                uint64_t untagged = ((uint64_t)xnu_ptr_to_va(scratch_space) &
                    0xffffffffffff) - kernel_slide;

                /* re-tag */
                uint64_t new_sy_call = untagged | ((uint64_t)old_tag << 48);

                // XXX
                *(uint64_t *)sysent_to_patch = new_sy_call;
            }

            /* no 32 bit processes on iOS 11+, so no argument munger */
            *(uint64_t *)(sysent_to_patch + 0x8) = 0;

            /* this syscall will return an integer */
            *(int32_t *)(sysent_to_patch + 0x10) = 4;//1; /* _SYSCALL_RET_INT_T */

            /* this syscall has four arguments */
            *(int16_t *)(sysent_to_patch + 0x14) = 4;

            /* four 32 bit arguments, so arguments total 32 bytes */
            *(uint16_t *)(sysent_to_patch + 0x16) = 0x10;

            break;
        }

        sysent_stream += sizeof_struct_sysent;
    }

    /* autogenerated by hookgen.pl, see svc_stalker_ctl_patches.h */
    DO_SVC_STALKER_CTL_PATCHES;

#define IMPORTANT_MSG(x) \
    putchar('*'); \
    putchar(' '); \
    puts(x); \

    puts("***** IMPORTANT *****");
    printf("* System call %#x has been\n", patched_syscall_num);
    IMPORTANT_MSG("patched. It is your way");
    IMPORTANT_MSG("of controlling what");
    IMPORTANT_MSG("processes you intercept");
    IMPORTANT_MSG("system calls for.");
    IMPORTANT_MSG("Please refer back to the");
    IMPORTANT_MSG("README for info about");
    IMPORTANT_MSG("this patched system call.");
    puts("*********************");

    /* return true; */

    // XXX commenting out for testing
    write_blr(8, branch_from, last_TEXT_EXEC_sect_end + handle_svc_hook_cache_size);
    /* there's an extra B.NE after the five instrs we overwrote, so NOP it out */
    *(uint32_t *)(branch_from + (4*5)) = 0xd503201f;


    // XXX
    /* return true; */
    /* XXX return to caller from exception_triage_thread on EXC_SYSCALL & EXC_MACH_SYSCALL */
    /* uint64_t cmp_addr1 = ptr_for_sa(0xFFFFFFF007BF859C); */
    /* uint64_t cmp_addr2 = ptr_for_sa(0xFFFFFFF007BF86AC); */
    
    /* both cmp w25, #-3 */
    /* *(uint32_t *)cmp_addr1 = 0x31000F3F; */
    /* *(uint32_t *)cmp_addr2 = 0x31000F3F; */

    /* uint64_t branch_addr1 = ptr_for_sa(0xFFFFFFF007BF85A0); */
    /* b.cs --> b.lt */
    /* *(uint32_t *)branch_addr1 = 0x540008ab; */


    /* uint64_t branch_addr2 = ptr_for_sa(0xFFFFFFF007BF86B0); */
    /* b.cc --> b.ge */
    /* *(uint32_t *)branch_addr2 = 0x54fff7aa; */

    return true;
}

static void stalker_apply_patches(const char *cmd, char *args){
    /* puts("inside stalker_apply_patches"); */
    xnu_pf_patchset_t *patchset = xnu_pf_patchset_create(XNU_PF_ACCESS_32BIT);

    uint64_t proc_pid_finder_match[] = {
        0x94000000,     /* BL n (_proc_pid) */
        0xf90003e0,     /* STR X0, [SP] (store _proc_pid return value) */

        /* ADRP X0, n or ADR X0, n
         * (X0 = format string which uses return value)
         */
        0x10000000,
    };

    const size_t num_proc_pid_matches = sizeof(proc_pid_finder_match) /
        sizeof(*proc_pid_finder_match);

    uint64_t proc_pid_finder_masks[] = {
        0xfc000000,     /* ignore BL immediate */
        0xffffffff,     /* match exactly */
        0x1f00001f,     /* ignore immediate */
    };

    struct mach_header_64 *AMFI = xnu_pf_get_kext_header(mh_execute_header,
            "com.apple.driver.AppleMobileFileIntegrity");

    xnu_pf_range_t *AMFI___TEXT_EXEC = xnu_pf_segment(AMFI, "__TEXT_EXEC");
    xnu_pf_maskmatch(patchset, proc_pid_finder_match, proc_pid_finder_masks,
            num_proc_pid_matches, false, // XXX for testing,
            proc_pid_finder);
    xnu_pf_apply(AMFI___TEXT_EXEC, patchset);

    uint64_t sysent_finder_match[] = {
        0x1a803000,     /* CSEL Wn, Wn, Wn, CC */
        0x12003c00,     /* AND Wn, Wn, 0xffff */
        0x10000000,     /* ADRP Xn, n or ADR Xn, n */
    };

    const size_t num_sysent_matches = sizeof(sysent_finder_match) /
        sizeof(*sysent_finder_match);

    uint64_t sysent_finder_masks[] = {
        0xffe0fc00,     /* ignore all but condition code */
        0xfffffc00,     /* ignore all but immediate */
        0x1f000000,     /* ignore everything */
    };

    xnu_pf_range_t *__TEXT_EXEC = xnu_pf_segment(mh_execute_header, "__TEXT_EXEC");

    xnu_pf_maskmatch(patchset, sysent_finder_match, sysent_finder_masks,
            num_sysent_matches, false, // XXX for testing,
            sysent_finder);
    xnu_pf_apply(__TEXT_EXEC, patchset);

    uint64_t kalloc_canblock_match[] = {
        0xaa0003f3,     /* MOV X19, X0 */
        0xf90003ff,     /* STR XZR, [SP, n] */
        0xf9400000,     /* LDR Xn, [Xn] */
        0xf11fbc1f,     /* CMP Xn, 0x7ef */
    };

    const size_t num_kalloc_canblock_matches = sizeof(kalloc_canblock_match) /
        sizeof(*kalloc_canblock_match);

    uint64_t kalloc_canblock_masks[] = {
        0xffffffff,     /* match exactly */
        0xffc003ff,     /* ignore immediate */
        0xffc00000,     /* ignore immediate, Rn, and Rt */
        0xfffffc1f,     /* ignore Rn */
    };

    xnu_pf_maskmatch(patchset, kalloc_canblock_match, kalloc_canblock_masks,
            num_kalloc_canblock_matches, false, // XXX for testing,
            kalloc_canblock_finder);
    xnu_pf_apply(__TEXT_EXEC, patchset);

    uint64_t kfree_addr_match[] = {
        0xf90003f3,     /* STR X19, [SP] */
        0x10000000,     /* ADRP Xn, n or ADR Xn, n */
    };

    const size_t num_kfree_addr_matches = sizeof(kfree_addr_match) /
        sizeof(*kfree_addr_match);

    uint64_t kfree_addr_masks[] = {
        0xffffffff,     /* match exactly */
        0x1f000000,     /* ignore everything */
    };

    xnu_pf_maskmatch(patchset, kfree_addr_match, kfree_addr_masks,
            num_kfree_addr_matches, false, // XXX for testing,
            kfree_addr_finder);
    xnu_pf_apply(__TEXT_EXEC, patchset);

    uint64_t mach_syscall_patcher_match[] = {
        0x94000000,     /* BL n (_exception_triage) */
        /* ADRP X0, n or ADR X0, n
         * (X0 = panic string)
         */
        0x10000000,
    };

    const size_t num_mach_syscall_matches = sizeof(mach_syscall_patcher_match) /
        sizeof(*mach_syscall_patcher_match);

    uint64_t mach_syscall_patcher_masks[] = {
        0xfc000000,     /* ignore BL immediate */
        0x1f00001f,     /* ignore immediate */
    };

    xnu_pf_maskmatch(patchset, mach_syscall_patcher_match, mach_syscall_patcher_masks,
            num_mach_syscall_matches, false, // XXX for testing,
            mach_syscall_patcher);
    xnu_pf_apply(__TEXT_EXEC, patchset);

    uint64_t sleh_synchronous_patcher_match[] = {
        0xb9408a60,     /* LDR Wn, [X19, #0x88] (trap_no = state->__x[16] */
        0xd538d080,     /* MRS Xn, TPIDR_EL1    (Xn = current_thread()) */
        0x12800000,     /* MOV Wn, 0xFFFFFFFF   (Wn = THROTTLE_LEVEL_NONE) */
    };

    const size_t num_ss_matches = sizeof(sleh_synchronous_patcher_match) / 
        sizeof(*sleh_synchronous_patcher_match);

    uint64_t sleh_synchronous_patcher_masks[] = {
        0xffffffe0,     /* ignore Wn in LDR */
        0xffffffe0,     /* ignore Xn in MRS */
        0xffffffe0,     /* ignore Wn in MOV */
    };

    xnu_pf_maskmatch(patchset, sleh_synchronous_patcher_match,
            sleh_synchronous_patcher_masks, num_ss_matches, false, // XXX for testing
            sleh_synchronous_patcher);
    xnu_pf_apply(__TEXT_EXEC, patchset);
    xnu_pf_patchset_destroy(patchset);

    /* now we have the address of exception_triage */
    /* xnu_pf_patchset_t *et_patchset = xnu_pf_patchset_create(XNU_PF_ACCESS_32BIT); */
    /* xnu_pf_range_t *exception_triage_range = */
    /*     xnu_pf_range_from_va(g_exception_triage_addr, 0x8); */

    /* print_register(exception_triage_range); */

    /* xnu_pf_maskmatch(et_patchset, NULL, NULL, 0, false, */
    /*         exception_triage_thread_patcher); */
    /* xnu_pf_apply(exception_triage_range, et_patchset); */
    /* xnu_pf_patchset_destroy(et_patchset); */




    puts("------stalker_apply_patches DONE------");
}

static void stalker_preboot_hook(void){
    puts("inside stalker_preboot_hook");

    if(next_preboot_hook)
        next_preboot_hook();
}

void module_entry(void){
    puts("svc_stalker pongoOS module entry");

    mh_execute_header = xnu_header();
    kernel_slide = xnu_slide_value(mh_execute_header);

    next_preboot_hook = preboot_hook;
    preboot_hook = stalker_preboot_hook;

    command_register("stalker-patch", "apply sleh_synchronous patches",
            stalker_apply_patches);
}

const char *module_name = "svc_stalker";

struct pongo_exports exported_symbols[] = {
    { .name = 0, .value = 0 }
};
