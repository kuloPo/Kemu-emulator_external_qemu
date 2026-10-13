// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "netsim/packet_streamer.pb.h"

namespace grpc {
class Channel;
}

namespace android {
namespace qemu2 {

// Owns successive Wi-Fi streams. Guest callbacks and channel discovery run on
// the worker, never on a gRPC reaction thread. start() is called once; stop()
// drains callbacks, so its caller must not hold a lock needed by onReceive.
class NetsimWifiConnection {
public:
    using IsCancelled = std::function<bool()>;
    using ChannelFactory =
            std::function<std::shared_ptr<grpc::Channel>(const IsCancelled&)>;
    using OnReceive = std::function<void(const std::string&)>;

    NetsimWifiConnection(ChannelFactory channelFactory,
                         netsim::packet::PacketRequest registration,
                         OnReceive onReceive,
                         std::chrono::milliseconds retryDelay =
                                 std::chrono::seconds(1));
    ~NetsimWifiConnection();

    bool start();
    bool sendPacket(const std::string& packet);
    void stop();

private:
    class Session;
    bool stopped();
    void connectLocked(std::shared_ptr<grpc::Channel> channel);
    void run();

    const ChannelFactory mChannelFactory;
    const netsim::packet::PacketRequest mRegistration;
    const OnReceive mOnReceive;
    const std::chrono::milliseconds mRetryDelay;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::queue<std::string> mIncoming;
    std::shared_ptr<Session> mSession;
    bool mSessionDone = false;
    bool mStopped = false;
    std::thread mWorker;
    std::mutex mJoinMutex;
};

}  // namespace qemu2
}  // namespace android
