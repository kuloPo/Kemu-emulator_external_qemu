// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "android/emulation/PipeSharedTransmit.h"
#include "android/emulation/SharedReceiveQueue.h"
#include <vector>

namespace android {

// A serialized guest producer reserves contiguous virtual ranges. The host
// reads the registered physical pages directly into its decoder input buffer.
class SharedTransmitQueue {
public:
    SharedTransmitQueue(PipeTxGuest* guest, PipeTxHost* host, PipeRxDirty dirty,
                        std::function<void()> wake, bool restoring)
        : mGuest(guest), mHost(host), mDirty(dirty), mWake(std::move(wake)),
          mRestoring(restoring) {
        if (!restoring) {
            std::memset(host, 0, sizeof(*host));
            host->read_generation = 1;
            dirtyHost();
        }
    }

    bool add(unsigned id, void* const* pages, unsigned count) {
        std::lock_guard<std::mutex> lock(mLock);
        if (id != mCount || id >= PIPE_TX_MAX_SEGMENTS || !count ||
            count > PIPE_TX_MAX_PAGES) return false;
        for (unsigned i = 0; i < count; ++i) if (!pages[i]) return false;
        const uint32_t capacity = count * PIPE_TX_PAGE_SIZE;
        if (mRestoring && mHost->capacities[id] != capacity) return false;
        mPages[id].assign(pages, pages + count);
        ++mCount;
        if (!mRestoring) {
            mHost->capacities[id] = capacity;
            pipeRxStore(&mHost->segment_count, mCount);
            dirtyHost();
        }
        return true;
    }

    bool activate() {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mCount || mHost->segment_count != mCount ||
            mHost->read_segment >= mCount || !mHost->read_generation ||
            mHost->read_offset > mPages[mHost->read_segment].size() * PIPE_TX_PAGE_SIZE)
            return false;
        mRestoring = false;
        mActive = true;
        return true;
    }

    // SIZE_MAX means a terminal error; zero means no published bytes yet.
    size_t read(void* destination, size_t size) {
        bool wake = false;
        size_t copied = 0;
        size = std::min<size_t>(size, PIPE_TX_SEGMENT_SIZE);
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (!mActive || !mRunning) return 0;
            if (mHost->flags & (PIPE_TX_ERROR | PIPE_TX_CLOSED)) return SIZE_MAX;
            unsigned transitions = 0;
            for (;;) {
                const unsigned id = mHost->read_segment;
                auto& segment = mGuest->segments[id];
                const uint32_t committed = pipeRxLoad(&segment.committed);
                const uint32_t available = committed & ~PIPE_TX_SEALED;
                const uint32_t offset = mHost->read_offset;
                if (pipeRxLoad(&segment.generation) != mHost->read_generation ||
                    available > mPages[id].size() * PIPE_TX_PAGE_SIZE || available < offset) {
                    fail(wake);
                    break;
                }
                if (available > offset) {
                    const size_t count = std::min<size_t>(size - copied, available - offset);
                    size_t done = 0;
                    while (done < count) {
                        const size_t position = offset + done;
                        const size_t chunk = std::min(count - done,
                                PIPE_TX_PAGE_SIZE - position % PIPE_TX_PAGE_SIZE);
                        const auto* source = static_cast<const unsigned char*>(
                                mPages[id][position / PIPE_TX_PAGE_SIZE]);
                        std::memcpy(static_cast<unsigned char*>(destination) + copied + done,
                                    source + position % PIPE_TX_PAGE_SIZE, chunk);
                        done += chunk;
                    }
                    copied += count;
                    mHost->read_offset += static_cast<uint32_t>(count);
                }
                if (mHost->read_offset == available && (committed & PIPE_TX_SEALED)) {
                    const unsigned next = pipeRxLoad(&segment.next);
                    if (next >= mCount || next == id) { fail(wake); break; }
                    const uint32_t generation = pipeRxLoad(&mGuest->segments[next].generation);
                    if (!generation || generation == mHost->released[next]) {
                        fail(wake);
                        break;
                    }
                    const uint32_t released = mHost->read_generation;
                    mHost->read_segment = next;
                    mHost->read_generation = generation;
                    mHost->read_offset = 0;
                    pipeRxStore(&mHost->released[id], released);
                    publish(wake);
                    if (++transitions < PIPE_TX_MAX_SEGMENTS) continue;
                }
                break;
            }
            if (copied) dirtyHost();
            if (mHost->flags & PIPE_TX_ERROR) copied = SIZE_MAX;
        }
        if (wake) mWake();
        return copied;
    }

    void arm(uint32_t sequence) {
        bool wake;
        {
            std::lock_guard<std::mutex> lock(mLock);
            wake = mHost->event_seq != sequence ||
                   (mHost->flags & (PIPE_TX_ERROR | PIPE_TX_CLOSED));
            mHost->irq_armed = !wake;
            dirtyHost();
        }
        if (wake) mWake();
    }

    void setRunning(bool running) {
        std::lock_guard<std::mutex> lock(mLock);
        mRunning = running;
    }

    void close() {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(mLock);
            pipeRxStore(&mHost->flags, mHost->flags | PIPE_TX_CLOSED);
            publish(wake);
        }
        if (wake) mWake();
    }

private:
    void publish(bool& wake) {
        pipeRxStore(&mHost->event_seq, mHost->event_seq + 1);
        if (mHost->irq_armed) { mHost->irq_armed = 0; wake = true; }
        dirtyHost();
    }
    void fail(bool& wake) {
        pipeRxStore(&mHost->flags, mHost->flags | PIPE_TX_ERROR);
        publish(wake);
    }
    void dirtyHost() { mDirty(mHost, sizeof(*mHost)); }
    PipeTxGuest* mGuest;
    PipeTxHost* mHost;
    PipeRxDirty mDirty;
    std::function<void()> mWake;
    std::mutex mLock;
    std::array<std::vector<void*>, PIPE_TX_MAX_SEGMENTS> mPages;
    unsigned mCount = 0;
    bool mRestoring;
    bool mActive = false;
    bool mRunning = true;
};
}
