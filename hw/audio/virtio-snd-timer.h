/* Copyright (C) 2026 The Kemu Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_AUDIO_VIRTIO_SND_TIMER_H
#define HW_AUDIO_VIRTIO_SND_TIMER_H

#include <stdint.h>

static inline uint32_t virtio_snd_periods_due(int64_t deadline,
                                            int64_t now,
                                            uint32_t period,
                                            uint32_t ring_periods)
{
    const uint64_t due = now > deadline ? (now - deadline) / period + 1 : 1;
    const uint32_t limit = ring_periods > 1 ? ring_periods - 1 : 1;
    /* Leave progress visible in the guest's modulo hardware pointer. */
    return due > limit ? limit : due;
}

static inline int64_t virtio_snd_next_irq_us(int64_t deadline,
                                           int64_t now,
                                           uint32_t interval)
{
    const int64_t next = deadline + interval;
    /* Rearm in the future so the timer loop cannot repeat the batch. */
    return next > now ? next : now + interval;
}

static inline int64_t virtio_snd_next_batch_irq_us(int64_t deadline,
                                                 int64_t now,
                                                 uint32_t period,
                                                 uint32_t completed)
{
    if (!completed) {
        const uint32_t retry = period / 4;
        return virtio_snd_next_irq_us(deadline, now, retry ? retry : 1);
    }

    const int64_t last = deadline + (int64_t)(completed - 1) * period;
    return virtio_snd_next_irq_us(last, now, period);
}

static inline int64_t virtio_snd_capture_deadline_us(int64_t deadline,
                                                    int64_t now,
                                                    uint32_t period,
                                                    uint32_t ring_periods)
{
    const uint64_t due = now > deadline ? (now - deadline) / period + 1 : 1;
    const uint64_t retained = (uint64_t)ring_periods + 1;
    /* Host capture retains at most this many periods. Skip older elapsed
     * periods without changing the phase or replaying unbounded silence. */
    if (due > retained) {
        deadline += (int64_t)((due - retained) * period);
    }
    return deadline;
}

static inline int64_t virtio_snd_capture_timer_us(int64_t deadline,
                                                 int64_t now,
                                                 uint32_t period)
{
    const uint32_t retry = period / 4;
    /* An unfinished time-based batch yields before retrying. Host buffer
     * occupancy must never cause a period to be delivered before it is due. */
    return deadline > now ? deadline : now + (retry ? retry : 1);
}

#endif
