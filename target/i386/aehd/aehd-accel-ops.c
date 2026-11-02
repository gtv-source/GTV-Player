/*
 * QEMU AEHD accelerator - vCPU threads
 *
 * Same structure as target/i386/whpx/whpx-accel-ops.c.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "sysemu/cpus.h"
#include "aehd-int.h"

static void *aehd_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;

    r = aehd_init_vcpu(cpu);
    if (r < 0) {
        error_report("AEHD: initialising vCPU %d failed: %s",
                     cpu->cpu_index, strerror(-r));
        exit(1);
    }

    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        if (cpu_can_run(cpu)) {
            r = aehd_cpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
        while (cpu_thread_is_idle(cpu)) {
            qemu_cond_wait_bql(cpu->halt_cond);
        }
        qemu_wait_io_event_common(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    aehd_destroy_vcpu(cpu);
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void aehd_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/AEHD",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, aehd_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void aehd_kick_vcpu_thread(CPUState *cpu)
{
    if (!qemu_cpu_is_self(cpu)) {
        aehd_kick_vcpu(cpu);
    }
}

static bool aehd_vcpu_thread_is_idle(CPUState *cpu)
{
    /* userspace APIC: a halted vCPU really waits in QEMU */
    return true;
}

static void aehd_accel_ops_class_init(ObjectClass *oc, void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = aehd_start_vcpu_thread;
    ops->kick_vcpu_thread = aehd_kick_vcpu_thread;
    ops->cpu_thread_is_idle = aehd_vcpu_thread_is_idle;

    ops->synchronize_post_reset = aehd_cpu_synchronize_post_reset;
    ops->synchronize_post_init = aehd_cpu_synchronize_post_init;
    ops->synchronize_state = aehd_cpu_synchronize_state;
    ops->synchronize_pre_loadvm = aehd_cpu_synchronize_pre_loadvm;
}

static const TypeInfo aehd_accel_ops_type = {
    .name = ACCEL_OPS_NAME("aehd"),
    .parent = TYPE_ACCEL_OPS,
    .class_init = aehd_accel_ops_class_init,
    .abstract = true,
};

static void aehd_accel_ops_register_types(void)
{
    type_register_static(&aehd_accel_ops_type);
}
type_init(aehd_accel_ops_register_types);
