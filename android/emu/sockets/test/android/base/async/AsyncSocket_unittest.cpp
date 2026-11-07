// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0

#include "android/base/async/AsyncSocket.h"
#include "aemu/base/async/DefaultLooper.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace android {
namespace base {
namespace {

using namespace std::chrono_literals;

class SocketTestLooper : public DefaultLooper {
public:
    bool onLooperThread() const override {
        return std::this_thread::get_id() == mOwner;
    }

private:
    const std::thread::id mOwner = std::this_thread::get_id();
};

class PausedLooper {
public:
    explicit PausedLooper(Looper* looper, std::function<void()> onResume = {}) {
        auto resumed = mResume.get_future().share();
        auto paused = std::make_shared<std::promise<void>>();
        auto ready = paused->get_future();
        looper->scheduleCallback([paused, resumed, onResume = std::move(onResume)] {
            paused->set_value();
            resumed.wait();
            if (onResume) onResume();
        });
        ready.wait();
    }

    ~PausedLooper() { resume(); }

    void resume() {
        if (!mResumed) {
            mResumed = true;
            mResume.set_value();
        }
    }

private:
    std::promise<void> mResume;
    bool mResumed = false;
};

class AsyncSocketTest : public ::testing::Test,
                        public AsyncSocketEventListener {
protected:
    void SetUp() override {
        mServer = socketTcp4LoopbackServer(0);
        ASSERT_GE(mServer.get(), 0);
        socketSetNonBlocking(mServer.get());
        std::promise<Looper*> ready;
        auto result = ready.get_future();
        mLooperThread = std::thread([ready = std::move(ready)]() mutable {
            std::unique_ptr<Looper> looper(new SocketTestLooper());
            std::unique_ptr<Looper::Timer> keepAlive(looper->createTimer(
                    [](void*, Looper::Timer* timer) { timer->startRelative(10); },
                    nullptr));
            keepAlive->startRelative(10);
            ready.set_value(looper.get());
            looper->run();
        });
        mLooper = result.get();
        mSocket = std::make_unique<AsyncSocket>(mLooper, socketGetPort(mServer.get()));
        mSocket->setSocketEventListener(this);
    }

    void TearDown() override {
        mSocket.reset();
        if (mLooper) mLooper->scheduleCallback([this] { mLooper->forceQuit(); });
        if (mLooperThread.joinable()) mLooperThread.join();
    }

    void onConnected(AsyncSocketAdapter*) override { ++mConnections; }
    void onRead(AsyncSocketAdapter* socket) override {
        char byte;
        socket->recv(&byte, 1);
    }
    void onClose(AsyncSocketAdapter*, int) override {
        std::lock_guard<std::mutex> lock(mCloseMutex);
        ++mCloses;
        mCloseCv.notify_all();
    }

    bool waitForClose(int count) {
        std::unique_lock<std::mutex> lock(mCloseMutex);
        return mCloseCv.wait_for(lock, 5s, [&] { return mCloses >= count; });
    }

    ScopedSocket acceptPeer() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            ScopedSocket peer(socketAcceptAny(mServer.get()));
            if (peer.get() >= 0) return peer;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return ScopedSocket();
    }

    bool waitForDisposalStart() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            if (!mSocket->connect()) return true;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    ScopedSocket mServer;
    Looper* mLooper = nullptr;
    std::thread mLooperThread;
    std::unique_ptr<AsyncSocket> mSocket;
    std::atomic<int> mConnections{0};
    std::mutex mCloseMutex;
    std::condition_variable mCloseCv;
    int mCloses = 0;
};

TEST_F(AsyncSocketTest, ReconnectAfterClose) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        ASSERT_TRUE(mSocket->connectSync(5s));
        ScopedSocket peer(socketAcceptAny(mServer.get()));
        ASSERT_GE(peer.get(), 0);
        EXPECT_EQ(attempt + 1, mConnections.load());
        mSocket->close();
        ASSERT_TRUE(waitForClose(attempt + 1));
    }
}

TEST_F(AsyncSocketTest, ReconnectAfterPeerDisconnect) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        ASSERT_TRUE(mSocket->connectSync(5s));
        ScopedSocket peer(socketAcceptAny(mServer.get()));
        ASSERT_GE(peer.get(), 0);
        peer.close();
        ASSERT_TRUE(waitForClose(attempt + 1));
    }
    EXPECT_EQ(20, mConnections.load());
}

TEST_F(AsyncSocketTest, ConnectWhileConnectedKeepsConnection) {
    ASSERT_TRUE(mSocket->connectSync(5s));
    ScopedSocket peer(socketAcceptAny(mServer.get()));
    ASSERT_GE(peer.get(), 0);
    for (int attempt = 0; attempt < 20; ++attempt) {
        ASSERT_TRUE(mSocket->connect());
    }
    EXPECT_EQ(1, mConnections.load());
    EXPECT_TRUE(mSocket->connected());
}

TEST_F(AsyncSocketTest, ConcurrentConnectRequestsShareOneAttempt) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::promise<void> start;
        auto ready = start.get_future().share();
        std::vector<std::thread> callers;
        for (int i = 0; i < 8; ++i) {
            callers.emplace_back([&, ready] {
                ready.wait();
                EXPECT_TRUE(mSocket->connect());
            });
        }
        start.set_value();
        for (auto& caller : callers) caller.join();
        ASSERT_TRUE(mSocket->connectSync(5s));
        ScopedSocket peer(socketAcceptAny(mServer.get()));
        ASSERT_GE(peer.get(), 0);
        EXPECT_EQ(attempt + 1, mConnections.load());
        mSocket->close();
        ASSERT_TRUE(waitForClose(attempt + 1));
    }
}

TEST_F(AsyncSocketTest, ConnectTimeoutWhileLooperPaused) {
    PausedLooper paused(mLooper);
    auto result = std::async(std::launch::async, [this] { return mSocket->connectSync(50ms); });
    auto peer = acceptPeer();
    EXPECT_GE(peer.get(), 0);
    const auto status = result.wait_for(1s);
    EXPECT_EQ(std::future_status::ready, status);
    EXPECT_EQ(0, mConnections.load());
    paused.resume();
    EXPECT_FALSE(result.get());
    ASSERT_TRUE(mSocket->connectSync(5s));
    EXPECT_EQ(1, mConnections.load());
}

TEST_F(AsyncSocketTest, CloseCancelsPendingNotificationAndAllowsReconnect) {
    PausedLooper paused(mLooper);
    ASSERT_TRUE(mSocket->connect());
    auto peer = acceptPeer();
    EXPECT_GE(peer.get(), 0);
    auto closed = std::async(std::launch::async, [this] { mSocket->close(); });
    const auto status = closed.wait_for(1s);
    EXPECT_EQ(std::future_status::ready, status);
    if (status != std::future_status::ready) paused.resume();
    closed.get();
    ASSERT_TRUE(mSocket->connect());
    auto secondPeer = acceptPeer();
    EXPECT_GE(secondPeer.get(), 0);
    paused.resume();
    ASSERT_TRUE(mSocket->connectSync(5s));
    EXPECT_EQ(1, mConnections.load());
}

TEST_F(AsyncSocketTest, DisposeCancelsPendingNotificationAndWaiter) {
    PausedLooper paused(mLooper);
    auto result = std::async(std::launch::async, [this] { return mSocket->connectSync(5s); });
    auto peer = acceptPeer();
    EXPECT_GE(peer.get(), 0);
    auto disposed = std::async(std::launch::async, [this] { mSocket->dispose(); });
    const auto status = disposed.wait_for(1s);
    EXPECT_EQ(std::future_status::ready, status);
    if (status != std::future_status::ready) paused.resume();
    disposed.get();
    EXPECT_EQ(std::future_status::ready, result.wait_for(1s));
    EXPECT_FALSE(result.get());
    EXPECT_FALSE(mSocket->connect());
    // A stale callback must be harmless even after the socket is destroyed.
    mSocket.reset();
    paused.resume();
    auto drained = std::make_shared<std::promise<void>>();
    auto done = drained->get_future();
    mLooper->scheduleCallback([drained] { drained->set_value(); });
    done.wait();
    EXPECT_EQ(0, mConnections.load());
}

TEST_F(AsyncSocketTest, DisposeCancelsConnectionRetries) {
    mServer.close();
    ASSERT_FALSE(mSocket->connectSync(50ms));
    const auto start = std::chrono::steady_clock::now();
    mSocket->dispose();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);
    EXPECT_FALSE(mSocket->connectSync(5s));
}

TEST_F(AsyncSocketTest, ConcurrentDisposeWaitsForCleanup) {
    ASSERT_TRUE(mSocket->connectSync(5s));
    auto peer = acceptPeer();
    ASSERT_GE(peer.get(), 0);
    PausedLooper paused(mLooper);
    auto first = std::async(std::launch::async, [this] { mSocket->dispose(); });
    EXPECT_TRUE(waitForDisposalStart());
    auto second = std::async(std::launch::async, [this] { mSocket->dispose(); });
    EXPECT_EQ(std::future_status::timeout, first.wait_for(50ms));
    EXPECT_EQ(std::future_status::timeout, second.wait_for(50ms));
    paused.resume();
    first.get();
    second.get();
    EXPECT_FALSE(mSocket->connected());
    EXPECT_EQ(1, mCloses);
}

TEST_F(AsyncSocketTest, OwnerThreadCanFinishConcurrentDispose) {
    ASSERT_TRUE(mSocket->connectSync(5s));
    auto peer = acceptPeer();
    ASSERT_GE(peer.get(), 0);
    auto ownerDone = std::make_shared<std::promise<void>>();
    auto completed = ownerDone->get_future();
    PausedLooper paused(mLooper, [this, ownerDone] {
        EXPECT_TRUE(mLooper->onLooperThread());
        mSocket->dispose();
        EXPECT_FALSE(mSocket->connected());
        ownerDone->set_value();
    });
    auto first = std::async(std::launch::async, [this] { mSocket->dispose(); });
    EXPECT_TRUE(waitForDisposalStart());
    paused.resume();
    EXPECT_EQ(std::future_status::ready, completed.wait_for(5s));
    first.get();
    EXPECT_EQ(1, mCloses);
}

}  // namespace
}  // namespace base
}  // namespace android
