// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0

#include "android/base/async/AsyncSocket.h"

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

class AsyncSocketTest : public ::testing::Test,
                        public AsyncSocketEventListener {
protected:
    void SetUp() override {
        mServer = socketTcp4LoopbackServer(0);
        ASSERT_GE(mServer.get(), 0);
        socketSetNonBlocking(mServer.get());
        std::promise<Looper*> ready;
        auto result = ready.get_future();
        mLooperThread = std::thread([&ready] {
            std::unique_ptr<Looper> looper(Looper::create());
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

}  // namespace
}  // namespace base
}  // namespace android
