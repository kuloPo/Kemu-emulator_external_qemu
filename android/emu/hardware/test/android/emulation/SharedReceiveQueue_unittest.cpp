// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#include "android/emulation/SharedReceiveQueue.h"
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace android {
namespace {
class SharedReceiveTest : public ::testing::Test {
protected:
    PipeRxHost host{};
    PipeRxGuest guest{};
    std::vector<std::vector<unsigned char>> storage;
    std::unique_ptr<SharedReceiveQueue> queue;
    std::atomic<unsigned> wakes{0};
    unsigned reader = 0;
    uint32_t offset = 0;

    void SetUp() override {
        storage.reserve(PIPE_RX_MAX_SEGMENTS);
        queue = makeQueue(false);
        add();
        add();
        ASSERT_TRUE(queue->activate());
    }
    std::unique_ptr<SharedReceiveQueue> makeQueue(bool restoring) {
        return std::make_unique<SharedReceiveQueue>(&host, &guest,
            [](void*, size_t) {}, [this] { wakes.fetch_add(1); }, restoring);
    }
    void add() {
        unsigned id = storage.size();
        storage.emplace_back(PIPE_RX_SEGMENT_SIZE);
        addMapping(id);
    }
    void addMapping(unsigned id) {
        void* pages[PIPE_RX_SEGMENT_PAGES];
        for (unsigned i = 0; i < PIPE_RX_SEGMENT_PAGES; ++i)
            pages[i] = storage[id].data() + i * PIPE_RX_PAGE_SIZE;
        ASSERT_TRUE(queue->add(id, pages));
    }
    size_t writeAvailable(const void* source, size_t size) {
        size_t total = 0;
        while (total < size) {
            size_t n = queue->write(static_cast<const char*>(source) + total, size - total);
            if (!n) break;
            total += n;
        }
        return total;
    }
    size_t read(unsigned char* target, size_t size) {
        size_t count = 0;
        while (count < size) {
            auto& segment = host.segments[reader];
            uint32_t committed = pipeRxLoad(&segment.committed);
            uint32_t end = committed & ~PIPE_RX_SEALED;
            if (end < offset || end > PIPE_RX_SEGMENT_SIZE) return 0;
            size_t n = std::min<size_t>(size - count, end - offset);
            memcpy(target + count, storage[reader].data() + offset, n);
            count += n;
            offset += n;
            if (offset == end && (committed & PIPE_RX_SEALED)) {
                unsigned previous = reader;
                uint32_t generation = segment.generation;
                reader = segment.next;
                offset = 0;
                pipeRxStore(&guest.released[previous], generation);
                pipeRxFence();
            } else if (!n) break;
        }
        return count;
    }
};

TEST_F(SharedReceiveTest, StopPreservesRamAndResumePublishesPendingReply) {
    const std::vector<unsigned char> source(2 * PIPE_RX_SEGMENT_SIZE, 0x73);
    EXPECT_EQ(PIPE_RX_SEGMENT_SIZE, queue->write(source.data(), source.size()));
    queue->setRunning(false);
    const auto savedHost = host;
    const auto savedStorage = storage;
    EXPECT_EQ(0U, queue->write(source.data(), source.size()));
    EXPECT_EQ(0, std::memcmp(&savedHost, &host, sizeof(host)));
    EXPECT_EQ(savedStorage, storage);
    queue->setRunning(true);
    EXPECT_EQ(PIPE_RX_SEGMENT_SIZE, queue->write(source.data(), source.size()));
    std::vector<unsigned char> result(source.size());
    EXPECT_EQ(result.size(), read(result.data(), result.size()));
    EXPECT_EQ(source, result);
}

TEST_F(SharedReceiveTest, LargeReplyGrowsWithoutChangingByteOrder) {
    std::vector<unsigned char> source(9 * PIPE_RX_SEGMENT_SIZE + 19), result(source.size());
    for (size_t i = 0; i < source.size(); ++i) source[i] = (i * 37 + i / 257) & 255;
    size_t sent = writeAvailable(source.data(), source.size());
    ASSERT_EQ(2 * PIPE_RX_SEGMENT_SIZE, sent);
    ASSERT_TRUE(host.flags & PIPE_RX_NEED_SEGMENT);
    add();
    add();
    size_t received = 0;
    for (unsigned step = 0; step < 10000 && received < result.size(); ++step) {
        received += read(result.data() + received, std::min<size_t>(7919, result.size() - received));
        sent += writeAvailable(source.data() + sent, source.size() - sent);
    }
    EXPECT_EQ(source.size(), sent);
    EXPECT_EQ(result.size(), received);
    EXPECT_EQ(source, result);
}

TEST_F(SharedReceiveTest, ConcurrentRecyclingPreservesPayload) {
    std::vector<unsigned char> source(8 * 1024 * 1024 + 7), result(source.size());
    for (size_t i = 0; i < source.size(); ++i) source[i] = (i ^ (i >> 11)) & 255;
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        size_t sent = 0;
        while (sent < source.size() && !stop.load()) {
            sent += writeAvailable(source.data() + sent, source.size() - sent);
            std::this_thread::yield();
        }
    });
    size_t received = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received < result.size() && std::chrono::steady_clock::now() < deadline) {
        received += read(result.data() + received, std::min<size_t>(4093, result.size() - received));
        std::this_thread::yield();
    }
    stop.store(true);
    producer.join();
    EXPECT_EQ(result.size(), received);
    EXPECT_EQ(source, result);
}

TEST_F(SharedReceiveTest, ArmRechecksSequenceAndInterruptIsOneShot) {
    uint32_t data = 0x12345678;
    host.event_seq = UINT32_MAX;
    queue->arm(UINT32_MAX);
    EXPECT_EQ(sizeof(data), writeAvailable(&data, sizeof(data)));
    EXPECT_EQ(0U, host.event_seq);
    EXPECT_EQ(1U, wakes.load());
    EXPECT_EQ(sizeof(data), writeAvailable(&data, sizeof(data)));
    EXPECT_EQ(1U, wakes.load());
    queue->arm(0);
    EXPECT_EQ(2U, wakes.load());
    queue->arm(host.event_seq);
    queue->close();
    EXPECT_EQ(3U, wakes.load());
    EXPECT_TRUE(host.flags & PIPE_RX_CLOSED);
}

TEST_F(SharedReceiveTest, RestoreRetainsPartiallyConsumedSegments) {
    std::vector<unsigned char> source(PIPE_RX_SEGMENT_SIZE + 39, 0x7b), result(source.size());
    ASSERT_EQ(source.size(), writeAvailable(source.data(), source.size()));
    size_t received = read(result.data(), 123);
    queue.reset();
    queue = makeQueue(true);
    for (unsigned i = 0; i < storage.size(); ++i) addMapping(i);
    ASSERT_TRUE(queue->activate());
    received += read(result.data() + received, result.size() - received);
    EXPECT_EQ(result.size(), received);
    EXPECT_EQ(source, result);
}

TEST_F(SharedReceiveTest, GenerationWrapAndInvalidMappings) {
    host.segments[0].generation = UINT32_MAX;
    std::vector<unsigned char> source(3 * PIPE_RX_SEGMENT_SIZE + 1, 0xff), result(source.size());
    size_t sent = writeAvailable(source.data(), source.size());
    size_t received = read(result.data(), PIPE_RX_SEGMENT_SIZE);
    sent += writeAvailable(source.data() + sent, source.size() - sent);
    received += read(result.data() + received, result.size() - received);
    sent += writeAvailable(source.data() + sent, source.size() - sent);
    received += read(result.data() + received, result.size() - received);
    EXPECT_EQ(1U, host.segments[0].generation);
    EXPECT_EQ(source.size(), sent);
    EXPECT_EQ(source, result);
    void* pages[PIPE_RX_SEGMENT_PAGES]{};
    EXPECT_FALSE(queue->add(0, pages));
    EXPECT_FALSE(queue->add(storage.size(), pages));
}
}
}
