// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "android/emulation/AdbHostScreencap.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "android/emulation/KemuTabPipe.h"
#include "render-utils/Renderer.h"

namespace android {
namespace emulation {
namespace {

constexpr uint32_t kHostId = 7;
constexpr uint32_t kMaxPayload = 4096;
constexpr char kGzipService[] = "exec:screencap | gzip -1";
using GuestDisplay = AdbHostScreencap::GuestDisplay;

std::string packet(uint32_t command, uint32_t arg0, uint32_t arg1, const std::string& payload = {}) {
    amessage header;
    header.command = command;
    header.arg0 = arg0;
    header.arg1 = arg1;
    header.data_length = static_cast<unsigned>(payload.size());
    header.magic = command ^ 0xffffffff;
    return std::string(reinterpret_cast<const char*>(&header), sizeof(header)) + payload;
}

std::string open(const std::string& service) {
    return packet(ADB_OPEN, kHostId, 0, service + '\0');
}

amessage headerAt(const std::string& bytes, size_t offset = 0) {
    amessage header;
    EXPECT_GE(bytes.size(), offset + sizeof(header));
    memcpy(&header, bytes.data() + offset, sizeof(header));
    return header;
}

std::string inflated(const std::string& gzip) {
    z_stream z = {};
    EXPECT_EQ(Z_OK, inflateInit2(&z, MAX_WBITS + 16));
    std::string out(16 << 20, '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gzip.data()));
    z.avail_in = static_cast<uInt>(gzip.size());
    z.next_out = reinterpret_cast<Bytef*>(&out[0]);
    z.avail_out = static_cast<uInt>(out.size());
    EXPECT_EQ(Z_STREAM_END, inflate(&z, Z_FINISH));
    EXPECT_EQ(0u, z.avail_in);
    out.resize(z.total_out);
    inflateEnd(&z);
    return out;
}

// Stands in for Renderer::getScreenshot() with frames of the given sizes, one
// per call, the last one repeating.
AdbHostScreencap::Screenshot frames(std::vector<std::pair<unsigned int, unsigned int>> sizes) {
    auto calls = std::make_shared<size_t>(0);
    return [sizes, calls](unsigned int* width, unsigned int* height, uint8_t* pixels,
                          size_t* size) {
        const auto [w, h] = sizes[std::min(*calls, sizes.size() - 1)];
        ++*calls;
        *width = w;
        *height = h;
        const size_t needed = size_t{4} * w * h;
        if (pixels == nullptr || *size < needed) {
            *size = needed;
            return gfxstream::Renderer::GET_SCREENSHOT_RESULT_PIXELS_SIZE;
        }
        *size = needed;
        memset(pixels, 0x11, needed);
        return 0;
    };
}

class AdbHostScreencapTest : public ::testing::Test {
protected:
    AdbHostScreencapTest()
        : mScreencap([this](const char* data, size_t size) { mToGuest.append(data, size); },
                     [this](const char* data, size_t size) { mToHost.append(data, size); },
                     [this](const AdbHostScreencap::Request& request,
                            std::function<void(AdbHostScreencap::Reply)> done) {
                         mRequests.push_back(request);
                         mDone.push_back(std::move(done));
                     }) {}

    void host(const std::string& bytes) { mScreencap.onHostData(bytes.data(), bytes.size()); }
    void guest(const std::string& bytes) { mScreencap.onGuestData(bytes.data(), bytes.size()); }

    void connect() {
        host(packet(ADB_CNXN, A_VERSION, 1 << 20, "host::"));
        guest(packet(ADB_CNXN, A_VERSION, kMaxPayload, "device::"));
        mToGuest.clear();
        mToHost.clear();
    }

    // Opens the gzip service and returns the id the stream was given.
    uint32_t answer(const std::string& reply) {
        host(open(kGzipService));
        EXPECT_EQ(1u, mDone.size());
        EXPECT_TRUE(mToGuest.empty());
        mDone.back()(reply);
        const amessage okay = headerAt(mToHost);
        EXPECT_EQ(static_cast<unsigned>(ADB_OKAY), okay.command);
        EXPECT_EQ(kHostId, okay.arg1);
        mToHost.erase(0, sizeof(amessage));
        return okay.arg0;
    }

    std::string mToGuest;
    std::string mToHost;
    std::vector<AdbHostScreencap::Request> mRequests;
    std::vector<std::function<void(AdbHostScreencap::Reply)>> mDone;
    AdbHostScreencap mScreencap;
};

TEST(AdbHostScreencap, parse) {
    using Output = AdbHostScreencap::Request::Output;
    auto request = AdbHostScreencap::parse(kGzipService);
    ASSERT_TRUE(request);
    EXPECT_EQ(Output::Gzip, request->output);

    request = AdbHostScreencap::parse(std::string("exec:screencap | nc -w 3 10.0.2.4 13248") + '\0');
    ASSERT_TRUE(request);
    EXPECT_EQ(Output::Netcat, request->output);
    EXPECT_EQ(13248, request->port);

    for (const char* service : {
                 "exec:screencap",
                 "exec:screencap -p",
                 "exec:screencap | gzip -9",
                 "exec:screencap | gzip -1 ",
                 "exec:screencap -d 1 | gzip -1",
                 "shell:screencap | gzip -1",
                 "exec:screencap | nc -w 3 10.0.2.2",
                 "exec:screencap | nc -w 3 10.0.2.2 0",
                 "exec:screencap | nc -w 3 10.0.2.2 65536",
                 "exec:screencap | nc -w 3 10.0.2.2 80; id",
                 "exec:screencap | nc -w 3 10.0.2.256 80",
                 "exec:screencap | nc -w 3 192.168.1.2 80",
                 "exec:screencap | nc -w 5 10.0.2.2 80",
         }) {
        EXPECT_FALSE(AdbHostScreencap::parse(service)) << service;
    }
}

TEST(AdbHostScreencap, gzip) {
    for (const size_t size : {size_t{0}, size_t{1}, size_t{1} << 20, (size_t{3} << 20) + 17}) {
        std::string data(size, '\0');
        for (size_t i = 0; i < size; i++) {
            data[i] = static_cast<char>((i * 2654435761u) >> (i % 3 ? 24 : 13));
        }
        EXPECT_EQ(data, inflated(AdbHostScreencap::gzip(data))) << size;
    }
}

TEST(AdbHostScreencap, captureWritesFinalFrame) {
    // The frame shrinks between the size query and the readback.
    const std::string frame = AdbHostScreencap::capture(GuestDisplay{0, 2, 2}, frames({{4, 4}, {2, 2}}));
    const uint32_t header[] = {2, 2, 1, 1};
    std::string expected(reinterpret_cast<const char*>(header), sizeof(header));
    for (int i = 0; i < 4; i++) {
        expected += "\x11\x11\x11\xff";
    }
    EXPECT_EQ(expected, frame);
}

TEST(AdbHostScreencap, captureRejectsOtherFrames) {
    EXPECT_TRUE(AdbHostScreencap::capture(GuestDisplay{0, 2, 2}, frames({{4, 4}})).empty());
    EXPECT_TRUE(AdbHostScreencap::capture(GuestDisplay{0, 2, 2}, frames({{2, 2}, {4, 4}})).empty());
    EXPECT_TRUE(AdbHostScreencap::capture(GuestDisplay{1, 2, 4}, frames({{4, 2}})).empty());
}

TEST(AdbHostScreencap, tabFrameUsesItsOwnSize) {
    const auto frame = AdbHostScreencap::capture(std::nullopt, frames({{2, 4}}));
    const uint32_t header[] = {2, 4, 1, 1};
    ASSERT_EQ(sizeof(header) + 2 * 4 * 4, frame.size());
    EXPECT_EQ(0, memcmp(frame.data(), header, sizeof(header)));
}

TEST(AdbHostScreencap, pendingTabDoesNotFallBackToLauncher) {
    gfxstream_display_view previous{-1};
    kemu::getPresentationView(&previous);
    kemu::setPresentationView({1});
    const auto frame = AdbHostScreencap::capture();
    kemu::setPresentationView(previous);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->empty());
}

TEST_F(AdbHostScreencapTest, relaysOtherTraffic) {
    const std::string fromHost = packet(ADB_CNXN, A_VERSION, 1 << 20, "host::") +
                                 open("shell:ls") + packet(ADB_WRTE, kHostId, 3, "data") +
                                 packet(ADB_CLSE, kHostId, 3);
    const std::string fromGuest = packet(ADB_CNXN, A_VERSION, kMaxPayload, "device::") +
                                  packet(ADB_OKAY, 3, kHostId) + packet(ADB_WRTE, 3, kHostId, "out");
    // Byte by byte, with the two directions interleaved.
    for (size_t i = 0; i < std::max(fromHost.size(), fromGuest.size()); i++) {
        if (i < fromHost.size()) host(fromHost.substr(i, 1));
        if (i < fromGuest.size()) guest(fromGuest.substr(i, 1));
    }
    EXPECT_EQ(fromHost, mToGuest);
    EXPECT_EQ(fromGuest, mToHost);
    EXPECT_TRUE(mDone.empty());
}

TEST_F(AdbHostScreencapTest, leavesServiceToGuestBeforeConnect) {
    host(open(kGzipService));
    EXPECT_EQ(open(kGzipService), mToGuest);
    EXPECT_TRUE(mDone.empty());
}

TEST_F(AdbHostScreencapTest, answersInPayloadSizedParts) {
    connect();
    const std::string reply(kMaxPayload + 10, 'x');
    const uint32_t id = answer(reply);
    EXPECT_GE(id, 0xf0000000u);
    EXPECT_EQ(packet(ADB_WRTE, id, kHostId, reply.substr(0, kMaxPayload)), mToHost);

    mToHost.clear();
    host(packet(ADB_OKAY, kHostId, id));
    EXPECT_EQ(packet(ADB_WRTE, id, kHostId, reply.substr(kMaxPayload)), mToHost);

    mToHost.clear();
    host(packet(ADB_OKAY, kHostId, id));
    EXPECT_EQ(packet(ADB_CLSE, id, kHostId), mToHost);

    // The host's own close is not for the guest.
    host(packet(ADB_CLSE, kHostId, id));
    EXPECT_TRUE(mToGuest.empty());
}

TEST_F(AdbHostScreencapTest, closesEmptyReplyAtOnce) {
    connect();
    host(open("exec:screencap | nc -w 3 10.0.2.2 4000"));
    ASSERT_EQ(1u, mRequests.size());
    EXPECT_EQ(4000, mRequests[0].port);
    mDone[0](std::string());
    const uint32_t id = headerAt(mToHost).arg0;
    EXPECT_EQ(packet(ADB_OKAY, id, kHostId) + packet(ADB_CLSE, id, kHostId), mToHost);
    EXPECT_TRUE(mToGuest.empty());
}

TEST_F(AdbHostScreencapTest, handsUnansweredStreamToGuest) {
    connect();
    host(open(kGzipService));
    // The stream goes to the guest after the packet the host is in the middle of.
    const std::string other = packet(ADB_WRTE, kHostId + 1, 3, "0123456789ab");
    host(other.substr(0, 30));
    mDone[0](std::nullopt);
    EXPECT_EQ(other.substr(0, 30), mToGuest);
    host(other.substr(30));
    EXPECT_EQ(other + open(kGzipService), mToGuest);
    EXPECT_TRUE(mToHost.empty());
}

TEST_F(AdbHostScreencapTest, waitsForGuestPacketBoundary) {
    connect();
    host(open(kGzipService));
    const std::string other = packet(ADB_WRTE, 3, kHostId + 1, "data");
    guest(other.substr(0, 10));
    mDone[0](std::string("frame"));
    EXPECT_EQ(other.substr(0, 10), mToHost);
    guest(other.substr(10) + other);
    const uint32_t id = headerAt(mToHost, other.size()).arg0;
    EXPECT_EQ(other + packet(ADB_OKAY, id, kHostId) + packet(ADB_WRTE, id, kHostId, "frame") + other,
              mToHost);
}

TEST_F(AdbHostScreencapTest, dropsReplyForStreamClosedByHost) {
    connect();
    const uint32_t id = answer(std::string(kMaxPayload * 2, 'x'));
    mToHost.clear();
    host(packet(ADB_CLSE, kHostId, id));
    host(packet(ADB_OKAY, kHostId, id));
    EXPECT_TRUE(mToHost.empty());
    EXPECT_TRUE(mToGuest.empty());
}

TEST_F(AdbHostScreencapTest, ignoresReplyAfterDestruction) {
    std::string toHost;
    std::function<void(AdbHostScreencap::Reply)> done;
    {
        AdbHostScreencap screencap(
                [](const char*, size_t) {},
                [&toHost](const char* data, size_t size) { toHost.append(data, size); },
                [&done](const AdbHostScreencap::Request&,
                        std::function<void(AdbHostScreencap::Reply)> d) { done = std::move(d); });
        const std::string fromHost = packet(ADB_CNXN, A_VERSION, 1 << 20) + open(kGzipService);
        const std::string fromGuest = packet(ADB_CNXN, A_VERSION, kMaxPayload);
        screencap.onGuestData(fromGuest.data(), fromGuest.size());
        screencap.onHostData(fromHost.data(), fromHost.size());
    }
    ASSERT_TRUE(done);
    const std::string before = toHost;
    done(std::string("frame"));
    EXPECT_EQ(before, toHost);
}

TEST_F(AdbHostScreencapTest, stopsParsingEncryptedTransport) {
    host(packet(ADB_CNXN, A_VERSION, 1 << 20));
    const std::string stls = packet(0x534c5453, A_VERSION, 0);
    guest(stls);
    // A handshake that happens to look like the start of a packet.
    const std::string handshake = open(kGzipService).substr(0, 20);
    host(stls.substr(0, 5));
    host(stls.substr(5) + handshake);
    guest(handshake);
    EXPECT_EQ(packet(ADB_CNXN, A_VERSION, 1 << 20) + stls + handshake, mToGuest);
    EXPECT_EQ(stls + handshake, mToHost);
    EXPECT_TRUE(mDone.empty());
}

}  // namespace
}  // namespace emulation
}  // namespace android
