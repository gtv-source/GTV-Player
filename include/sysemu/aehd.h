/*
 * QEMU Android Emulator Hypervisor Driver (AEHD) accelerator
 *
 * AEHD is Google's KVM-derived hypervisor driver for Windows hosts that do not
 * run Hyper-V.  This port supports the userspace irqchip only
 * (-machine kernel-irqchip=off), i.e. APIC/IOAPIC/PIC are emulated by QEMU.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

/* header to be included in non-AEHD-specific code */

#ifndef QEMU_AEHD_H
#define QEMU_AEHD_H

#ifdef COMPILING_PER_TARGET

#ifdef CONFIG_AEHD

extern bool aehd_allowed;
#define aehd_enabled() (aehd_allowed)

#else /* CONFIG_AEHD */

#define aehd_enabled() (0)

#endif /* CONFIG_AEHD */

#endif /* COMPILING_PER_TARGET */

#endif /* QEMU_AEHD_H */
