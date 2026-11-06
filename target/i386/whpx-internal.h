/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) internals shared
 * between the WHPX accelerator and the WHPX APIC.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef TARGET_I386_WHPX_INTERNAL_H
#define TARGET_I386_WHPX_INTERNAL_H

#include "whp-dispatch.h"

struct whpx_state {
    uint64_t mem_quota;
    WHV_PARTITION_HANDLE partition;
    /* The local APICs are emulated by the hypervisor (whpx-apic). */
    bool apic_in_platform;
};

extern struct whpx_state whpx_global;

/* Read the hypervisor APIC state of the vCPU into the whpx-apic device. */
void whpx_apic_get(DeviceState *dev);

/*
 * The hypervisor's wait-for-SIPI (startup suspend) and halt state of a
 * vCPU, used with the hypervisor APIC.
 */
void whpx_set_activity_state(CPUState *cpu, bool startup_suspend,
                             bool halt_suspend);
bool whpx_get_activity_state(CPUState *cpu, bool *startup_suspend,
                             bool *halt_suspend);

/* Count a WHvRequestInterrupt call in the exit statistics. */
void whpx_stats_count_request_irq(void);

#endif /* TARGET_I386_WHPX_INTERNAL_H */
