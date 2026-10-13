// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-only

#include "android-qemu2-glue/netsim/NetsimWifiConnection.h"

#include <deque>
#include <utility>

#include "aemu/base/logging/Log.h"
#include "grpcpp/grpcpp.h"
#include "netsim/packet_streamer.grpc.pb.h"

namespace android {
namespace qemu2 {
namespace {
// Bound memory while the VM is paused or a peer stops consuming packets.
constexpr size_t kMaxQueuedPackets = 256;
using netsim::packet::PacketRequest;
using netsim::packet::PacketResponse;
using netsim::packet::PacketStreamer;
}  // namespace

class NetsimWifiConnection::Session
    : public grpc::ClientBidiReactor<PacketRequest, PacketResponse> {
public:
    using OnDoneCallback = std::function<void(const grpc::Status&)>;

    Session(std::shared_ptr<grpc::Channel> channel,
            OnReceive onReceive,
            OnDoneCallback onDone)
        : mStub(PacketStreamer::NewStub(channel)),
          mOnReceive(std::move(onReceive)),
          mOnDone(std::move(onDone)) {}

    void start(const std::shared_ptr<Session>& self,
               const PacketRequest& registration) {
        mSelf = self;
        mStub->async()->StreamPackets(&mContext, this);
        // sendPacket() starts writes outside reactions. Keep OnDone from racing
        // those writes until cancel()/a failed read or write closes the gate.
        AddHold();
        mOutgoing.push_back(registration);
        mWriting = true;
        StartRead(&mRead);
        StartWrite(&mOutgoing.front());
        StartCall();
    }

    bool sendPacket(const std::string& packet) {
        std::lock_guard<std::mutex> lock(mWriteMutex);
        if (mClosed || mOutgoing.size() >= kMaxQueuedPackets) {
            return false;
        }
        mOutgoing.emplace_back();
        mOutgoing.back().set_packet(packet);
        if (!mWriting) {
            mWriting = true;
            StartWrite(&mOutgoing.front());
        }
        return true;
    }

    void cancel() {
        {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            if (mClosed) {
                return;
            }
            mClosed = true;
        }
        mContext.TryCancel();
        // Exactly one caller removes the external-writer hold. No further
        // StartWrite is possible once mClosed is set under mWriteMutex.
        RemoveHold();
    }

    void await() {
        std::unique_lock<std::mutex> lock(mDoneMutex);
        mDoneWake.wait(lock, [this] { return mDone; });
    }

    void OnReadDone(bool ok) override {
        if (!ok) {
            cancel();
            return;
        }
        if (mRead.has_packet()) {
            mOnReceive(mRead.packet());
        }
        StartRead(&mRead);
    }

    void OnWriteDone(bool ok) override {
        {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            mOutgoing.pop_front();
            mWriting = false;
            if (ok && !mClosed && !mOutgoing.empty()) {
                mWriting = true;
                StartWrite(&mOutgoing.front());
            }
        }
        if (!ok) {
            cancel();
        }
    }

    void OnDone(const grpc::Status& status) override {
        // Keep this reactor alive even if the worker releases its reference
        // as soon as it receives the completion notification.
        auto self = std::move(mSelf);
        mOnDone(status);
        std::lock_guard<std::mutex> lock(mDoneMutex);
        mDone = true;
        mDoneWake.notify_all();
    }

private:
    grpc::ClientContext mContext;
    std::unique_ptr<PacketStreamer::Stub> mStub;
    const OnReceive mOnReceive;
    const OnDoneCallback mOnDone;
    std::shared_ptr<Session> mSelf;
    PacketResponse mRead;
    std::mutex mWriteMutex;
    std::deque<PacketRequest> mOutgoing;
    bool mWriting = false;
    bool mClosed = false;
    std::mutex mDoneMutex;
    std::condition_variable mDoneWake;
    bool mDone = false;
};

NetsimWifiConnection::NetsimWifiConnection(
        ChannelFactory channelFactory,
        PacketRequest registration,
        OnReceive onReceive,
        std::chrono::milliseconds retryDelay)
    : mChannelFactory(std::move(channelFactory)),
      mRegistration(std::move(registration)),
      mOnReceive(std::move(onReceive)),
      mRetryDelay(retryDelay) {}

NetsimWifiConnection::~NetsimWifiConnection() {
    stop();
}

bool NetsimWifiConnection::stopped() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mStopped;
}

bool NetsimWifiConnection::start() {
    std::lock_guard<std::mutex> joinLock(mJoinMutex);
    auto channel = mChannelFactory([this] { return stopped(); });
    std::lock_guard<std::mutex> lock(mMutex);
    if (!channel || mStopped) {
        return false;
    }
    connectLocked(std::move(channel));
    mWorker = std::thread(&NetsimWifiConnection::run, this);
    return true;
}

void NetsimWifiConnection::connectLocked(std::shared_ptr<grpc::Channel> channel) {
    mSessionDone = false;
    mSession = std::make_shared<Session>(
            std::move(channel),
            [this](const std::string& packet) {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mStopped && mIncoming.size() < kMaxQueuedPackets) {
                    mIncoming.push(packet);
                    mWake.notify_one();
                }
            },
            [this](const grpc::Status& status) {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mStopped) {
                    LOG(WARNING) << "Netsim WiFi disconnected: "
                                 << status.error_message() << "; reconnecting";
                }
                mSessionDone = true;
                mWake.notify_one();
            });
    // Registration must be the first write on EVERY new stream. Only publish
    // the stream to sendPacket() after start() has queued it and installed its hold.
    mSession->start(mSession, mRegistration);
}

bool NetsimWifiConnection::sendPacket(const std::string& packet) {
    std::lock_guard<std::mutex> lock(mMutex);
    return !mStopped && !mSessionDone && mSession && mSession->sendPacket(packet);
}

void NetsimWifiConnection::run() {
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mStopped) {
        if (mSessionDone) {
            mSession.reset();
            // Frames from the old AP/session must not be replayed on reconnect.
            mIncoming = {};
            if (mWake.wait_for(lock, mRetryDelay, [this] { return mStopped; })) {
                break;
            }
            lock.unlock();
            auto channel = mChannelFactory([this] { return stopped(); });
            lock.lock();
            if (mStopped) {
                break;
            }
            if (!channel) {
                continue;
            }
            connectLocked(std::move(channel));
            LOG(INFO) << "Re-established netsim WiFi packet stream";
        }
        mWake.wait(lock, [this] {
            return mStopped || mSessionDone || !mIncoming.empty();
        });
        if (mStopped || mSessionDone) {
            continue;
        }
        auto packet = std::move(mIncoming.front());
        mIncoming.pop();
        lock.unlock();
        mOnReceive(packet);
        lock.lock();
    }
    auto session = std::move(mSession);
    mIncoming = {};
    lock.unlock();
    if (session) {
        session->cancel();
        session->await();
    }
}

void NetsimWifiConnection::stop() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStopped = true;
        session = mSession;
        mWake.notify_all();
    }
    if (session) {
        session->cancel();
    }
    std::lock_guard<std::mutex> joinLock(mJoinMutex);
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

}  // namespace qemu2
}  // namespace android
