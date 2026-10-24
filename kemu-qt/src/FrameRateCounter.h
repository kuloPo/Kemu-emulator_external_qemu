#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace kemu {
class FrameRateCounter final {
public:
    using Clock = std::chrono::steady_clock;

    explicit FrameRateCounter(Clock::time_point now = Clock::now())
        : mLastSample(now) {}

    // Called by the renderer; sampling stays on the UI thread.
    void recordFrame() { mFrames.fetch_add(1, std::memory_order_relaxed); }

    double sample(Clock::time_point now = Clock::now()) {
        const double seconds = std::chrono::duration<double>(now - mLastSample).count();
        if (seconds <= 0.0) return 0.0;
        mLastSample = now;
        return mFrames.exchange(0, std::memory_order_relaxed) / seconds;
    }

private:
    std::atomic<uint64_t> mFrames{0};
    Clock::time_point mLastSample;
};
}  // namespace kemu
