#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace kemu {
class FrameRateCounter final {
public:
    using Clock = std::chrono::steady_clock;

    explicit FrameRateCounter(Clock::time_point now = Clock::now(), int maxFrameRate = 0)
        : mLastSample(now), mMaxFrameRate(maxFrameRate) {}

    void recordFrame(Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mMutex);
        now = std::max(now, mLastFrame);
        if (mFrames == 0) mFirstFrame = now;
        mLastFrame = now;
        ++mFrames;
    }

    double sample(Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mMutex);
        const double sampleSeconds = std::chrono::duration<double>(now - mLastSample).count();
        if (sampleSeconds <= 0.0) return 0.0;
        mLastSample = now;
        if (mFrames == 0) {
            mPreviousFrame.reset();
            return 0.0;
        }

        const auto first = mPreviousFrame.value_or(mFirstFrame);
        const auto intervals = mFrames - (mPreviousFrame ? 0 : 1);
        const double frameSeconds = std::chrono::duration<double>(mLastFrame - first).count();
        double rate = mFrames / sampleSeconds;
        if (intervals > 0 && frameSeconds > 0.0) {
            // Measure between frames so timer phase does not bias steady FPS.
            // Include stalls longer than one measured frame period in the rate.
            const double period = frameSeconds / intervals;
            const double elapsed = std::chrono::duration<double>(now - first).count();
            rate = intervals / std::max(frameSeconds, elapsed - period);
        }
        mPreviousFrame = mLastFrame;
        mFrames = 0;
        // Keep the displayed rate within the configured guest refresh limit.
        return mMaxFrameRate > 0 ? std::min(rate, double(mMaxFrameRate)) : rate;
    }

private:
    std::mutex mMutex;
    uint64_t mFrames = 0;
    Clock::time_point mFirstFrame{};
    Clock::time_point mLastFrame{};
    std::optional<Clock::time_point> mPreviousFrame;
    Clock::time_point mLastSample;
    int mMaxFrameRate;
};
}  // namespace kemu
