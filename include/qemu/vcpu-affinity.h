/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_VCPU_AFFINITY_H
#define QEMU_VCPU_AFFINITY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Establish the process pool before host initialization, then set each vCPU's
 * ideal processor through its Windows thread ID. */
#ifdef _WIN32
void kemu_process_affinity_init(unsigned vcpu_count);
void kemu_vcpu_affinity_apply(unsigned vcpu, unsigned thread_id);
#else
static inline void kemu_process_affinity_init(unsigned vcpu_count)
{
}
static inline void kemu_vcpu_affinity_apply(unsigned vcpu, unsigned thread_id)
{
}
#endif

#ifdef __cplusplus
}
#endif
#endif
