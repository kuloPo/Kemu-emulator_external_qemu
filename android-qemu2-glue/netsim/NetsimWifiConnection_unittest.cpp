// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-only

#include "android-qemu2-glue/netsim/NetsimWifiConnection.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "grpcpp/grpcpp.h"
#include "gtest/gtest.h"
#include "netsim/packet_streamer.grpc.pb.h"

namespace android {
namespace qemu2 {
namespace {
using namespace std::chrono_literals;
using netsim::packet::PacketRequest;
using netsim::packet::PacketResponse;
using netsim::packet::PacketStreamer;

class WifiPeer : public PacketStreamer::Service {
public:
    grpc::Status StreamPackets(
            grpc::ServerContext* context,
            grpc::ServerReaderWriter<PacketResponse, PacketRequest>* stream)
            override {
        PacketRequest request;
        if (!stream->Read(&request)) {
            return grpc::Status::CANCELLED;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            registrations.push_back(request);
            changed.notify_all();
        }
        // Simulate a netsimd which accepts registration but stops reading.
        while (pauseReads && !context->IsCancelled()) {
            std::this_thread::sleep_for(1ms);
        }
        while (stream->Read(&request)) {
            PacketResponse response;
            response.set_packet(request.packet());
            if (!stream->Write(response)) {
                break;
            }
        }
        return grpc::Status::OK;
    }

    bool waitForRegistrations(size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] {
            return registrations.size() >= count;
        });
    }

    std::atomic<bool> pauseReads{false};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<PacketRequest> registrations;
};

class NetsimWifiConnectionTest : public ::testing::Test {
protected:
    void SetUp() override { startPeer(); }

    void TearDown() override {
        if (connection) {
            connection->stop();
        }
        stopPeer();
    }

    void startPeer() {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&peer);
        server = builder.BuildAndStart();
        ASSERT_NE(nullptr, server);
        std::atomic_store(&channel,
                          grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                              grpc::InsecureChannelCredentials()));
    }

    void stopPeer() {
        std::atomic_store(&channel, std::shared_ptr<grpc::Channel>());
        if (server) {
            server->Shutdown(std::chrono::system_clock::now());
            server->Wait();
            server.reset();
        }
    }

    void makeConnection(std::chrono::milliseconds retryDelay = 20ms) {
        PacketRequest registration;
        auto* info = registration.mutable_initial_info();
        info->set_name("wifi-reconnect-test");
        info->mutable_chip()->set_kind(netsim::common::ChipKind::WIFI);
        info->mutable_device_info()->set_name("wifi-reconnect-test");
        connection = std::make_unique<NetsimWifiConnection>(
                [this](const auto& cancelled) {
                    ++factoryCalls;
                    while (blockFactory && !cancelled()) {
                        factoryBlocked = true;
                        std::this_thread::sleep_for(1ms);
                    }
                    return cancelled() ? nullptr : std::atomic_load(&channel);
                },
                std::move(registration),
                [this](const std::string& packet) {
                    std::lock_guard<std::mutex> lock(receiveMutex);
                    received.push_back(packet);
                    receivedChanged.notify_all();
                },
                retryDelay);
    }

    bool waitForPacket(const std::string& packet) {
        std::unique_lock<std::mutex> lock(receiveMutex);
        return receivedChanged.wait_for(lock, 5s, [&] {
            return std::find(received.begin(), received.end(), packet) !=
                   received.end();
        });
    }

    template <class Predicate>
    bool waitUntil(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    WifiPeer peer;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<NetsimWifiConnection> connection;
    std::atomic<int> factoryCalls{0};
    std::atomic<bool> blockFactory{false};
    std::atomic<bool> factoryBlocked{false};
    std::mutex receiveMutex;
    std::condition_variable receivedChanged;
    std::vector<std::string> received;
};

TEST_F(NetsimWifiConnectionTest, ReconnectsAcrossPortsAndRegistersBeforeTraffic) {
    makeConnection();
    ASSERT_TRUE(connection->start());
    ASSERT_TRUE(peer.waitForRegistrations(1));
    ASSERT_TRUE(connection->sendPacket("before-restart"));
    ASSERT_TRUE(waitForPacket("before-restart"));

    for (int restart = 0; restart < 2; ++restart) {
        stopPeer();
        const int attempts = factoryCalls;
        // A temporarily unavailable channel must not terminate the retry loop.
        ASSERT_TRUE(waitUntil([&] { return factoryCalls >= attempts + 2; }));
        EXPECT_FALSE(connection->sendPacket("stale-frame"));
        startPeer();
        ASSERT_TRUE(peer.waitForRegistrations(restart + 2));
        const auto packet = "after-restart-" + std::to_string(restart);
        ASSERT_TRUE(connection->sendPacket(packet));
        ASSERT_TRUE(waitForPacket(packet));
    }
    connection->stop();
    std::lock_guard<std::mutex> lock(peer.mutex);
    ASSERT_EQ(3U, peer.registrations.size());
    for (const auto& registration : peer.registrations) {
        ASSERT_TRUE(registration.has_initial_info());
        EXPECT_EQ(netsim::common::ChipKind::WIFI,
                  registration.initial_info().chip().kind());
        EXPECT_EQ("wifi-reconnect-test", registration.initial_info().name());
        EXPECT_EQ("wifi-reconnect-test",
                  registration.initial_info().device_info().name());
    }
    EXPECT_EQ(received.end(),
              std::find(received.begin(), received.end(), "stale-frame"));
}

TEST_F(NetsimWifiConnectionTest, StopInterruptsRetryDelay) {
    makeConnection(10s);
    ASSERT_TRUE(connection->start());
    ASSERT_TRUE(peer.waitForRegistrations(1));
    stopPeer();
    ASSERT_TRUE(waitUntil([&] { return !connection->sendPacket("probe"); }));
    const auto started = std::chrono::steady_clock::now();
    connection->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_EQ(1, factoryCalls);
    EXPECT_FALSE(connection->sendPacket("after-stop"));
    connection->stop();
}

TEST_F(NetsimWifiConnectionTest, StopCancelsChannelDiscovery) {
    makeConnection();
    ASSERT_TRUE(connection->start());
    ASSERT_TRUE(peer.waitForRegistrations(1));
    blockFactory = true;
    stopPeer();
    ASSERT_TRUE(waitUntil([&] { return factoryBlocked.load(); }));
    const auto started = std::chrono::steady_clock::now();
    connection->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_FALSE(connection->sendPacket("after-stop"));
}

TEST_F(NetsimWifiConnectionTest, InitialFailureAllowsFallback) {
    stopPeer();
    makeConnection();
    EXPECT_FALSE(connection->start());
    EXPECT_EQ(1, factoryCalls);
    EXPECT_FALSE(connection->sendPacket("not-connected"));
    connection->stop();
}

TEST_F(NetsimWifiConnectionTest, BoundsWritesAndStopsDuringConcurrentSends) {
    peer.pauseReads = true;
    makeConnection();
    ASSERT_TRUE(connection->start());
    ASSERT_TRUE(peer.waitForRegistrations(1));
    const std::string packet(64 * 1024, 'x');
    bool rejected = false;
    for (int i = 0; i < 4096; ++i) {
        if (!connection->sendPacket(packet)) {
            rejected = true;
            break;
        }
    }
    ASSERT_TRUE(rejected);
    std::atomic<bool> sending{true};
    std::thread sender([&] {
        while (sending) {
            connection->sendPacket(packet);
            std::this_thread::yield();
        }
    });
    connection->stop();
    sending = false;
    sender.join();
    EXPECT_FALSE(connection->sendPacket("after-stop"));
    EXPECT_EQ(1, factoryCalls);
    EXPECT_TRUE(received.empty());
}

}  // namespace
}  // namespace qemu2
}  // namespace android
