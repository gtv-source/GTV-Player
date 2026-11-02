/*
 * QEMU AEHD accelerator - x86 part
 *
 * Ported from the Android Emulator's QEMU (external/qemu, emu-master-dev,
 * target/i386/aehd.c) to QEMU 9.2.  Userspace irqchip only; no SMM, PMU
 * passthrough, guest debugging or migration support.
 *
 * Copyright (C) 2006-2008 Qumranet Technologies
 * Copyright IBM, Corp. 2008
 * Copyright (c) 2017 Intel Corporation (Haitao Shan <hshan@google.com>)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/memalign.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/i386/apic.h"
#include "hw/i386/e820_memory_layout.h"
#include "sysemu/runstate.h"
#include "sysemu/aehd.h"
#include "aehd-int.h"
#include "aehd-i386.h"

static bool has_msr_star;
static bool has_msr_hsave_pa;
static bool has_msr_tsc_aux;
static bool has_msr_tsc_adjust;
static bool has_msr_tsc_deadline;
static bool has_msr_feature_control;
static bool has_msr_misc_enable;
static bool has_msr_bndcfgs;
static bool has_msr_mtrr;
static bool has_msr_xss;

static int has_xsave;
static int has_xcrs;

static struct aehd_cpuid *cpuid_cache;

/* ===== CPUID ===== */

static struct aehd_cpuid *try_get_cpuid(AEHDState *s, int max)
{
    int size = sizeof(struct aehd_cpuid) + max * sizeof(struct aehd_cpuid_entry);
    struct aehd_cpuid *cpuid = g_malloc0(size);
    int r;

    cpuid->nent = max;
    r = aehd_ioctl(s, AEHD_GET_SUPPORTED_CPUID, cpuid, size, cpuid, size);
    if (r == 0 && cpuid->nent >= max) {
        r = -E2BIG;
    }
    if (r == -E2BIG) {
        g_free(cpuid);
        return NULL;
    }
    if (r < 0) {
        error_report("AEHD: AEHD_GET_SUPPORTED_CPUID failed: %s", strerror(-r));
        exit(1);
    }
    return cpuid;
}

static struct aehd_cpuid *get_supported_cpuid(AEHDState *s)
{
    int max = 1;

    if (!cpuid_cache) {
        while ((cpuid_cache = try_get_cpuid(s, max)) == NULL) {
            max *= 2;
        }
    }
    return cpuid_cache;
}

static struct aehd_cpuid_entry *cpuid_find_entry(struct aehd_cpuid *cpuid,
                                                 uint32_t function,
                                                 uint32_t index)
{
    for (int i = 0; i < cpuid->nent; ++i) {
        if (cpuid->entries[i].function == function &&
            cpuid->entries[i].index == index) {
            return &cpuid->entries[i];
        }
    }
    return NULL;
}

uint32_t aehd_arch_get_supported_cpuid(uint32_t function, uint32_t index,
                                       int reg)
{
    struct aehd_cpuid_entry *entry =
        cpuid_find_entry(get_supported_cpuid(aehd_state), function, index);
    uint32_t ret = 0;

    if (entry) {
        switch (reg) {
        case R_EAX:
            ret = entry->eax;
            break;
        case R_EBX:
            ret = entry->ebx;
            break;
        case R_ECX:
            ret = entry->ecx;
            break;
        case R_EDX:
            ret = entry->edx;
            break;
        }
    }

    if (function == 1 && reg == R_ECX) {
        /* the hypervisor bit is always fine; x2apic needs an in-kernel APIC */
        ret |= CPUID_EXT_HYPERVISOR;
        ret &= ~CPUID_EXT_X2APIC;
    } else if (function == 6 && reg == R_EAX) {
        ret |= CPUID_6_EAX_ARAT;    /* emulated APIC timer is always running */
    } else if (function == 0x80000001 && reg == R_EDX) {
        /* Intel CPUs report these per the Intel spec; add the AMD aliases */
        ret |= aehd_arch_get_supported_cpuid(1, 0, R_EDX) &
               CPUID_EXT2_AMD_ALIASES;
    }
    return ret;
}

static void cpu_update_state(void *opaque, bool running, RunState state)
{
    CPUX86State *env = opaque;

    if (running) {
        env->tsc_valid = false;
    }
}

unsigned long aehd_arch_vcpu_id(CPUState *cs)
{
    return X86_CPU(cs)->apic_id;
}

#define AEHD_MAX_CPUID_ENTRIES  100

int aehd_arch_init_vcpu(CPUState *cs)
{
    struct {
        struct aehd_cpuid cpuid;
        struct aehd_cpuid_entry entries[AEHD_MAX_CPUID_ENTRIES];
    } cpuid_data;
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;
    uint32_t limit, i, j, cpuid_i = 0;
    uint32_t unused;
    struct aehd_cpuid_entry *c;
    int r;

    memset(&cpuid_data, 0, sizeof(cpuid_data));

    cpu_x86_cpuid(env, 0, 0, &limit, &unused, &unused, &unused);
    for (i = 0; i <= limit; i++) {
        if (cpuid_i == AEHD_MAX_CPUID_ENTRIES) {
            error_report("AEHD: unsupported CPUID level 0x%x", limit);
            abort();
        }
        c = &cpuid_data.entries[cpuid_i++];

        switch (i) {
        case 2: {
            /* keep reading function 2 till all the input is received */
            int times;

            c->function = i;
            c->flags = AEHD_CPUID_FLAG_STATEFUL_FUNC |
                       AEHD_CPUID_FLAG_STATE_READ_NEXT;
            cpu_x86_cpuid(env, i, 0, &c->eax, &c->ebx, &c->ecx, &c->edx);
            times = c->eax & 0xff;
            for (j = 1; j < times; ++j) {
                if (cpuid_i == AEHD_MAX_CPUID_ENTRIES) {
                    error_report("AEHD: CPUID table full (leaf 2)");
                    abort();
                }
                c = &cpuid_data.entries[cpuid_i++];
                c->function = i;
                c->flags = AEHD_CPUID_FLAG_STATEFUL_FUNC;
                cpu_x86_cpuid(env, i, 0, &c->eax, &c->ebx, &c->ecx, &c->edx);
            }
            break;
        }
        case 4:
        case 0xb:
        case 0xd:
            for (j = 0; ; j++) {
                if (i == 0xd && j == 64) {
                    break;
                }
                c->function = i;
                c->flags = AEHD_CPUID_FLAG_SIGNIFCANT_INDEX;
                c->index = j;
                cpu_x86_cpuid(env, i, j, &c->eax, &c->ebx, &c->ecx, &c->edx);

                if (i == 4 && c->eax == 0) {
                    break;
                }
                if (i == 0xb && !(c->ecx & 0xff00)) {
                    break;
                }
                if (i == 0xd && c->eax == 0) {
                    continue;
                }
                if (cpuid_i == AEHD_MAX_CPUID_ENTRIES) {
                    error_report("AEHD: CPUID table full (leaf 0x%x)", i);
                    abort();
                }
                c = &cpuid_data.entries[cpuid_i++];
            }
            break;
        default:
            c->function = i;
            c->flags = 0;
            cpu_x86_cpuid(env, i, 0, &c->eax, &c->ebx, &c->ecx, &c->edx);
            break;
        }
    }

    cpu_x86_cpuid(env, 0x80000000, 0, &limit, &unused, &unused, &unused);
    for (i = 0x80000000; i <= limit; i++) {
        if (cpuid_i == AEHD_MAX_CPUID_ENTRIES) {
            error_report("AEHD: unsupported CPUID xlevel 0x%x", limit);
            abort();
        }
        c = &cpuid_data.entries[cpuid_i++];
        c->function = i;
        c->flags = 0;
        cpu_x86_cpuid(env, i, 0, &c->eax, &c->ebx, &c->ecx, &c->edx);
    }

    cpuid_data.cpuid.nent = cpuid_i;
    cpuid_data.cpuid.padding = 0;

    qemu_add_vm_change_state_handler(cpu_update_state, env);

    c = cpuid_find_entry(&cpuid_data.cpuid, 1, 0);
    if (c) {
        has_msr_feature_control = !!(c->ecx & CPUID_EXT_VMX) ||
                                  !!(c->ecx & CPUID_EXT_SMX);
    }

    r = aehd_vcpu_ioctl(cs, AEHD_SET_CPUID, &cpuid_data, sizeof(cpuid_data),
                        NULL, 0);
    if (r) {
        error_report("AEHD: AEHD_SET_CPUID failed");
        return r;
    }

    if (has_xsave) {
        cs->accel->xsave_buf = qemu_memalign(4096, sizeof(struct aehd_xsave));
    }
    cs->accel->msr_buf = g_malloc0(AEHD_MSR_BUF_SIZE);

    if (env->features[FEAT_1_EDX] & CPUID_MTRR) {
        has_msr_mtrr = true;
    }
    if (!(env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_RDTSCP)) {
        has_msr_tsc_aux = false;
    }
    return 0;
}

void aehd_arch_reset_vcpu(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;

    env->xcr0 = 1;
    /* userspace APIC: every vCPU is runnable, the APIC model gates SIPI */
    env->mp_state = AEHD_MP_STATE_RUNNABLE;
}

void aehd_arch_do_init_vcpu(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;

    /* APs get directly into wait-for-SIPI state */
    if (env->mp_state == AEHD_MP_STATE_UNINITIALIZED) {
        env->mp_state = AEHD_MP_STATE_INIT_RECEIVED;
    }
}

static int aehd_get_supported_msrs(AEHDState *s)
{
    struct aehd_msr_list msr_list, *list;
    unsigned long size;
    int ret;

    /* first call gets the count, second one the indices */
    msr_list.nmsrs = 0;
    ret = aehd_ioctl(s, AEHD_GET_MSR_INDEX_LIST, &msr_list, sizeof(msr_list),
                     &msr_list, sizeof(msr_list));
    if (ret < 0 && ret != -E2BIG) {
        return ret;
    }

    size = sizeof(msr_list) + msr_list.nmsrs * sizeof(msr_list.indices[0]);
    list = g_malloc0(size);
    list->nmsrs = msr_list.nmsrs;
    ret = aehd_ioctl(s, AEHD_GET_MSR_INDEX_LIST, list, size, list, size);
    if (ret >= 0) {
        for (int i = 0; i < list->nmsrs; i++) {
            switch (list->indices[i]) {
            case MSR_STAR:
                has_msr_star = true;
                break;
            case MSR_VM_HSAVE_PA:
                has_msr_hsave_pa = true;
                break;
            case MSR_TSC_AUX:
                has_msr_tsc_aux = true;
                break;
            case MSR_TSC_ADJUST:
                has_msr_tsc_adjust = true;
                break;
            case MSR_IA32_TSCDEADLINE:
                has_msr_tsc_deadline = true;
                break;
            case MSR_IA32_MISC_ENABLE:
                has_msr_misc_enable = true;
                break;
            case MSR_IA32_BNDCFGS:
                has_msr_bndcfgs = true;
                break;
            case MSR_IA32_XSS:
                has_msr_xss = true;
                break;
            }
        }
    }
    g_free(list);
    return ret;
}

int aehd_arch_init(MachineState *ms, AEHDState *s)
{
    /* allows up to 16M BIOSes */
    uint64_t identity_base = 0xfeffc000;
    uint64_t tss_base;
    int ret;

    has_xsave = aehd_check_extension(s, AEHD_CAP_XSAVE);
    has_xcrs = aehd_check_extension(s, AEHD_CAP_XCRS);

    ret = aehd_get_supported_msrs(s);
    if (ret < 0) {
        error_report("AEHD: AEHD_GET_MSR_INDEX_LIST failed");
        return ret;
    }

    /*
     * On older Intel CPUs the driver runs 16-bit code in vm86 mode, which
     * needs an EPT identity map and a TSS inside guest physical memory:
     * 4 pages just below the BIOS, reserved in the e820 map.
     */
    ret = aehd_vm_ioctl(s, AEHD_SET_IDENTITY_MAP_ADDR, &identity_base,
                        sizeof(identity_base), NULL, 0);
    if (ret < 0) {
        error_report("AEHD: AEHD_SET_IDENTITY_MAP_ADDR failed");
        return ret;
    }
    tss_base = identity_base + 0x1000;
    ret = aehd_vm_ioctl(s, AEHD_SET_TSS_ADDR, &tss_base, sizeof(tss_base),
                        NULL, 0);
    if (ret < 0) {
        error_report("AEHD: AEHD_SET_TSS_ADDR failed");
        return ret;
    }
    e820_add_entry(identity_base, 0x4000, E820_RESERVED);
    return 0;
}

/* ===== register state ===== */

static void set_v8086_seg(struct aehd_segment *lhs, const SegmentCache *rhs)
{
    lhs->selector = rhs->selector;
    lhs->base = rhs->base;
    lhs->limit = rhs->limit;
    lhs->type = 3;
    lhs->present = 1;
    lhs->dpl = 3;
    lhs->db = 0;
    lhs->s = 1;
    lhs->l = 0;
    lhs->g = 0;
    lhs->avl = 0;
    lhs->unusable = 0;
}

static void set_seg(struct aehd_segment *lhs, const SegmentCache *rhs)
{
    unsigned flags = rhs->flags;

    lhs->selector = rhs->selector;
    lhs->base = rhs->base;
    lhs->limit = rhs->limit;
    lhs->type = (flags >> DESC_TYPE_SHIFT) & 15;
    lhs->present = (flags & DESC_P_MASK) != 0;
    lhs->dpl = (flags >> DESC_DPL_SHIFT) & 3;
    lhs->db = (flags >> DESC_B_SHIFT) & 1;
    lhs->s = (flags & DESC_S_MASK) != 0;
    lhs->l = (flags >> DESC_L_SHIFT) & 1;
    lhs->g = (flags & DESC_G_MASK) != 0;
    lhs->avl = (flags & DESC_AVL_MASK) != 0;
    lhs->unusable = !lhs->present;
    lhs->padding = 0;
}

static void get_seg(SegmentCache *lhs, const struct aehd_segment *rhs)
{
    lhs->selector = rhs->selector;
    lhs->base = rhs->base;
    lhs->limit = rhs->limit;
    if (rhs->unusable) {
        lhs->flags = 0;
    } else {
        lhs->flags = (rhs->type << DESC_TYPE_SHIFT) |
                     (rhs->present * DESC_P_MASK) |
                     (rhs->dpl << DESC_DPL_SHIFT) |
                     (rhs->db << DESC_B_SHIFT) |
                     (rhs->s * DESC_S_MASK) |
                     (rhs->l << DESC_L_SHIFT) |
                     (rhs->g * DESC_G_MASK) |
                     (rhs->avl * DESC_AVL_MASK);
    }
}

static void aehd_getput_reg(__u64 *aehd_reg, target_ulong *qemu_reg, int set)
{
    if (set) {
        *aehd_reg = *qemu_reg;
    } else {
        *qemu_reg = *aehd_reg;
    }
}

static int aehd_getput_regs(X86CPU *cpu, int set)
{
    CPUX86State *env = &cpu->env;
    struct aehd_regs regs;
    int ret;

    if (!set) {
        ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_REGS, NULL, 0,
                              &regs, sizeof(regs));
        if (ret < 0) {
            return ret;
        }
    }

    aehd_getput_reg(&regs.rax, &env->regs[R_EAX], set);
    aehd_getput_reg(&regs.rbx, &env->regs[R_EBX], set);
    aehd_getput_reg(&regs.rcx, &env->regs[R_ECX], set);
    aehd_getput_reg(&regs.rdx, &env->regs[R_EDX], set);
    aehd_getput_reg(&regs.rsi, &env->regs[R_ESI], set);
    aehd_getput_reg(&regs.rdi, &env->regs[R_EDI], set);
    aehd_getput_reg(&regs.rsp, &env->regs[R_ESP], set);
    aehd_getput_reg(&regs.rbp, &env->regs[R_EBP], set);
#ifdef TARGET_X86_64
    aehd_getput_reg(&regs.r8, &env->regs[8], set);
    aehd_getput_reg(&regs.r9, &env->regs[9], set);
    aehd_getput_reg(&regs.r10, &env->regs[10], set);
    aehd_getput_reg(&regs.r11, &env->regs[11], set);
    aehd_getput_reg(&regs.r12, &env->regs[12], set);
    aehd_getput_reg(&regs.r13, &env->regs[13], set);
    aehd_getput_reg(&regs.r14, &env->regs[14], set);
    aehd_getput_reg(&regs.r15, &env->regs[15], set);
#endif
    aehd_getput_reg(&regs.rflags, &env->eflags, set);
    aehd_getput_reg(&regs.rip, &env->eip, set);

    if (set) {
        return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_REGS, &regs, sizeof(regs),
                               NULL, 0);
    }
    return 0;
}

static int aehd_put_fpu(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_fpu fpu;

    memset(&fpu, 0, sizeof fpu);
    fpu.fsw = env->fpus & ~(7 << 11);
    fpu.fsw |= (env->fpstt & 7) << 11;
    fpu.fcw = env->fpuc;
    fpu.last_opcode = env->fpop;
    fpu.last_ip = env->fpip;
    fpu.last_dp = env->fpdp;
    for (int i = 0; i < 8; ++i) {
        fpu.ftwx |= (!env->fptags[i]) << i;
    }
    memcpy(fpu.fpr, env->fpregs, sizeof env->fpregs);
    for (int i = 0; i < CPU_NB_REGS; i++) {
        stq_p(&fpu.xmm[i][0], env->xmm_regs[i].ZMM_Q(0));
        stq_p(&fpu.xmm[i][8], env->xmm_regs[i].ZMM_Q(1));
    }
    fpu.mxcsr = env->mxcsr;
    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_FPU, &fpu, sizeof(fpu), NULL, 0);
}

static int aehd_get_fpu(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_fpu fpu;
    int ret;

    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_FPU, NULL, 0, &fpu, sizeof(fpu));
    if (ret < 0) {
        return ret;
    }
    env->fpstt = (fpu.fsw >> 11) & 7;
    env->fpus = fpu.fsw;
    env->fpuc = fpu.fcw;
    env->fpop = fpu.last_opcode;
    env->fpip = fpu.last_ip;
    env->fpdp = fpu.last_dp;
    for (int i = 0; i < 8; ++i) {
        env->fptags[i] = !((fpu.ftwx >> i) & 1);
    }
    memcpy(env->fpregs, fpu.fpr, sizeof env->fpregs);
    for (int i = 0; i < CPU_NB_REGS; i++) {
        env->xmm_regs[i].ZMM_Q(0) = ldq_p(&fpu.xmm[i][0]);
        env->xmm_regs[i].ZMM_Q(1) = ldq_p(&fpu.xmm[i][8]);
    }
    env->mxcsr = fpu.mxcsr;
    return 0;
}

/* the driver uses the standard (non-compacted) 4 KiB XSAVE layout */
static int aehd_put_xsave(X86CPU *cpu)
{
    struct aehd_xsave *xsave = CPU(cpu)->accel->xsave_buf;

    if (!has_xsave) {
        return aehd_put_fpu(cpu);
    }
    memset(xsave, 0, sizeof(*xsave));
    x86_cpu_xsave_all_areas(cpu, xsave, sizeof(*xsave));
    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_XSAVE, xsave, sizeof(*xsave),
                           NULL, 0);
}

static int aehd_get_xsave(X86CPU *cpu)
{
    struct aehd_xsave *xsave = CPU(cpu)->accel->xsave_buf;
    int ret;

    if (!has_xsave) {
        return aehd_get_fpu(cpu);
    }
    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_XSAVE, NULL, 0,
                          xsave, sizeof(*xsave));
    if (ret < 0) {
        return ret;
    }
    x86_cpu_xrstor_all_areas(cpu, xsave, sizeof(*xsave));
    return 0;
}

static int aehd_put_xcrs(X86CPU *cpu)
{
    struct aehd_xcrs xcrs = {};

    if (!has_xcrs) {
        return 0;
    }
    xcrs.nr_xcrs = 1;
    xcrs.xcrs[0].xcr = 0;
    xcrs.xcrs[0].value = cpu->env.xcr0;
    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_XCRS, &xcrs, sizeof(xcrs),
                           NULL, 0);
}

static int aehd_get_xcrs(X86CPU *cpu)
{
    struct aehd_xcrs xcrs;
    int ret;

    if (!has_xcrs) {
        return 0;
    }
    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_XCRS, NULL, 0,
                          &xcrs, sizeof(xcrs));
    if (ret < 0) {
        return ret;
    }
    for (int i = 0; i < xcrs.nr_xcrs; i++) {
        if (xcrs.xcrs[i].xcr == 0) {    /* only xcr0 is supported */
            cpu->env.xcr0 = xcrs.xcrs[i].value;
            break;
        }
    }
    return 0;
}

static int aehd_put_sregs(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_sregs sregs;

    memset(&sregs, 0, sizeof(sregs));
    if (env->interrupt_injected >= 0) {
        sregs.interrupt_bitmap[env->interrupt_injected / 64] |=
            (uint64_t)1 << (env->interrupt_injected % 64);
    }

    if (env->eflags & VM_MASK) {
        set_v8086_seg(&sregs.cs, &env->segs[R_CS]);
        set_v8086_seg(&sregs.ds, &env->segs[R_DS]);
        set_v8086_seg(&sregs.es, &env->segs[R_ES]);
        set_v8086_seg(&sregs.fs, &env->segs[R_FS]);
        set_v8086_seg(&sregs.gs, &env->segs[R_GS]);
        set_v8086_seg(&sregs.ss, &env->segs[R_SS]);
    } else {
        set_seg(&sregs.cs, &env->segs[R_CS]);
        set_seg(&sregs.ds, &env->segs[R_DS]);
        set_seg(&sregs.es, &env->segs[R_ES]);
        set_seg(&sregs.fs, &env->segs[R_FS]);
        set_seg(&sregs.gs, &env->segs[R_GS]);
        set_seg(&sregs.ss, &env->segs[R_SS]);
    }
    set_seg(&sregs.tr, &env->tr);
    set_seg(&sregs.ldt, &env->ldt);

    sregs.idt.limit = env->idt.limit;
    sregs.idt.base = env->idt.base;
    sregs.gdt.limit = env->gdt.limit;
    sregs.gdt.base = env->gdt.base;

    sregs.cr0 = env->cr[0];
    sregs.cr2 = env->cr[2];
    sregs.cr3 = env->cr[3];
    sregs.cr4 = env->cr[4];
    sregs.cr8 = cpu_get_apic_tpr(cpu->apic_state);
    sregs.apic_base = cpu_get_apic_base(cpu->apic_state);
    sregs.efer = env->efer;

    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_SREGS, &sregs, sizeof(sregs),
                           NULL, 0);
}

static int aehd_get_sregs(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_sregs sregs;
    uint32_t hflags;
    int ret;

    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_SREGS, NULL, 0,
                          &sregs, sizeof(sregs));
    if (ret < 0) {
        return ret;
    }

    /* at most one pending IRQ can be set in the bitmap */
    env->interrupt_injected = -1;
    for (int i = 0; i < ARRAY_SIZE(sregs.interrupt_bitmap); i++) {
        if (sregs.interrupt_bitmap[i]) {
            env->interrupt_injected = i * 64 + ctz64(sregs.interrupt_bitmap[i]);
            break;
        }
    }

    get_seg(&env->segs[R_CS], &sregs.cs);
    get_seg(&env->segs[R_DS], &sregs.ds);
    get_seg(&env->segs[R_ES], &sregs.es);
    get_seg(&env->segs[R_FS], &sregs.fs);
    get_seg(&env->segs[R_GS], &sregs.gs);
    get_seg(&env->segs[R_SS], &sregs.ss);
    get_seg(&env->tr, &sregs.tr);
    get_seg(&env->ldt, &sregs.ldt);

    env->idt.limit = sregs.idt.limit;
    env->idt.base = sregs.idt.base;
    env->gdt.limit = sregs.gdt.limit;
    env->gdt.base = sregs.gdt.base;

    env->cr[0] = sregs.cr0;
    env->cr[2] = sregs.cr2;
    env->cr[3] = sregs.cr3;
    env->cr[4] = sregs.cr4;
    env->efer = sregs.efer;
    /* apic base and cr8/tpr are read back in aehd_arch_post_run */

#define HFLAG_COPY_MASK \
    ~(HF_CPL_MASK | HF_PE_MASK | HF_MP_MASK | HF_EM_MASK | \
      HF_TS_MASK | HF_TF_MASK | HF_VM_MASK | HF_IOPL_MASK | \
      HF_OSFXSR_MASK | HF_LMA_MASK | HF_CS32_MASK | \
      HF_SS32_MASK | HF_CS64_MASK | HF_ADDSEG_MASK)

    hflags = env->hflags & HFLAG_COPY_MASK;
    hflags |= (env->segs[R_SS].flags >> DESC_DPL_SHIFT) & HF_CPL_MASK;
    hflags |= (env->cr[0] & CR0_PE_MASK) << (HF_PE_SHIFT - CR0_PE_SHIFT);
    hflags |= (env->cr[0] << (HF_MP_SHIFT - CR0_MP_SHIFT)) &
              (HF_MP_MASK | HF_EM_MASK | HF_TS_MASK);
    hflags |= (env->eflags & (HF_TF_MASK | HF_VM_MASK | HF_IOPL_MASK));
    if (env->cr[4] & CR4_OSFXSR_MASK) {
        hflags |= HF_OSFXSR_MASK;
    }
    if (env->efer & MSR_EFER_LMA) {
        hflags |= HF_LMA_MASK;
    }
    if ((hflags & HF_LMA_MASK) && (env->segs[R_CS].flags & DESC_L_MASK)) {
        hflags |= HF_CS32_MASK | HF_SS32_MASK | HF_CS64_MASK;
    } else {
        hflags |= (env->segs[R_CS].flags & DESC_B_MASK) >>
                  (DESC_B_SHIFT - HF_CS32_SHIFT);
        hflags |= (env->segs[R_SS].flags & DESC_B_MASK) >>
                  (DESC_B_SHIFT - HF_SS32_SHIFT);
        if (!(env->cr[0] & CR0_PE_MASK) || (env->eflags & VM_MASK) ||
            !(hflags & HF_CS32_MASK)) {
            hflags |= HF_ADDSEG_MASK;
        } else {
            hflags |= ((env->segs[R_DS].base | env->segs[R_ES].base |
                        env->segs[R_SS].base) != 0) << HF_ADDSEG_SHIFT;
        }
    }
    env->hflags = hflags;
    return 0;
}

/* ----- MSRs ----- */

static void aehd_msr_buf_reset(X86CPU *cpu)
{
    memset(CPU(cpu)->accel->msr_buf, 0, AEHD_MSR_BUF_SIZE);
}

static void aehd_msr_entry_add(X86CPU *cpu, uint32_t index, uint64_t value)
{
    struct aehd_msrs *msrs = CPU(cpu)->accel->msr_buf;
    struct aehd_msr_entry *entry = &msrs->entries[msrs->nmsrs];

    assert((void *)(entry + 1) <= (void *)msrs + AEHD_MSR_BUF_SIZE);
    entry->index = index;
    entry->reserved = 0;
    entry->data = value;
    msrs->nmsrs++;
}

static int aehd_set_msr_buf(X86CPU *cpu)
{
    struct aehd_msrs *msrs = CPU(cpu)->accel->msr_buf;

    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_MSRS, msrs,
                           sizeof(*msrs) + msrs->nmsrs * sizeof(msrs->entries[0]),
                           msrs, sizeof(*msrs));
}

/*
 * Written separately and first: it kicks the vCPU out of VMXON (or guest
 * mode) on reset, which invalidates any state written before it.
 */
static int aehd_put_msr_feature_control(X86CPU *cpu)
{
    if (!has_msr_feature_control) {
        return 0;
    }
    aehd_msr_buf_reset(cpu);
    aehd_msr_entry_add(cpu, MSR_IA32_FEATURE_CONTROL,
                       cpu->env.msr_ia32_feature_control);
    return aehd_set_msr_buf(cpu);
}

static int aehd_put_tscdeadline_msr(X86CPU *cpu)
{
    if (!has_msr_tsc_deadline) {
        return 0;
    }
    aehd_msr_buf_reset(cpu);
    aehd_msr_entry_add(cpu, MSR_IA32_TSCDEADLINE, cpu->env.tsc_deadline);
    return aehd_set_msr_buf(cpu);
}

static int aehd_put_msrs(X86CPU *cpu, int level)
{
    CPUX86State *env = &cpu->env;

    aehd_msr_buf_reset(cpu);
    aehd_msr_entry_add(cpu, MSR_IA32_SYSENTER_CS, env->sysenter_cs);
    aehd_msr_entry_add(cpu, MSR_IA32_SYSENTER_ESP, env->sysenter_esp);
    aehd_msr_entry_add(cpu, MSR_IA32_SYSENTER_EIP, env->sysenter_eip);
    aehd_msr_entry_add(cpu, MSR_PAT, env->pat);
    if (has_msr_star) {
        aehd_msr_entry_add(cpu, MSR_STAR, env->star);
    }
    if (has_msr_hsave_pa) {
        aehd_msr_entry_add(cpu, MSR_VM_HSAVE_PA, env->vm_hsave);
    }
    if (has_msr_tsc_aux) {
        aehd_msr_entry_add(cpu, MSR_TSC_AUX, env->tsc_aux);
    }
    if (has_msr_tsc_adjust) {
        aehd_msr_entry_add(cpu, MSR_TSC_ADJUST, env->tsc_adjust);
    }
    if (has_msr_misc_enable) {
        aehd_msr_entry_add(cpu, MSR_IA32_MISC_ENABLE,
                           env->msr_ia32_misc_enable);
    }
    if (has_msr_bndcfgs) {
        aehd_msr_entry_add(cpu, MSR_IA32_BNDCFGS, env->msr_bndcfgs);
    }
    if (has_msr_xss) {
        aehd_msr_entry_add(cpu, MSR_IA32_XSS, env->xss);
    }
#ifdef TARGET_X86_64
    aehd_msr_entry_add(cpu, MSR_CSTAR, env->cstar);
    aehd_msr_entry_add(cpu, MSR_KERNELGSBASE, env->kernelgsbase);
    aehd_msr_entry_add(cpu, MSR_FMASK, env->fmask);
    aehd_msr_entry_add(cpu, MSR_LSTAR, env->lstar);
#endif
    /* side effects on the guest: only on reset or full state updates */
    if (level >= AEHD_PUT_RESET_STATE) {
        aehd_msr_entry_add(cpu, MSR_IA32_TSC, env->tsc);
        if (has_msr_mtrr) {
            uint64_t phys_mask = MAKE_64BIT_MASK(0, cpu->phys_bits);

            aehd_msr_entry_add(cpu, MSR_MTRRdefType, env->mtrr_deftype);
            aehd_msr_entry_add(cpu, MSR_MTRRfix64K_00000, env->mtrr_fixed[0]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix16K_80000, env->mtrr_fixed[1]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix16K_A0000, env->mtrr_fixed[2]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_C0000, env->mtrr_fixed[3]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_C8000, env->mtrr_fixed[4]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_D0000, env->mtrr_fixed[5]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_D8000, env->mtrr_fixed[6]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_E0000, env->mtrr_fixed[7]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_E8000, env->mtrr_fixed[8]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_F0000, env->mtrr_fixed[9]);
            aehd_msr_entry_add(cpu, MSR_MTRRfix4K_F8000, env->mtrr_fixed[10]);
            for (int i = 0; i < MSR_MTRRcap_VCNT; i++) {
                /* writing bits above the host's physical limit #GPs */
                aehd_msr_entry_add(cpu, MSR_MTRRphysBase(i),
                                   env->mtrr_var[i].base);
                aehd_msr_entry_add(cpu, MSR_MTRRphysMask(i),
                                   env->mtrr_var[i].mask & phys_mask);
            }
        }
    }
    return aehd_set_msr_buf(cpu);
}

static int aehd_get_msrs(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_msrs *buf = CPU(cpu)->accel->msr_buf;
    struct aehd_msr_entry *msrs = buf->entries;
    uint64_t mtrr_top_bits = 0;
    uint64_t bufsize;
    int ret;

    aehd_msr_buf_reset(cpu);
    aehd_msr_entry_add(cpu, MSR_IA32_SYSENTER_CS, 0);
    aehd_msr_entry_add(cpu, MSR_IA32_SYSENTER_ESP, 0);
    aehd_msr_entry_add(cpu, MSR_IA32_SYSENTER_EIP, 0);
    aehd_msr_entry_add(cpu, MSR_PAT, 0);
    if (has_msr_star) {
        aehd_msr_entry_add(cpu, MSR_STAR, 0);
    }
    if (has_msr_hsave_pa) {
        aehd_msr_entry_add(cpu, MSR_VM_HSAVE_PA, 0);
    }
    if (has_msr_tsc_aux) {
        aehd_msr_entry_add(cpu, MSR_TSC_AUX, 0);
    }
    if (has_msr_tsc_adjust) {
        aehd_msr_entry_add(cpu, MSR_TSC_ADJUST, 0);
    }
    if (has_msr_tsc_deadline) {
        aehd_msr_entry_add(cpu, MSR_IA32_TSCDEADLINE, 0);
    }
    if (has_msr_misc_enable) {
        aehd_msr_entry_add(cpu, MSR_IA32_MISC_ENABLE, 0);
    }
    if (has_msr_feature_control) {
        aehd_msr_entry_add(cpu, MSR_IA32_FEATURE_CONTROL, 0);
    }
    if (has_msr_bndcfgs) {
        aehd_msr_entry_add(cpu, MSR_IA32_BNDCFGS, 0);
    }
    if (has_msr_xss) {
        aehd_msr_entry_add(cpu, MSR_IA32_XSS, 0);
    }
    if (!env->tsc_valid) {
        aehd_msr_entry_add(cpu, MSR_IA32_TSC, 0);
        env->tsc_valid = !runstate_is_running();
    }
#ifdef TARGET_X86_64
    aehd_msr_entry_add(cpu, MSR_CSTAR, 0);
    aehd_msr_entry_add(cpu, MSR_KERNELGSBASE, 0);
    aehd_msr_entry_add(cpu, MSR_FMASK, 0);
    aehd_msr_entry_add(cpu, MSR_LSTAR, 0);
#endif
    if (has_msr_mtrr) {
        aehd_msr_entry_add(cpu, MSR_MTRRdefType, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix64K_00000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix16K_80000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix16K_A0000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_C0000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_C8000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_D0000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_D8000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_E0000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_E8000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_F0000, 0);
        aehd_msr_entry_add(cpu, MSR_MTRRfix4K_F8000, 0);
        for (int i = 0; i < MSR_MTRRcap_VCNT; i++) {
            aehd_msr_entry_add(cpu, MSR_MTRRphysBase(i), 0);
            aehd_msr_entry_add(cpu, MSR_MTRRphysMask(i), 0);
        }
    }

    bufsize = sizeof(*buf) + buf->nmsrs * sizeof(buf->entries[0]);
    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_MSRS, buf, bufsize, buf, bufsize);
    if (ret < 0) {
        return ret;
    }

    /* keep the MTRR mask bits above our phys_bits consistent, see kvm.c */
    if (cpu->fill_mtrr_mask) {
        mtrr_top_bits = MAKE_64BIT_MASK(cpu->phys_bits, 52 - cpu->phys_bits);
    }

    for (int i = 0; i < buf->nmsrs; i++) {
        uint32_t index = msrs[i].index;
        uint64_t data = msrs[i].data;

        switch (index) {
        case MSR_IA32_SYSENTER_CS:
            env->sysenter_cs = data;
            break;
        case MSR_IA32_SYSENTER_ESP:
            env->sysenter_esp = data;
            break;
        case MSR_IA32_SYSENTER_EIP:
            env->sysenter_eip = data;
            break;
        case MSR_PAT:
            env->pat = data;
            break;
        case MSR_STAR:
            env->star = data;
            break;
#ifdef TARGET_X86_64
        case MSR_CSTAR:
            env->cstar = data;
            break;
        case MSR_KERNELGSBASE:
            env->kernelgsbase = data;
            break;
        case MSR_FMASK:
            env->fmask = data;
            break;
        case MSR_LSTAR:
            env->lstar = data;
            break;
#endif
        case MSR_IA32_TSC:
            env->tsc = data;
            break;
        case MSR_TSC_AUX:
            env->tsc_aux = data;
            break;
        case MSR_TSC_ADJUST:
            env->tsc_adjust = data;
            break;
        case MSR_IA32_TSCDEADLINE:
            env->tsc_deadline = data;
            break;
        case MSR_VM_HSAVE_PA:
            env->vm_hsave = data;
            break;
        case MSR_IA32_MISC_ENABLE:
            env->msr_ia32_misc_enable = data;
            break;
        case MSR_IA32_FEATURE_CONTROL:
            env->msr_ia32_feature_control = data;
            break;
        case MSR_IA32_BNDCFGS:
            env->msr_bndcfgs = data;
            break;
        case MSR_IA32_XSS:
            env->xss = data;
            break;
        case MSR_MTRRdefType:
            env->mtrr_deftype = data;
            break;
        case MSR_MTRRfix64K_00000:
            env->mtrr_fixed[0] = data;
            break;
        case MSR_MTRRfix16K_80000:
            env->mtrr_fixed[1] = data;
            break;
        case MSR_MTRRfix16K_A0000:
            env->mtrr_fixed[2] = data;
            break;
        case MSR_MTRRfix4K_C0000:
            env->mtrr_fixed[3] = data;
            break;
        case MSR_MTRRfix4K_C8000:
            env->mtrr_fixed[4] = data;
            break;
        case MSR_MTRRfix4K_D0000:
            env->mtrr_fixed[5] = data;
            break;
        case MSR_MTRRfix4K_D8000:
            env->mtrr_fixed[6] = data;
            break;
        case MSR_MTRRfix4K_E0000:
            env->mtrr_fixed[7] = data;
            break;
        case MSR_MTRRfix4K_E8000:
            env->mtrr_fixed[8] = data;
            break;
        case MSR_MTRRfix4K_F0000:
            env->mtrr_fixed[9] = data;
            break;
        case MSR_MTRRfix4K_F8000:
            env->mtrr_fixed[10] = data;
            break;
        case MSR_MTRRphysBase(0) ... MSR_MTRRphysMask(MSR_MTRRcap_VCNT - 1):
            if (index & 1) {
                env->mtrr_var[MSR_MTRRphysIndex(index)].mask =
                    data | mtrr_top_bits;
            } else {
                env->mtrr_var[MSR_MTRRphysIndex(index)].base = data;
            }
            break;
        }
    }
    return 0;
}

/* ----- vCPU events, mp_state, debug registers ----- */

static int aehd_put_mp_state(X86CPU *cpu)
{
    struct aehd_mp_state mp_state = { .mp_state = cpu->env.mp_state };

    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_MP_STATE, &mp_state,
                           sizeof(mp_state), NULL, 0);
}

static int aehd_get_mp_state(X86CPU *cpu)
{
    struct aehd_mp_state mp_state;
    int ret;

    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_MP_STATE, NULL, 0,
                          &mp_state, sizeof(mp_state));
    if (ret < 0) {
        return ret;
    }
    cpu->env.mp_state = mp_state.mp_state;
    return 0;
}

static int aehd_put_vcpu_events(X86CPU *cpu, int level)
{
    CPUX86State *env = &cpu->env;
    struct aehd_vcpu_events events = {};

    events.exception.injected = env->exception_injected ||
                                env->exception_pending;
    events.exception.nr = env->exception_nr;
    events.exception.has_error_code = env->has_error_code;
    events.exception.error_code = env->error_code;

    events.interrupt.injected = (env->interrupt_injected >= 0);
    events.interrupt.nr = env->interrupt_injected;
    events.interrupt.soft = env->soft_interrupt;

    events.nmi.injected = env->nmi_injected;
    events.nmi.pending = env->nmi_pending;
    events.nmi.masked = !!(env->hflags2 & HF2_NMI_MASK);

    events.sipi_vector = env->sipi_vector;

    events.flags = 0;
    if (level >= AEHD_PUT_RESET_STATE) {
        events.flags |= AEHD_VCPUEVENT_VALID_NMI_PENDING |
                        AEHD_VCPUEVENT_VALID_SIPI_VECTOR;
    }
    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_VCPU_EVENTS, &events,
                           sizeof(events), NULL, 0);
}

static int aehd_get_vcpu_events(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_vcpu_events events;
    int ret;

    memset(&events, 0, sizeof(events));
    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_VCPU_EVENTS, NULL, 0,
                          &events, sizeof(events));
    if (ret < 0) {
        return ret;
    }

    env->exception_pending = 0;
    env->exception_injected = events.exception.injected;
    env->exception_nr = events.exception.injected ? events.exception.nr : -1;
    env->has_error_code = events.exception.has_error_code;
    env->error_code = events.exception.error_code;

    env->interrupt_injected =
        events.interrupt.injected ? events.interrupt.nr : -1;
    env->soft_interrupt = events.interrupt.soft;

    env->nmi_injected = events.nmi.injected;
    env->nmi_pending = events.nmi.pending;
    if (events.nmi.masked) {
        env->hflags2 |= HF2_NMI_MASK;
    } else {
        env->hflags2 &= ~HF2_NMI_MASK;
    }

    env->sipi_vector = events.sipi_vector;
    return 0;
}

static int aehd_put_debugregs(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_debugregs dbgregs = {};

    for (int i = 0; i < 4; i++) {
        dbgregs.db[i] = env->dr[i];
    }
    dbgregs.dr6 = env->dr[6];
    dbgregs.dr7 = env->dr[7];
    return aehd_vcpu_ioctl(CPU(cpu), AEHD_SET_DEBUGREGS, &dbgregs,
                           sizeof(dbgregs), NULL, 0);
}

static int aehd_get_debugregs(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;
    struct aehd_debugregs dbgregs;
    int ret;

    ret = aehd_vcpu_ioctl(CPU(cpu), AEHD_GET_DEBUGREGS, NULL, 0,
                          &dbgregs, sizeof(dbgregs));
    if (ret < 0) {
        return ret;
    }
    for (int i = 0; i < 4; i++) {
        env->dr[i] = dbgregs.db[i];
    }
    env->dr[4] = env->dr[6] = dbgregs.dr6;
    env->dr[5] = env->dr[7] = dbgregs.dr7;
    return 0;
}

int aehd_arch_put_registers(CPUState *cs, int level)
{
    X86CPU *cpu = X86_CPU(cs);
    int ret;

    assert(cpu_is_stopped(cs) || qemu_cpu_is_self(cs));

    if (level >= AEHD_PUT_RESET_STATE) {
        ret = aehd_put_msr_feature_control(cpu);
        if (ret < 0) {
            return ret;
        }
    }
    ret = aehd_getput_regs(cpu, 1);
    if (ret < 0) {
        return ret;
    }
    ret = aehd_put_xsave(cpu);
    if (ret < 0) {
        return ret;
    }
    ret = aehd_put_xcrs(cpu);
    if (ret < 0) {
        return ret;
    }
    ret = aehd_put_sregs(cpu);
    if (ret < 0) {
        return ret;
    }
    ret = aehd_put_msrs(cpu, level);
    if (ret < 0) {
        return ret;
    }
    if (level >= AEHD_PUT_RESET_STATE) {
        ret = aehd_put_mp_state(cpu);
        if (ret < 0) {
            return ret;
        }
    }
    ret = aehd_put_tscdeadline_msr(cpu);
    if (ret < 0) {
        return ret;
    }
    ret = aehd_put_vcpu_events(cpu, level);
    if (ret < 0) {
        return ret;
    }
    return aehd_put_debugregs(cpu);
}

int aehd_arch_get_registers(CPUState *cs)
{
    X86CPU *cpu = X86_CPU(cs);
    int ret;

    assert(cpu_is_stopped(cs) || qemu_cpu_is_self(cs));

    ret = aehd_getput_regs(cpu, 0);
    if (ret >= 0) {
        ret = aehd_get_xsave(cpu);
    }
    if (ret >= 0) {
        ret = aehd_get_xcrs(cpu);
    }
    if (ret >= 0) {
        ret = aehd_get_sregs(cpu);
    }
    if (ret >= 0) {
        ret = aehd_get_msrs(cpu);
    }
    if (ret >= 0) {
        ret = aehd_get_mp_state(cpu);
    }
    if (ret >= 0) {
        ret = aehd_get_vcpu_events(cpu);
    }
    if (ret >= 0) {
        ret = aehd_get_debugregs(cpu);
    }
    cpu_sync_bndcs_hflags(&cpu->env);
    return ret < 0 ? ret : 0;
}

/* ===== run loop hooks (userspace irqchip) ===== */

void aehd_arch_pre_run(CPUState *cs, struct aehd_run *run)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;
    int ret;

    if (cs->interrupt_request & CPU_INTERRUPT_NMI) {
        bql_lock();
        cs->interrupt_request &= ~CPU_INTERRUPT_NMI;
        bql_unlock();
        ret = aehd_vcpu_ioctl(cs, AEHD_NMI, NULL, 0, NULL, 0);
        if (ret < 0) {
            error_report("AEHD: NMI injection failed: %s", strerror(-ret));
        }
    }

    bql_lock();

    /*
     * Leave the inner loop to process INIT requests and, for the userspace
     * APIC, pending TPR access reports.
     */
    if (cs->interrupt_request & (CPU_INTERRUPT_INIT | CPU_INTERRUPT_TPR)) {
        qatomic_set(&cs->exit_request, 1);
    }

    /* inject an interrupt if the guest can take it now */
    if (run->ready_for_interrupt_injection &&
        (cs->interrupt_request & CPU_INTERRUPT_HARD) &&
        (env->eflags & IF_MASK)) {
        int irq;

        cs->interrupt_request &= ~CPU_INTERRUPT_HARD;
        irq = cpu_get_pic_interrupt(env);
        if (irq >= 0) {
            struct aehd_interrupt intr = { .irq = irq };

            ret = aehd_vcpu_ioctl(cs, AEHD_INTERRUPT, &intr, sizeof(intr),
                                  NULL, 0);
            if (ret < 0) {
                error_report("AEHD: interrupt injection failed: %s",
                             strerror(-ret));
            }
        }
    }

    /* otherwise ask for an exit as soon as the guest can take one */
    run->request_interrupt_window =
        !!(cs->interrupt_request & CPU_INTERRUPT_HARD);
    run->cr8 = cpu_get_apic_tpr(cpu->apic_state);

    bql_unlock();
}

MemTxAttrs aehd_arch_post_run(CPUState *cs, struct aehd_run *run)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;

    if (run->if_flag) {
        env->eflags |= IF_MASK;
    } else {
        env->eflags &= ~IF_MASK;
    }

    /* the userspace APIC is shared with other threads */
    bql_lock();
    cpu_set_apic_tpr(cpu->apic_state, run->cr8);
    cpu_set_apic_base(cpu->apic_state, run->apic_base);
    bql_unlock();
    return cpu_get_mem_attrs(env);
}

/* called with the BQL held; returns whether the vCPU is halted */
int aehd_arch_process_async_events(CPUState *cs)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;

    if (cs->interrupt_request & CPU_INTERRUPT_MCE) {
        /* must not be raised if MCE is not supported */
        assert(env->mcg_cap);
        cs->interrupt_request &= ~CPU_INTERRUPT_MCE;
        aehd_cpu_synchronize_state(cs);
        if (env->exception_nr == EXCP08_DBLE) {
            /* triple fault */
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            qatomic_set(&cs->exit_request, 1);
            return 0;
        }
        env->exception_nr = EXCP12_MCHK;
        env->exception_injected = 1;
        env->has_error_code = 0;
        cs->halted = 0;
    }

    if (cs->interrupt_request & CPU_INTERRUPT_INIT) {
        aehd_cpu_synchronize_state(cs);
        do_cpu_init(cpu);
    }

    if (cs->interrupt_request & CPU_INTERRUPT_POLL) {
        cs->interrupt_request &= ~CPU_INTERRUPT_POLL;
        apic_poll_irq(cpu->apic_state);
    }
    if (((cs->interrupt_request & CPU_INTERRUPT_HARD) &&
         (env->eflags & IF_MASK)) ||
        (cs->interrupt_request & CPU_INTERRUPT_NMI)) {
        cs->halted = 0;
    }
    if (cs->interrupt_request & CPU_INTERRUPT_SIPI) {
        aehd_cpu_synchronize_state(cs);
        do_cpu_sipi(cpu);
    }
    if (cs->interrupt_request & CPU_INTERRUPT_TPR) {
        cs->interrupt_request &= ~CPU_INTERRUPT_TPR;
        aehd_cpu_synchronize_state(cs);
        apic_handle_tpr_access_report(cpu->apic_state, env->eip,
                                      env->tpr_access_type);
    }
    return cs->halted;
}

static int aehd_handle_halt(X86CPU *cpu)
{
    CPUState *cs = CPU(cpu);
    CPUX86State *env = &cpu->env;

    if (!((cs->interrupt_request & CPU_INTERRUPT_HARD) &&
          (env->eflags & IF_MASK)) &&
        !(cs->interrupt_request & CPU_INTERRUPT_NMI)) {
        cs->halted = 1;
        return EXCP_HLT;
    }
    return 0;
}

#define VMX_INVALID_GUEST_STATE 0x80000021

int aehd_arch_handle_exit(CPUState *cs, struct aehd_run *run)
{
    X86CPU *cpu = X86_CPU(cs);
    uint32_t ecx, unused;
    uint64_t code;
    int ret;

    switch (run->exit_reason) {
    case AEHD_EXIT_HLT:
        bql_lock();
        ret = aehd_handle_halt(cpu);
        bql_unlock();
        break;
    case AEHD_EXIT_SET_TPR:
        ret = 0;
        break;
    case AEHD_EXIT_TPR_ACCESS:
        bql_lock();
        apic_handle_tpr_access_report(cpu->apic_state, run->tpr_access.rip,
                                      run->tpr_access.is_write ?
                                      TPR_ACCESS_WRITE : TPR_ACCESS_READ);
        bql_unlock();
        ret = 1;
        break;
    case AEHD_EXIT_FAIL_ENTRY:
        code = run->fail_entry.hardware_entry_failure_reason;
        error_report("AEHD: entry failed, hardware error 0x%" PRIx64, code);
        host_cpuid(1, 0, &unused, &unused, &ecx, &unused);
        if ((ecx & CPUID_EXT_VMX) && code == VMX_INVALID_GUEST_STATE) {
            error_report("On Intel CPUs without unrestricted guest support "
                         "this usually means the guest entered big real "
                         "mode.");
        }
        ret = -1;
        break;
    case AEHD_EXIT_EXCEPTION:
        error_report("AEHD: exception %d exit (error code 0x%x)",
                     run->ex.exception, run->ex.error_code);
        ret = -1;
        break;
    default:
        error_report("AEHD: unknown exit reason %d", run->exit_reason);
        ret = -1;
        break;
    }
    return ret;
}

bool aehd_arch_stop_on_emulation_error(CPUState *cs)
{
    CPUX86State *env = &X86_CPU(cs)->env;

    aehd_cpu_synchronize_state(cs);
    return !(env->cr[0] & CR0_PE_MASK) ||
           ((env->segs[R_CS].selector & 3) != 3);
}
