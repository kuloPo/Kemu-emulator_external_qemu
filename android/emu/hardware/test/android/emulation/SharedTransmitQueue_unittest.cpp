// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#include "android/emulation/SharedTransmitQueue.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace android {
namespace {
class SharedTransmitTest : public ::testing::Test {
protected:
    PipeTxGuest guest{};
    PipeTxHost host{};
    std::vector<std::vector<unsigned char>> storage;
    std::unique_ptr<SharedTransmitQueue> queue;
    unsigned wakes = 0;

    size_t readAvailable(void* target, size_t size) {
        size_t total = 0;
        while (total < size) {
            size_t n = queue->read(static_cast<char*>(target) + total, size - total);
            if (n == SIZE_MAX) return n;
            if (!n) break;
            total += n;
        }
        return total;
    }
    void SetUp() override {
        storage.reserve(PIPE_TX_MAX_SEGMENTS);
        queue = make(false);
        add(PIPE_TX_SEGMENT_SIZE);
        add(PIPE_TX_SEGMENT_SIZE);
        guest.segments[0].generation = 1;
        ASSERT_TRUE(queue->activate());
    }
    std::unique_ptr<SharedTransmitQueue> make(bool restoring) {
        return std::make_unique<SharedTransmitQueue>(&guest, &host,
            [](void*, size_t) {}, [this] { ++wakes; }, restoring);
    }
    void mapping(unsigned id) {
        std::vector<void*> pages;
        for (size_t offset = 0; offset < storage[id].size(); offset += PIPE_TX_PAGE_SIZE)
            pages.push_back(storage[id].data() + offset);
        ASSERT_TRUE(queue->add(id, pages.data(), pages.size()));
    }
    void add(size_t size) {
        storage.emplace_back(size);
        mapping(storage.size() - 1);
    }
    void publish(unsigned id, uint32_t generation, size_t size, unsigned char byte) {
        std::fill(storage[id].begin(), storage[id].begin() + size, byte);
        pipeRxStore(&guest.segments[id].generation, generation);
        pipeRxStore(&guest.segments[id].committed, size);
    }
    void link(unsigned from, unsigned to) {
        guest.segments[from].next = to;
        pipeRxStore(&guest.segments[from].committed,
                    guest.segments[from].committed | PIPE_TX_SEALED);
    }
};

TEST_F(SharedTransmitTest, CopyBatchIsBounded) {
    add(4 * PIPE_TX_SEGMENT_SIZE);
    publish(2, 1, storage[2].size(), 0x6d);
    link(0, 2);
    std::vector<unsigned char> output(storage[2].size(), 0xcc);
    EXPECT_EQ(PIPE_TX_SEGMENT_SIZE, queue->read(output.data(), output.size()));
    EXPECT_EQ(PIPE_TX_SEGMENT_SIZE, host.read_offset);
    EXPECT_EQ(0xcc, output[PIPE_TX_SEGMENT_SIZE]);
}

TEST_F(SharedTransmitTest, CopiesOnlyPublishedBytesAcrossPages) {
    publish(0, 1, 9000, 0x5a);
    pipeRxStore(&guest.segments[0].committed, 5000);
    std::vector<unsigned char> output(10002, 0xcc);
    EXPECT_EQ(5000U, readAvailable(output.data() + 1, 10000));
    EXPECT_EQ(0U, readAvailable(output.data() + 5001, 5000));
    pipeRxStore(&guest.segments[0].committed, 9000);
    EXPECT_EQ(4000U, readAvailable(output.data() + 5001, 5000));
    EXPECT_TRUE(std::all_of(output.begin() + 1, output.begin() + 9001,
                            [](unsigned char b) { return b == 0x5a; }));
    EXPECT_EQ(0xcc, output.front());
    EXPECT_EQ(0xcc, output[9001]);
    EXPECT_EQ(0U, host.released[0]);
}

TEST_F(SharedTransmitTest, ReleasesSealedSegmentAfterLastByteAndWrapsGeneration) {
    publish(0, UINT32_MAX, 6000, 1);
    host.read_generation = UINT32_MAX;
    publish(1, 1, 20, 2);
    link(0, 1);
    std::vector<unsigned char> output(6020);
    EXPECT_EQ(2500U, readAvailable(output.data(), 2500));
    EXPECT_EQ(0U, host.released[0]);
    EXPECT_EQ(3520U, readAvailable(output.data() + 2500, 3520));
    EXPECT_EQ(UINT32_MAX, host.released[0]);
    publish(0, 1, 32, 3);
    link(1, 0);
    unsigned char bytes[32];
    EXPECT_EQ(32U, readAvailable(bytes, sizeof(bytes)));
    EXPECT_TRUE(std::all_of(bytes, bytes + 32, [](unsigned char b) { return b == 3; }));
    EXPECT_EQ(1U, host.released[1]);
}

TEST_F(SharedTransmitTest, LargeAllocationAndSnapshotKeepPartialPosition) {
    add(5 * 1024 * 1024);
    publish(2, 1, storage[2].size(), 0xa6);
    link(0, 2);
    std::vector<unsigned char> output(storage[2].size());
    EXPECT_EQ(65553U, readAvailable(output.data(), 65553));
    queue = make(true);
    for (unsigned id = 0; id < storage.size(); ++id) mapping(id);
    ASSERT_TRUE(queue->activate());
    EXPECT_EQ(output.size() - 65553, readAvailable(output.data() + 65553, output.size() - 65553));
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](unsigned char b) { return b == 0xa6; }));
}

TEST_F(SharedTransmitTest, InvalidLengthAndLinkDoNotReadOutsideMappings) {
    guest.segments[0].committed = PIPE_TX_SEGMENT_SIZE + 1;
    unsigned char output = 0xcc;
    EXPECT_EQ(SIZE_MAX, readAvailable(&output, 1));
    EXPECT_EQ(0xcc, output);
    EXPECT_TRUE(host.flags & PIPE_TX_ERROR);
}

TEST_F(SharedTransmitTest, PausedConsumerPreservesPublishedInput) {
    publish(0, 1, 16, 0x42);
    queue->setRunning(false);
    unsigned char output[16]{};
    EXPECT_EQ(0U, readAvailable(output, sizeof(output)));
    EXPECT_EQ(0U, host.read_offset);
    queue->setRunning(true);
    EXPECT_EQ(sizeof(output), readAvailable(output, sizeof(output)));
    EXPECT_EQ(0x42, output[0]);
}

TEST_F(SharedTransmitTest, InvalidNextAndGenerationAreTerminal) {
    guest.segments[0].next = PIPE_TX_MAX_SEGMENTS;
    guest.segments[0].committed = PIPE_TX_SEALED;
    unsigned char output = 0xcc;
    EXPECT_EQ(SIZE_MAX, readAvailable(&output, 1));
    EXPECT_EQ(0xcc, output);
}

TEST_F(SharedTransmitTest, SpaceInterruptRechecksSequenceAndIsOneShot) {
    publish(1, 1, 0, 0);
    link(0, 1);
    queue->arm(host.event_seq);
    unsigned char output;
    EXPECT_EQ(0U, readAvailable(&output, 1));
    EXPECT_EQ(1U, wakes);
    EXPECT_EQ(0U, host.irq_armed);
    queue->arm(host.event_seq - 1);
    EXPECT_EQ(2U, wakes);
    queue->arm(host.event_seq);
    queue->close();
    EXPECT_EQ(3U, wakes);
    EXPECT_EQ(SIZE_MAX, readAvailable(&output, 1));
}

TEST_F(SharedTransmitTest, ConcurrentRecyclingPreservesByteOrder) {
    constexpr unsigned iterations = 1000;
    constexpr size_t bytes = 8193;
    std::atomic<bool> failed{false};
    std::thread consumer([&] {
        std::vector<unsigned char> output(bytes);
        for (unsigned n = 0; n < iterations; ++n) {
            size_t offset = 0;
            while (offset < bytes) {
                const size_t count = readAvailable(output.data() + offset, bytes - offset);
                if (count == SIZE_MAX) { failed = true; return; }
                offset += count;
                if (!count) std::this_thread::yield();
            }
            for (auto b : output) if (b != static_cast<unsigned char>(n)) failed = true;
        }
    });
    unsigned current = 0;
    for (unsigned n = 0; n < iterations; ++n) {
        publish(current, guest.segments[current].generation, bytes, n);
        if (n + 1 == iterations) break;
        unsigned next = 1 - current;
        uint32_t generation = guest.segments[next].generation;
        while (generation && pipeRxLoad(&host.released[next]) != generation && !failed)
            std::this_thread::yield();
        if (failed) break;
        pipeRxStore(&guest.segments[next].committed, 0);
        pipeRxStore(&guest.segments[next].generation, generation + 1);
        link(current, next);
        current = next;
    }
    consumer.join();
    EXPECT_FALSE(failed);
}
}
}
