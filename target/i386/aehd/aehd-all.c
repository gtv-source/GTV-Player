/*
 * QEMU AEHD accelerator - generic part
 *
 * Ported from the Android Emulator's QEMU (external/qemu, emu-master-dev,
 * target/i386/aehd-all.c) to the QEMU 9.2 accelerator API.  Only the
 * userspace irqchip is supported: the in-kernel APIC/IOAPIC/PIC models, MSI
 * routing, guest debugging and the emulator's user-backed RAM API were left
 * out.
 *
 * Copyright IBM, Corp. 2008
 *           Red Hat, Inc. 2008
 * Copyright (c) 2017 Intel Corporation (Haitao Shan <hshan@google.com>)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/memalign.h"
#include "qemu/main-loop.h"
#include "qemu/accel.h"
#include "exec/address-spaces.h"
#include "exec/ram_addr.h"
#include "hw/boards.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/aehd.h"
#include "aehd-int.h"

struct AEHDState {
    AccelState parent_obj;

    HANDLE fd;          /* \\.\AEHD */
    HANDLE vmfd;        /* from AEHD_CREATE_VM */
    int nr_slots;
    AEHDSlot *slots;
    MemoryListener memory_listener;
};

OBJECT_DECLARE_SIMPLE_TYPE(AEHDState, AEHD_ACCEL)

AEHDState *aehd_state;
bool aehd_allowed;

/* ===== driver calls ===== */

static int aehd_ioctl_handle(HANDLE h, int type,
                             void *input, size_t input_size,
                             void *output, size_t output_size)
{
    DWORD bytes;

    if (DeviceIoControl(h, type, input, input_size, output, output_size,
                        &bytes, NULL)) {
        return 0;
    }
    switch (GetLastError()) {
    case ERROR_MORE_DATA:
        return -E2BIG;
    case ERROR_RETRY:
        return -EAGAIN;
    default:
        return -EFAULT;
    }
}

int aehd_ioctl(AEHDState *s, int type, void *input, size_t input_size,
               void *output, size_t output_size)
{
    return aehd_ioctl_handle(s->fd, type, input, input_size,
                             output, output_size);
}

int aehd_vm_ioctl(AEHDState *s, int type, void *input, size_t input_size,
                  void *output, size_t output_size)
{
    return aehd_ioctl_handle(s->vmfd, type, input, input_size,
                             output, output_size);
}

int aehd_vcpu_ioctl(CPUState *cpu, int type, void *input, size_t input_size,
                    void *output, size_t output_size)
{
    return aehd_ioctl_handle(cpu->accel->fd, type, input, input_size,
                             output, output_size);
}

int aehd_check_extension(AEHDState *s, unsigned int extension)
{
    int result = 0;

    if (aehd_ioctl(s, AEHD_CHECK_EXTENSION, &extension, sizeof(extension),
                   &result, sizeof(result))) {
        return 0;
    }
    return result;
}

/* ===== guest memory: one AEHD slot per RAM section ===== */

static AEHDSlot *aehd_get_free_slot(AEHDState *s)
{
    for (int i = 0; i < s->nr_slots; i++) {
        if (s->slots[i].memory_size == 0) {
            return &s->slots[i];
        }
    }
    return NULL;
}

static AEHDSlot *aehd_lookup_matching_slot(AEHDState *s, hwaddr start_addr,
                                           hwaddr size)
{
    for (int i = 0; i < s->nr_slots; i++) {
        AEHDSlot *mem = &s->slots[i];

        if (start_addr == mem->start_addr && size == mem->memory_size) {
            return mem;
        }
    }
    return NULL;
}

/*
 * The driver works in page size chunks, but sections may be sub-page sized or
 * unaligned: pad the start to the next page boundary and truncate the size.
 */
static hwaddr aehd_align_section(MemoryRegionSection *section, hwaddr *start)
{
    hwaddr size = int128_get64(section->size);
    hwaddr aligned = ROUND_UP(section->offset_within_address_space,
                              qemu_real_host_page_size());
    hwaddr delta = aligned - section->offset_within_address_space;

    *start = aligned;
    if (delta > size) {
        return 0;
    }
    return (size - delta) & qemu_real_host_page_mask();
}

static int aehd_set_user_memory_region(AEHDState *s, AEHDSlot *slot)
{
    struct aehd_userspace_memory_region mem = {
        .slot = slot->slot,
        .flags = slot->flags,
        .guest_phys_addr = slot->start_addr,
        .userspace_addr = (uint64_t)(uintptr_t)slot->ram,
    };

    if (slot->memory_size && (mem.flags & AEHD_MEM_READONLY)) {
        /* drop the old mapping first (KVM commit 75d61fbc) */
        mem.memory_size = 0;
        aehd_vm_ioctl(s, AEHD_SET_USER_MEMORY_REGION, &mem, sizeof(mem),
                      NULL, 0);
    }
    mem.memory_size = slot->memory_size;
    return aehd_vm_ioctl(s, AEHD_SET_USER_MEMORY_REGION, &mem, sizeof(mem),
                         NULL, 0);
}

static int aehd_mem_flags(MemoryRegion *mr)
{
    int flags = 0;

    if (memory_region_get_dirty_log_mask(mr) != 0) {
        flags |= AEHD_MEM_LOG_DIRTY_PAGES;
    }
    if (mr->readonly || memory_region_is_romd(mr)) {
        flags |= AEHD_MEM_READONLY;
    }
    return flags;
}

static void aehd_section_update_flags(AEHDState *s,
                                      MemoryRegionSection *section)
{
    hwaddr start_addr, size;
    AEHDSlot *mem;
    int old_flags;

    size = aehd_align_section(section, &start_addr);
    mem = size ? aehd_lookup_matching_slot(s, start_addr, size) : NULL;
    if (!mem) {
        return;     /* no slot: every access traps anyway */
    }
    old_flags = mem->flags;
    mem->flags = aehd_mem_flags(section->mr);
    if (mem->flags != old_flags && aehd_set_user_memory_region(s, mem)) {
        error_report("AEHD: failed to change dirty logging of a memory slot");
        abort();
    }
}

static void aehd_log_start(MemoryListener *listener,
                           MemoryRegionSection *section, int old, int new)
{
    if (old == 0) {
        aehd_section_update_flags(container_of(listener, AEHDState,
                                               memory_listener), section);
    }
}

static void aehd_log_stop(MemoryListener *listener,
                          MemoryRegionSection *section, int old, int new)
{
    if (new == 0) {
        aehd_section_update_flags(container_of(listener, AEHDState,
                                               memory_listener), section);
    }
}

/* fetch the driver's dirty bitmap of one section into QEMU's */
static void aehd_physical_sync_dirty_bitmap(AEHDState *s,
                                            MemoryRegionSection *section)
{
    struct aehd_dirty_log d = {};
    hwaddr start_addr, size, base, page = qemu_real_host_page_size();
    unsigned long *bitmap, npages;
    AEHDSlot *mem;

    size = aehd_align_section(section, &start_addr);
    mem = size ? aehd_lookup_matching_slot(s, start_addr, size) : NULL;
    if (!mem) {
        return;
    }

    /* the driver fills a bitmap of 64-bit words */
    size = ROUND_UP(mem->memory_size >> TARGET_PAGE_BITS, 64) / 8;
    bitmap = g_malloc0(size);
    d.dirty_bitmap = bitmap;
    d.slot = mem->slot;
    if (aehd_vm_ioctl(s, AEHD_GET_DIRTY_LOG, &d, sizeof(d), &d, sizeof(d))) {
        g_free(bitmap);
        error_report("AEHD: AEHD_GET_DIRTY_LOG failed");
        abort();
    }
    /*
     * cpu_physical_memory_set_dirty_lebitmap() is not built on Windows: mark
     * the dirty pages one by one (bit i = page i of the slot).
     */
    base = section->offset_within_region +
           (start_addr - section->offset_within_address_space);
    npages = mem->memory_size / page;
    for (unsigned long i = find_first_bit(bitmap, npages); i < npages;
         i = find_next_bit(bitmap, npages, i + 1)) {
        memory_region_set_dirty(section->mr, base + i * page, page);
    }
    g_free(bitmap);
}

static void aehd_set_phys_mem(AEHDState *s, MemoryRegionSection *section,
                              bool add)
{
    MemoryRegion *mr = section->mr;
    bool writeable = !mr->readonly && !mr->rom_device;
    hwaddr start_addr, size;
    AEHDSlot *mem;

    if (!memory_region_is_ram(mr)) {
        if (writeable) {
            return;
        } else if (!mr->romd_mode) {
            /* ROM device not in romd mode: remove the slot so accesses trap */
            add = false;
        }
    }

    size = aehd_align_section(section, &start_addr);
    if (!size) {
        return;
    }

    if (!add) {
        mem = aehd_lookup_matching_slot(s, start_addr, size);
        if (!mem) {
            return;
        }
        if (mem->flags & AEHD_MEM_LOG_DIRTY_PAGES) {
            aehd_physical_sync_dirty_bitmap(s, section);
        }
        mem->memory_size = 0;
        if (aehd_set_user_memory_region(s, mem)) {
            error_report("AEHD: failed to unregister a memory slot");
            abort();
        }
        return;
    }

    mem = aehd_get_free_slot(s);
    if (!mem) {
        error_report("AEHD: no free memory slot (%d in use)", s->nr_slots);
        abort();
    }
    mem->memory_size = size;
    mem->start_addr = start_addr;
    /* aligned delta also applies to the host address */
    mem->ram = memory_region_get_ram_ptr(mr) + section->offset_within_region +
               (start_addr - section->offset_within_address_space);
    mem->flags = aehd_mem_flags(mr);
    if (aehd_set_user_memory_region(s, mem)) {
        error_report("AEHD: failed to register a memory slot at 0x%" HWADDR_PRIx
                     " size 0x%" HWADDR_PRIx, start_addr, size);
        abort();
    }
}

static void aehd_region_add(MemoryListener *listener,
                            MemoryRegionSection *section)
{
    memory_region_ref(section->mr);
    aehd_set_phys_mem(container_of(listener, AEHDState, memory_listener),
                      section, true);
}

static void aehd_region_del(MemoryListener *listener,
                            MemoryRegionSection *section)
{
    aehd_set_phys_mem(container_of(listener, AEHDState, memory_listener),
                      section, false);
    memory_region_unref(section->mr);
}

static void aehd_log_sync(MemoryListener *listener,
                          MemoryRegionSection *section)
{
    aehd_physical_sync_dirty_bitmap(container_of(listener, AEHDState,
                                                 memory_listener), section);
}

static void aehd_memory_listener_register(AEHDState *s)
{
    s->slots = g_new0(AEHDSlot, s->nr_slots);
    for (int i = 0; i < s->nr_slots; i++) {
        s->slots[i].slot = i;
    }
    s->memory_listener = (MemoryListener) {
        .name = "aehd",
        .region_add = aehd_region_add,
        .region_del = aehd_region_del,
        .log_start = aehd_log_start,
        .log_stop = aehd_log_stop,
        .log_sync = aehd_log_sync,
        .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
    };
    memory_listener_register(&s->memory_listener, &address_space_memory);
}

/* ===== vCPUs ===== */

int aehd_init_vcpu(CPUState *cpu)
{
    AEHDState *s = aehd_state;
    unsigned long vcpu_id = aehd_arch_vcpu_id(cpu);
    AccelCPUState *vcpu = g_new0(AccelCPUState, 1);
    HANDLE fd = INVALID_HANDLE_VALUE;
    int ret;

    ret = aehd_vm_ioctl(s, AEHD_CREATE_VCPU, &vcpu_id, sizeof(vcpu_id),
                        &fd, sizeof(fd));
    if (ret || fd == INVALID_HANDLE_VALUE) {
        error_report("AEHD: AEHD_CREATE_VCPU failed for vCPU %lu", vcpu_id);
        g_free(vcpu);
        return ret ? ret : -EFAULT;
    }
    vcpu->fd = fd;
    cpu->accel = vcpu;
    cpu->vcpu_dirty = true;

    ret = aehd_vcpu_ioctl(cpu, AEHD_VCPU_MMAP, NULL, 0,
                          &vcpu->run, sizeof(vcpu->run));
    if (ret) {
        error_report("AEHD: mapping the vCPU run page failed");
        return ret;
    }
    return aehd_arch_init_vcpu(cpu);
}

void aehd_destroy_vcpu(CPUState *cpu)
{
    AccelCPUState *vcpu = cpu->accel;

    if (!vcpu) {
        return;
    }
    CloseHandle(vcpu->fd);
    qemu_vfree(vcpu->xsave_buf);
    g_free(vcpu->msr_buf);
    g_free(vcpu);
    cpu->accel = NULL;
}

/*
 * Make the vCPU leave AEHD_RUN (or not enter it): the driver checks
 * user_event_pending before entering the guest, AEHD_KICK_VCPU forces an exit
 * if it is already running.
 */
void aehd_kick_vcpu(CPUState *cpu)
{
    AccelCPUState *vcpu = cpu->accel;
    unsigned long vcpu_id;

    if (!vcpu || !vcpu->run) {
        return;
    }
    vcpu->run->user_event_pending = 1;
    vcpu_id = aehd_arch_vcpu_id(cpu);
    aehd_vm_ioctl(aehd_state, AEHD_KICK_VCPU, &vcpu_id, sizeof(vcpu_id),
                  NULL, 0);
}

static void do_aehd_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    if (!cpu->vcpu_dirty) {
        aehd_arch_get_registers(cpu);
        cpu->vcpu_dirty = true;
    }
}

void aehd_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->vcpu_dirty) {
        run_on_cpu(cpu, do_aehd_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

static void do_aehd_cpu_synchronize_post_reset(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    aehd_arch_put_registers(cpu, AEHD_PUT_RESET_STATE);
    cpu->vcpu_dirty = false;
}

void aehd_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_aehd_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

static void do_aehd_cpu_synchronize_post_init(CPUState *cpu,
                                              run_on_cpu_data arg)
{
    aehd_arch_put_registers(cpu, AEHD_PUT_FULL_STATE);
    cpu->vcpu_dirty = false;
}

void aehd_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_aehd_cpu_synchronize_post_init, RUN_ON_CPU_NULL);
}

static void do_aehd_cpu_synchronize_pre_loadvm(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    cpu->vcpu_dirty = true;
}

void aehd_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_aehd_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}

static void aehd_handle_io(uint16_t port, MemTxAttrs attrs, void *data,
                           int direction, int size, uint32_t count)
{
    uint8_t *ptr = data;

    for (uint32_t i = 0; i < count; i++) {
        address_space_rw(&address_space_io, port, attrs, ptr, size,
                         direction == AEHD_EXIT_IO_OUT);
        ptr += size;
    }
}

static int aehd_handle_internal_error(CPUState *cpu, struct aehd_run *run)
{
    error_report("AEHD internal error, suberror %d", run->internal.suberror);
    for (int i = 0; i < run->internal.ndata; ++i) {
        error_report("extra data[%d]: 0x%016" PRIx64,
                     i, (uint64_t)run->internal.data[i]);
    }
    if (run->internal.suberror == AEHD_INTERNAL_ERROR_EMULATION) {
        error_report("emulation failure");
        if (!aehd_arch_stop_on_emulation_error(cpu)) {
            cpu_dump_state(cpu, stderr, CPU_DUMP_CODE);
            return EXCP_INTERRUPT;
        }
    }
    return -1;
}

/* called with the BQL held, returns with it held */
int aehd_cpu_exec(CPUState *cpu)
{
    struct aehd_run *run = cpu->accel->run;
    int ret, run_ret;

    if (aehd_arch_process_async_events(cpu)) {
        qatomic_set(&cpu->exit_request, 0);
        return EXCP_HLT;
    }

    bql_unlock();

    do {
        MemTxAttrs attrs;

        if (cpu->vcpu_dirty) {
            aehd_arch_put_registers(cpu, AEHD_PUT_RUNTIME_STATE);
            cpu->vcpu_dirty = false;
        }

        aehd_arch_pre_run(cpu, run);
        if (qatomic_read(&cpu->exit_request)) {
            /*
             * The driver needs to re-enter after I/O exits to finish the
             * instruction; with an event pending it comes back right away.
             */
            run->user_event_pending = 1;
        }

        run_ret = aehd_vcpu_ioctl(cpu, AEHD_RUN, NULL, 0, NULL, 0);
        run->user_event_pending = 0;

        attrs = aehd_arch_post_run(cpu, run);

        if (run_ret < 0) {
            if (run_ret == -EINTR || run_ret == -EAGAIN) {
                ret = EXCP_INTERRUPT;
                break;
            }
            error_report("AEHD: AEHD_RUN failed: %s", strerror(-run_ret));
            ret = -1;
            break;
        }

        switch (run->exit_reason) {
        case AEHD_EXIT_IO:
            /* outside the BQL; devices that need it take it themselves */
            aehd_handle_io(run->io.port, attrs,
                           (uint8_t *)run + run->io.data_offset,
                           run->io.direction, run->io.size, run->io.count);
            ret = 0;
            break;
        case AEHD_EXIT_MMIO:
            address_space_rw(&address_space_memory, run->mmio.phys_addr, attrs,
                             run->mmio.data, run->mmio.len,
                             run->mmio.is_write);
            ret = 0;
            break;
        case AEHD_EXIT_IRQ_WINDOW_OPEN:
        case AEHD_EXIT_INTR:
            ret = EXCP_INTERRUPT;
            break;
        case AEHD_EXIT_SHUTDOWN:
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            ret = EXCP_INTERRUPT;
            break;
        case AEHD_EXIT_UNKNOWN:
            error_report("AEHD: unknown exit, hardware reason 0x%" PRIx64,
                         (uint64_t)run->hw.hardware_exit_reason);
            ret = -1;
            break;
        case AEHD_EXIT_INTERNAL_ERROR:
            ret = aehd_handle_internal_error(cpu, run);
            break;
        case AEHD_EXIT_SYSTEM_EVENT:
            switch (run->system_event.type) {
            case AEHD_SYSTEM_EVENT_SHUTDOWN:
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
                ret = EXCP_INTERRUPT;
                break;
            case AEHD_SYSTEM_EVENT_RESET:
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
                ret = EXCP_INTERRUPT;
                break;
            case AEHD_SYSTEM_EVENT_CRASH:
                aehd_cpu_synchronize_state(cpu);
                bql_lock();
                qemu_system_guest_panicked(cpu_get_crash_info(cpu));
                bql_unlock();
                ret = 0;
                break;
            default:
                ret = aehd_arch_handle_exit(cpu, run);
                break;
            }
            break;
        default:
            ret = aehd_arch_handle_exit(cpu, run);
            break;
        }
    } while (ret == 0);

    bql_lock();

    if (ret < 0) {
        cpu_dump_state(cpu, stderr, CPU_DUMP_CODE);
        vm_stop(RUN_STATE_INTERNAL_ERROR);
    }

    qatomic_set(&cpu->exit_request, 0);
    return ret;
}

/* ===== accelerator ===== */

static HANDLE aehd_open_device(void)
{
    HANDLE h = CreateFile("\\\\.\\AEHD", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        /* AEHD 2.0 and older only have the old name */
        h = CreateFile("\\\\.\\gvm", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return h;
}

static int aehd_max_vcpus(AEHDState *s)
{
    int ret = aehd_check_extension(s, AEHD_CAP_MAX_VCPUS);

    if (!ret) {
        ret = aehd_check_extension(s, AEHD_CAP_NR_VCPUS);
    }
    return ret ? ret : 4;
}

static int aehd_init(MachineState *ms)
{
    AEHDState *s = AEHD_ACCEL(ms->accelerator);
    int type = 0;
    int max_vcpus;
    int ret;

    s->vmfd = INVALID_HANDLE_VALUE;
    s->fd = aehd_open_device();
    if (s->fd == INVALID_HANDLE_VALUE) {
        error_report("AEHD: cannot open the AEHD driver (error 0x%lx); is the "
                     "aehd service installed and running? (sc query aehd)",
                     GetLastError());
        return -ENODEV;
    }

    s->nr_slots = aehd_check_extension(s, AEHD_CAP_NR_MEMSLOTS);
    if (!s->nr_slots) {
        s->nr_slots = 32;
    }

    max_vcpus = aehd_max_vcpus(s);
    if (ms->smp.max_cpus > max_vcpus) {
        error_report("AEHD: %u vCPUs requested, the driver supports %d",
                     ms->smp.max_cpus, max_vcpus);
        ret = -EINVAL;
        goto err;
    }

    do {
        ret = aehd_ioctl(s, AEHD_CREATE_VM, &type, sizeof(type),
                         &s->vmfd, sizeof(s->vmfd));
    } while (ret == -EINTR);
    if (ret < 0) {
        error_report("AEHD: AEHD_CREATE_VM failed: %s", strerror(-ret));
        goto err;
    }

    ret = aehd_arch_init(ms, s);
    if (ret < 0) {
        goto err;
    }

    aehd_state = s;
    aehd_memory_listener_register(s);

    printf("AEHD accelerator is operational\n");
    return 0;

err:
    if (s->vmfd != INVALID_HANDLE_VALUE) {
        CloseHandle(s->vmfd);
    }
    CloseHandle(s->fd);
    return ret;
}

static void aehd_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);

    ac->name = "AEHD";
    ac->init_machine = aehd_init;
    ac->allowed = &aehd_allowed;
}

static const TypeInfo aehd_accel_type = {
    .name = TYPE_AEHD_ACCEL,
    .parent = TYPE_ACCEL,
    .instance_size = sizeof(AEHDState),
    .class_init = aehd_accel_class_init,
};

static void aehd_type_init(void)
{
    type_register_static(&aehd_accel_type);
}

type_init(aehd_type_init);
