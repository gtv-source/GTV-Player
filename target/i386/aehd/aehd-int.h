/*
 * QEMU AEHD accelerator - internal definitions
 *
 * Ported from the Android Emulator's QEMU (external/qemu, emu-master-dev),
 * which is derived from QEMU's KVM support.
 *
 * Copyright IBM, Corp. 2008
 * Copyright (c) 2017 Intel Corporation (Haitao Shan <hshan@google.com>)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef AEHD_INT_H
#define AEHD_INT_H

#include "exec/memory.h"
#include "qom/object.h"
#include "sysemu/accel-ops.h"
#include "hw/core/cpu.h"
#include "aehd-interface.h"

#define TYPE_AEHD_ACCEL ACCEL_CLASS_NAME("aehd")

/* level of register state written back to the driver */
#define AEHD_PUT_RUNTIME_STATE   1
#define AEHD_PUT_RESET_STATE     2
#define AEHD_PUT_FULL_STATE      3

/* A 4096-byte buffer holds the 8-byte aehd_msrs header plus 255 entries */
#define AEHD_MSR_BUF_SIZE 4096

typedef struct AEHDSlot {
    hwaddr start_addr;
    ram_addr_t memory_size;
    void *ram;
    int slot;
    int flags;
} AEHDSlot;

/* per-vCPU state, reached through CPUState::accel */
struct AccelCPUState {
    HANDLE fd;                      /* vCPU handle from AEHD_CREATE_VCPU */
    struct aehd_run *run;           /* shared run page (AEHD_VCPU_MMAP) */
    struct aehd_xsave *xsave_buf;   /* scratch for AEHD_GET/SET_XSAVE */
    struct aehd_msrs *msr_buf;      /* scratch for AEHD_GET/SET_MSRS */
};

typedef struct AEHDState AEHDState;
extern AEHDState *aehd_state;

/* driver calls (DeviceIoControl); 0 on success, -errno on failure */
int aehd_ioctl(AEHDState *s, int type, void *input, size_t input_size,
               void *output, size_t output_size);
int aehd_vm_ioctl(AEHDState *s, int type, void *input, size_t input_size,
                  void *output, size_t output_size);
int aehd_vcpu_ioctl(CPUState *cpu, int type, void *input, size_t input_size,
                    void *output, size_t output_size);
int aehd_check_extension(AEHDState *s, unsigned int extension);

/* generic part (aehd-all.c) */
int aehd_init_vcpu(CPUState *cpu);
void aehd_destroy_vcpu(CPUState *cpu);
int aehd_cpu_exec(CPUState *cpu);
void aehd_kick_vcpu(CPUState *cpu);
void aehd_cpu_synchronize_state(CPUState *cpu);
void aehd_cpu_synchronize_post_reset(CPUState *cpu);
void aehd_cpu_synchronize_post_init(CPUState *cpu);
void aehd_cpu_synchronize_pre_loadvm(CPUState *cpu);

/* x86 part (aehd-cpu.c) */
int aehd_arch_init(MachineState *ms, AEHDState *s);
int aehd_arch_init_vcpu(CPUState *cpu);
unsigned long aehd_arch_vcpu_id(CPUState *cpu);
int aehd_arch_put_registers(CPUState *cpu, int level);
int aehd_arch_get_registers(CPUState *cpu);
void aehd_arch_pre_run(CPUState *cpu, struct aehd_run *run);
MemTxAttrs aehd_arch_post_run(CPUState *cpu, struct aehd_run *run);
int aehd_arch_handle_exit(CPUState *cpu, struct aehd_run *run);
int aehd_arch_process_async_events(CPUState *cpu);
bool aehd_arch_stop_on_emulation_error(CPUState *cpu);

#endif /* AEHD_INT_H */
