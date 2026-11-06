/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) support
 *
 * Copyright Microsoft, Corp. 2017
 *
 * Authors:
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_WHPX_H
#define QEMU_WHPX_H

#include "config-host.h"
#include "qemu-common.h"

int whpx_init_vcpu(CPUState *cpu);
int whpx_vcpu_exec(CPUState *cpu);
void whpx_destroy_vcpu(CPUState *cpu);
void whpx_vcpu_kick(CPUState *cpu);

void whpx_cpu_synchronize_state(CPUState *cpu);
void whpx_cpu_synchronize_post_reset(CPUState *cpu);
void whpx_cpu_synchronize_post_init(CPUState *cpu);
void whpx_cpu_synchronize_pre_loadvm(CPUState *cpu);

void* whpx_gpa2hva(uint64_t gpa, bool* found);

#ifdef CONFIG_WHPX

int whpx_enabled(void);
bool whpx_apic_in_platform(void);

typedef struct WhpxDoorbell WhpxDoorbell;
/* The consumer owns event and must keep it alive until unregistering. */
WhpxDoorbell* whpx_doorbell_create(uint64_t gpa, uint32_t value, void* event);
void whpx_doorbell_destroy(WhpxDoorbell* doorbell);

#else /* CONFIG_WHPX */

#define whpx_enabled() (0)
#define whpx_apic_in_platform() (0)

#endif /* CONFIG_WHPX */

#endif /* QEMU_WHPX_H */
