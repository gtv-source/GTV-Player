/*
 * QEMU AEHD accelerator - hooks used by target/i386 code
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef AEHD_I386_H
#define AEHD_I386_H

void aehd_arch_reset_vcpu(X86CPU *cpu);
void aehd_arch_do_init_vcpu(X86CPU *cpu);
uint32_t aehd_arch_get_supported_cpuid(uint32_t function, uint32_t index,
                                       int reg);

#endif /* AEHD_I386_H */
