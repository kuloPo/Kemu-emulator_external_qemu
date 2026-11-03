#include "../../hw/audio/virtio-snd-timer.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace {
struct CaptureResult {
    int64_t delivered = 0;
    int64_t silence = 0;
    int64_t ahead = 0;
    int64_t bufferedAfterRecovery = 0;
};

CaptureResult simulateCapture(uint32_t period, int64_t chunk, int64_t phase,
                              uint32_t ringPeriods, bool reference,
                              int64_t stallDuration = 0) {
    const int64_t end = 2000000;
    const int64_t stallStart = 500000;
    const int64_t capacity = (ringPeriods + 1) * period;
    int64_t buffered = 0;
    int64_t nextChunk = phase;
    int64_t deadline = period;
    int64_t timer = deadline;
    CaptureResult result;
    while (timer < end) {
        const int64_t now = timer >= stallStart && timer < stallStart + stallDuration
            ? stallStart + stallDuration : timer;
        while (nextChunk <= now) {
            buffered = std::min(buffered + chunk, capacity);
            nextChunk += chunk;
        }
        if (!reference) {
            deadline = virtio_snd_capture_deadline_us(deadline, now, period, ringPeriods);
        }
        const uint32_t budget = reference ? 1 : virtio_snd_periods_due(
            deadline, now, period, ringPeriods);
        EXPECT_LE(budget, ringPeriods > 1 ? ringPeriods - 1 : 1);
        for (uint32_t completed = 0; completed < budget; ++completed) {
            const int64_t captured = std::min<int64_t>(buffered, period);
            buffered -= captured;
            result.silence += period - captured;
            result.delivered += period;
        }
        result.ahead = std::max(result.ahead, result.delivered - now);
        if (now > stallStart + stallDuration + 10 * period) {
            result.bufferedAfterRecovery = std::max(result.bufferedAfterRecovery, buffered);
        }
        deadline = reference ? now + period : deadline + budget * period;
        timer = reference ? deadline : virtio_snd_capture_timer_us(deadline, now, period);
        EXPECT_GT(timer, now);
        if (timer <= now) {
            break;
        }
    }
    return result;
}
}

TEST(VirtioSndTimer, DelayedDispatchCannotCompleteAnEntireGuestRing) {
    constexpr uint32_t period = 11333;
    const int64_t now = INT64_C(36000000000);
    for (uint32_t ringPeriods : {2, 4, 255}) {
        const int64_t deadline = now - INT64_C(30000000000);
        const auto completed = virtio_snd_periods_due(
            deadline, now, period, ringPeriods);
        EXPECT_GT(completed, 0u);
        EXPECT_LT(completed, ringPeriods);
        // QEMU dispatches again when the new deadline is <= now.
        EXPECT_GT(virtio_snd_next_batch_irq_us(deadline, now, period, completed), now);
    }
}

TEST(VirtioSndTimer, ModerateDelayCatchesUpWithoutMovingTheSchedule) {
    constexpr uint32_t period = 11333;
    constexpr int64_t deadline = 100000;
    const int64_t now = deadline + 15000;
    const auto completed = virtio_snd_periods_due(deadline, now, period, 4);
    EXPECT_EQ(2u, completed);
    EXPECT_EQ(deadline + 2 * period,
              virtio_snd_next_batch_irq_us(deadline, now, period, completed));
}

TEST(VirtioSndTimer, CatchUpIncludesDuePeriodsWithinTheRingBudget) {
    constexpr uint32_t period = 11333;
    constexpr int64_t deadline = 100000;
    for (uint32_t ringPeriods : {2, 4, 16}) {
        for (uint32_t due = 1; due < ringPeriods; ++due) {
            const int64_t now = deadline + (due - 1) * period;
            const auto completed = virtio_snd_periods_due(
                deadline, now, period, ringPeriods);
            EXPECT_EQ(due, completed);
            EXPECT_EQ(deadline + due * period,
                      virtio_snd_next_batch_irq_us(deadline, now, period, completed));
        }
    }
}

TEST(VirtioSndTimer, PartialBatchAccountsOnlyForCompletedPeriods) {
    constexpr uint32_t period = 11333;
    constexpr int64_t deadline = 100000;
    const int64_t now = deadline + 15000;
    EXPECT_EQ(now + period,
              virtio_snd_next_batch_irq_us(deadline, now, period, 1));
    EXPECT_EQ(now + period / 4,
              virtio_snd_next_batch_irq_us(deadline, now, period, 0));
}

TEST(VirtioSndTimer, NormalJitterDoesNotAccumulateDrift) {
    constexpr uint32_t period = 11333;
    int64_t deadline = 100000;
    for (int i = 0; i < 1000; ++i) {
        const int64_t expected = deadline + period;
        const auto completed = virtio_snd_periods_due(
            deadline, deadline + 500, period, 4);
        EXPECT_EQ(1u, completed);
        deadline = virtio_snd_next_batch_irq_us(
            deadline, deadline + 500, period, completed);
        EXPECT_EQ(expected, deadline);
    }
}

TEST(VirtioSndTimer, EmptyQueueRetriesRemainPacedAfterStall) {
    constexpr uint32_t period = 11333;
    constexpr uint32_t retry = period / 4;
    EXPECT_EQ(500000 + retry, virtio_snd_next_batch_irq_us(100000, 500000, period, 0));
    EXPECT_EQ(100000 + 2 * retry,
              virtio_snd_next_batch_irq_us(100000, 100000 + retry, period, 0));
    EXPECT_EQ(100000 + retry,
              virtio_snd_next_batch_irq_us(100000, 100000 + retry - 1, period, 0));
}

TEST(VirtioSndTimer, CaptureChunksDoNotAdvanceTheGuestClock) {
    for (uint32_t period : {7500, 11333}) {
        for (int64_t chunk : {10000, 23220}) {
            for (int64_t phase : {INT64_C(0), INT64_C(1234), chunk - 1}) {
                for (uint32_t ringPeriods : {2, 4}) {
                    SCOPED_TRACE(testing::Message() << period << ", " << chunk
                        << ", " << phase << ", " << ringPeriods);
                    const auto reference = simulateCapture(period, chunk, phase, ringPeriods, true);
                    const auto actual = simulateCapture(period, chunk, phase, ringPeriods, false);
                    EXPECT_EQ(reference.delivered, actual.delivered);
                    EXPECT_EQ(reference.silence, actual.silence);
                    EXPECT_EQ(0, actual.ahead);
                }
            }
        }
    }
}

TEST(VirtioSndTimer, CaptureStallsRecoverWithoutRunningAheadOrKeepingExtraLatency) {
    constexpr uint32_t period = 7500;
    for (int64_t chunk : {10000, 23220}) {
        for (int64_t stall : {15000, 50000, 500000}) {
            for (uint32_t ringPeriods : {2, 4}) {
                SCOPED_TRACE(testing::Message() << chunk << ", " << stall << ", " << ringPeriods);
                const auto result = simulateCapture(period, chunk, 1234, ringPeriods, false, stall);
                EXPECT_EQ(0, result.ahead);
                EXPECT_LT(result.bufferedAfterRecovery, chunk);
            }
        }
    }
}

TEST(VirtioSndTimer, CaptureKeepsUnservedElapsedPeriodsAcrossBatches) {
    constexpr uint32_t period = 7500;
    const int64_t deadline = virtio_snd_capture_deadline_us(100000, 150000, period, 4);
    EXPECT_EQ(115000, deadline);
    const auto first = virtio_snd_periods_due(deadline, 150000, period, 4);
    EXPECT_EQ(3u, first);
    const int64_t pending = deadline + first * period;
    EXPECT_EQ(137500, pending);
    const int64_t retry = virtio_snd_capture_timer_us(pending, 150000, period);
    EXPECT_GT(retry, 150000);
    const auto second = virtio_snd_periods_due(pending, retry, period, 4);
    EXPECT_EQ(2u, second);
    EXPECT_EQ(152500, virtio_snd_capture_timer_us(pending + second * period, retry, period));
}

TEST(VirtioSndTimer, CaptureDiscardsOnlyTimeOlderThanItsRetentionWindow) {
    constexpr uint32_t period = 7500;
    constexpr int64_t original = 100000;
    constexpr int64_t now = INT64_C(30000000000);
    for (uint32_t ringPeriods : {2, 4, 255}) {
        const int64_t deadline = virtio_snd_capture_deadline_us(original, now, period, ringPeriods);
        EXPECT_GT(deadline, original);
        EXPECT_LE(deadline, now);
        EXPECT_LT(now - deadline, (ringPeriods + 1) * period);
        EXPECT_EQ(0, (deadline - original) % period);
    }
}

TEST(VirtioSndTimer, CaptureEmptyGuestQueueDoesNotAdvanceTheNominalDeadline) {
    constexpr uint32_t period = 7500;
    constexpr int64_t deadline = 100000;
    const int64_t retry = virtio_snd_capture_timer_us(deadline, deadline, period);
    EXPECT_GT(retry, deadline);
    EXPECT_EQ(deadline, virtio_snd_capture_deadline_us(deadline, retry, period, 4));
    EXPECT_EQ(1u, virtio_snd_periods_due(deadline, retry, period, 4));
}

TEST(VirtioSndTimer, SmallConfigurationsStillMakeProgressAndYield) {
    EXPECT_EQ(1u, virtio_snd_periods_due(100, 500000, 1, 1));
    EXPECT_EQ(501, virtio_snd_next_batch_irq_us(100, 500, 1, 0));
    EXPECT_EQ(501, virtio_snd_capture_timer_us(100, 500, 1));
}
