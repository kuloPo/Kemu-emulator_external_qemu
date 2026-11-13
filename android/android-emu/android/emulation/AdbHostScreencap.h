// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "android/emulation/apacket_utils.h"

namespace android {
namespace emulation {

// Sits on an ADB transport between the host server and the guest daemon and
// answers screenshot pipelines from the host's copy of the display, so that a
// client polling `screencap` does not pay for a guest capture, compression and
// transfer on every frame. All other traffic is relayed unchanged.
//
// Only the services accepted by parse() are answered, and only while the host
// frame is known to equal what the guest's screencap would print: the guest
// must have reported its display state, the host frame must have the reported
// size, and the host must not apply a color transform to the display. In
// every other case the stream is handed to the guest as if this class was not
// there.
//
// An instance must be used and destroyed with the VM lock held.
class AdbHostScreencap {
public:
    // State of the guest's default display.
    struct GuestDisplay {
        int rotation = 0;  // Surface.ROTATION_*
        int width = 0;     // Logical size in that rotation.
        int height = 0;
    };

    // Publishes the display state reported by the guest, or nullopt when the
    // guest is no longer observed. Thread safe.
    static void setGuestDisplay(std::optional<GuestDisplay> display);

    struct Request {
        enum class Output { Netcat, Gzip };
        Output output = Output::Gzip;
        int port = 0;  // Netcat only.
    };

    // Returns the request if |service| names a screenshot pipeline that can be
    // answered from the host.
    static std::optional<Request> parse(std::string_view service);

    // The bytes the stream should carry, or nullopt to let the guest serve it.
    using Reply = std::optional<std::string>;
    using Sink = std::function<void(const char* data, size_t size)>;
    // Produces the reply to a request. |done| must be called exactly once with
    // the VM lock held; it may be called after the interceptor is destroyed.
    using Server = std::function<void(const Request& request,
                                      std::function<void(Reply)> done)>;

    // |toGuest| and |toHost| receive the bytes to pass on to each side.
    AdbHostScreencap(Sink toGuest, Sink toHost, Server server);

    void onHostData(const char* data, size_t size);
    void onGuestData(const char* data, size_t size);

    // The Server used on a live transport. Works on a separate thread.
    static void serve(const Request& request, std::function<void(Reply)> done);

    // Returns what `screencap` prints for the current frame, or an empty
    // string if the host frame cannot stand in for it.
    static std::string capture();

    // Reads the host frame as Renderer::getScreenshot() does for one display
    // and rotation: with null |pixels| it only reports the size it needs.
    using Screenshot = std::function<int(unsigned int* width, unsigned int* height,
                                         uint8_t* pixels, size_t* size)>;

    // Returns what `screencap` prints for the frame |screenshot| reads, or an
    // empty string if that frame does not have the size of |display|.
    static std::string capture(const GuestDisplay& display, const Screenshot& screenshot);

    // Returns |data| as a single gzip member.
    static std::string gzip(std::string_view data);

private:
    enum class HostPacket { Relay, Hold, Local };

    struct Stream {
        uint32_t hostId = 0;
        std::string open;  // The OPEN packet, until the stream is answered.
        std::string data;
        size_t sent = 0;
    };

    HostPacket classify(const amessage& header);
    void finishHostPacket(std::string* relayed);
    void onLocalPacket(const amessage& header);
    void onReply(uint32_t id, Reply reply);
    void advance(uint32_t id);
    void stopParsing();
    void sendToGuest(const std::string& bytes);
    void sendToHost(uint32_t command, uint32_t localId, uint32_t hostId,
                    std::string_view payload = {});

    const Sink mToGuest;
    const Sink mToHost;
    const Server mServer;

    // Set once the byte streams can no longer be split into packets.
    bool mOpaque = false;

    amessage mHostHeader;
    size_t mHostHeaderSize = 0;
    uint32_t mHostPayloadLeft = 0;
    HostPacket mHostPacket = HostPacket::Relay;
    std::string mHeld;
    // Packets waiting for the host stream to reach a packet boundary.
    std::string mGuestQueue;

    amessage mGuestHeader;
    size_t mGuestHeaderSize = 0;
    uint32_t mGuestPayloadLeft = 0;
    // Packets waiting for the guest stream to reach a packet boundary.
    std::string mHostQueue;

    uint32_t mHostVersion = 0;
    uint32_t mHostMaxPayload = 0;
    uint32_t mGuestVersion = 0;
    uint32_t mGuestMaxPayload = 0;

    std::unordered_map<uint32_t, Stream> mStreams;
    uint32_t mNextId = 0;
    // Expires with the instance; guards replies that arrive later.
    const std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
};

}  // namespace emulation
}  // namespace android
