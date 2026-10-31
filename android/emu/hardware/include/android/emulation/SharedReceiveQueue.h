// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "android/emulation/PipeSharedReceive.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace android {

inline uint32_t pipeRxLoad(const uint32_t* p) {
#ifdef _MSC_VER
    return static_cast<uint32_t>(_InterlockedCompareExchange(
            reinterpret_cast<volatile long*>(const_cast<uint32_t*>(p)), 0, 0));
#else
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#endif
}
inline void pipeRxStore(uint32_t* p, uint32_t value) {
#ifdef _MSC_VER
    _InterlockedExchange(reinterpret_cast<volatile long*>(p), static_cast<long>(value));
#else
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
#endif
}
inline void pipeRxFence() {
#ifdef _MSC_VER
    long barrier = 0;
    _InterlockedExchange(&barrier, 0);
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

// One serialized host producer and one guest consumer. The caller owns the
// mappings and must stop channel callbacks before releasing them.
class SharedReceiveQueue {
public:
    SharedReceiveQueue(PipeRxHost* host, PipeRxGuest* guest, PipeRxDirty dirty,
                       std::function<void()> wake, bool restoring)
        : mHost(host), mGuest(guest), mDirty(dirty), mWake(std::move(wake)),
          mRestoring(restoring) {
        if (!restoring) {
            std::memset(mHost, 0, sizeof(*mHost));
            mHost->write_segment = PIPE_RX_INVALID_SEGMENT;
            dirtyHost();
        }
    }

    bool add(unsigned id, void* const* pages) {
        std::lock_guard<std::mutex> lock(mLock);
        if (id != mCount || id >= PIPE_RX_MAX_SEGMENTS) return false;
        for (unsigned i = 0; i < PIPE_RX_SEGMENT_PAGES; ++i) {
            if (!pages[i]) return false;
        }
        std::copy(pages, pages + PIPE_RX_SEGMENT_PAGES, mPages[id].begin());
        ++mCount;
        if (!mRestoring) {
            pipeRxStore(&mHost->segment_count, mCount);
            dirtyHost();
        }
        return true;
    }

    bool activate() {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mCount || mHost->segment_count != mCount) return false;
        if (mRestoring) {
            if (mHost->write_segment >= mCount) return false;
            for (unsigned i = 0; i < mCount; ++i) {
                auto& s = mHost->segments[i];
                if ((s.committed & ~PIPE_RX_SEALED) > PIPE_RX_SEGMENT_SIZE ||
                    ((s.committed & PIPE_RX_SEALED) && s.next >= mCount)) return false;
            }
        } else {
            startSegment(0);
            mHost->write_segment = 0;
            dirtyHost();
        }
        mRestoring = false;
        return true;
    }

    size_t write(const void* source, size_t size) {
        bool wake = false;
        size_t written = 0;
        size = std::min<size_t>(size, PIPE_RX_SEGMENT_SIZE);
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (!mRunning || (mHost->flags & (PIPE_RX_ERROR | PIPE_RX_CLOSED))) return 0;
            while (written < size) {
                unsigned id = mHost->write_segment;
                if (id >= mCount) { fail(wake); break; }
                auto* segment = &mHost->segments[id];
                uint32_t offset = segment->committed;
                if (offset > PIPE_RX_SEGMENT_SIZE) { fail(wake); break; }
                if (offset == PIPE_RX_SEGMENT_SIZE) {
                    unsigned next = findFree(id);
                    if (next == PIPE_RX_INVALID_SEGMENT) {
                        bool changed = !(mHost->flags & PIPE_RX_NEED_SEGMENT);
                        pipeRxStore(&mHost->flags, mHost->flags | PIPE_RX_NEED_SEGMENT);
                        pipeRxFence();
                        next = findFree(id);
                        if (next == PIPE_RX_INVALID_SEGMENT) {
                            if (changed) publish(wake);
                            break;
                        }
                    }
                    startSegment(next);
                    segment->next = next;
                    pipeRxStore(&segment->committed, offset | PIPE_RX_SEALED);
                    mHost->write_segment = next;
                    pipeRxStore(&mHost->flags, mHost->flags & ~PIPE_RX_NEED_SEGMENT);
                    segment = &mHost->segments[next];
                    id = next;
                    offset = 0;
                }
                size_t count = std::min<size_t>(size - written, PIPE_RX_SEGMENT_SIZE - offset);
                size_t done = 0;
                while (done < count) {
                    size_t position = offset + done;
                    auto* target = static_cast<unsigned char*>(mPages[id][position / PIPE_RX_PAGE_SIZE]);
                    size_t chunk = std::min(count - done, PIPE_RX_PAGE_SIZE - position % PIPE_RX_PAGE_SIZE);
                    target += position % PIPE_RX_PAGE_SIZE;
                    std::memcpy(target, static_cast<const unsigned char*>(source) + written + done, chunk);
                    mDirty(target, chunk);
                    done += chunk;
                }
                pipeRxStore(&segment->committed, offset + static_cast<uint32_t>(count));
                written += count;
                publish(wake);
            }
        }
        if (wake) mWake();
        return written;
    }

    void setRunning(bool running) {
        std::lock_guard<std::mutex> lock(mLock);
        mRunning = running;
    }

    void arm(uint32_t sequence) {
        bool wake;
        {
            std::lock_guard<std::mutex> lock(mLock);
            wake = mHost->event_seq != sequence ||
                   (mHost->flags & (PIPE_RX_ERROR | PIPE_RX_CLOSED));
            mHost->irq_armed = !wake;
            dirtyHost();
        }
        if (wake) mWake();
    }

    void close() {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(mLock);
            pipeRxStore(&mHost->flags, mHost->flags | PIPE_RX_CLOSED);
            publish(wake);
        }
        if (wake) mWake();
    }

private:
    unsigned findFree(unsigned current) {
        for (unsigned n = 1; n <= mCount; ++n) {
            unsigned id = (current + n) % mCount;
            if (id == current) continue;
            uint32_t generation = mHost->segments[id].generation;
            if (!generation || pipeRxLoad(&mGuest->released[id]) == generation) return id;
        }
        return PIPE_RX_INVALID_SEGMENT;
    }
    void startSegment(unsigned id) {
        auto& s = mHost->segments[id];
        uint32_t generation = s.generation + 1;
        if (!generation) generation = 1;
        s.next = PIPE_RX_INVALID_SEGMENT;
        s.committed = 0;
        pipeRxStore(&s.generation, generation);
    }
    void publish(bool& wake) {
        pipeRxStore(&mHost->event_seq, mHost->event_seq + 1);
        if (mHost->irq_armed) { mHost->irq_armed = 0; wake = true; }
        dirtyHost();
    }
    void fail(bool& wake) {
        pipeRxStore(&mHost->flags, mHost->flags | PIPE_RX_ERROR);
        publish(wake);
    }
    void dirtyHost() { mDirty(mHost, sizeof(*mHost)); }
    PipeRxHost* mHost;
    PipeRxGuest* mGuest;
    PipeRxDirty mDirty;
    std::function<void()> mWake;
    std::mutex mLock;
    unsigned mCount = 0;
    bool mRestoring;
    bool mRunning = true;
    std::array<std::array<void*, PIPE_RX_SEGMENT_PAGES>, PIPE_RX_MAX_SEGMENTS> mPages{};
};
}
