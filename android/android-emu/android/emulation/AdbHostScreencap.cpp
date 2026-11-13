// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "android/emulation/AdbHostScreencap.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/sockets/ScopedSocket.h"
#include "aemu/base/sockets/SocketUtils.h"
#include "aemu/base/threads/WorkerThread.h"
#include "android/console.h"
#include "host-common/VmLock.h"
#include "host-common/multi_display_agent.h"
#include "host-common/opengles.h"
#include "render-utils/Renderer.h"

namespace android {
namespace emulation {
namespace {

constexpr uint32_t kAdbStls = 0x534c5453;
constexpr uint32_t kMaxPayload = 1024 * 1024;
constexpr size_t kMaxServiceSize = 128;
constexpr size_t kMaxStreams = 4;
// The guest daemon numbers its streams from 1 upwards, so ids from here on
// are free for the streams answered on its behalf.
constexpr uint32_t kFirstLocalId = 0xf0000000;
constexpr size_t kScreencapHeaderSize = 16;

std::mutex sDisplayLock;
std::optional<AdbHostScreencap::GuestDisplay> sDisplay;

bool isValid(const amessage& header) {
    return header.magic == (header.command ^ 0xffffffff);
}

std::string_view bytesOf(const amessage& header, size_t size = sizeof(amessage)) {
    return {reinterpret_cast<const char*>(&header), size};
}

// Removes a decimal number of up to |maxValue| from the front of |text|.
std::optional<int> takeNumber(std::string_view* text, int maxValue) {
    int value = 0;
    size_t digits = 0;
    while (digits < text->size() && digits < 5 && (*text)[digits] >= '0' &&
           (*text)[digits] <= '9') {
        value = value * 10 + ((*text)[digits++] - '0');
    }
    if (digits == 0 || value > maxValue || (digits > 1 && text->front() == '0')) {
        return std::nullopt;
    }
    text->remove_prefix(digits);
    return value;
}

// Returns true if the host applies a color transform to display 0, such as
// night light, color inversion or the black of a display that is off. Like the
// renderer, it takes a transform that cannot be read as no transform.
bool hasColorTransform() {
    float matrix[16];
    if (getConsoleAgents()->multi_display->getDisplayColorTransform(0, matrix) != 0) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (std::abs(matrix[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-6f) {
            return true;
        }
    }
    return false;
}

// Plays the part of `nc -w 3`: writes |data| to a TCP port on the host's
// loopback. Returns false if nothing listens there.
bool sendToLoopback(int port, const std::string& data) {
    base::ScopedSocket socket(base::socketTcp4LoopbackClient(port));
    if (!socket.valid()) {
        return false;
    }
#ifdef _WIN32
    const DWORD timeout = 3000;
#else
    const timeval timeout = {3, 0};
#endif
    setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    base::socketSendAll(socket.get(), data.data(), data.size());
    return true;
}

}  // namespace

// static
void AdbHostScreencap::setGuestDisplay(std::optional<GuestDisplay> display) {
    std::lock_guard<std::mutex> lock(sDisplayLock);
    sDisplay = display;
}

// static
std::optional<AdbHostScreencap::Request> AdbHostScreencap::parse(std::string_view service) {
    static const bool enabled = [] {
        const char* value = std::getenv("KEMU_ADB_HOST_SCREENCAP");
        return !value || strcmp(value, "0");
    }();
    while (!service.empty() && service.back() == '\0') {
        service.remove_suffix(1);
    }
    constexpr std::string_view kPipeline = "exec:screencap | ";
    if (!enabled || service.substr(0, kPipeline.size()) != kPipeline) {
        return std::nullopt;
    }
    service.remove_prefix(kPipeline.size());
    if (service == "gzip -1") {
        return Request{Request::Output::Gzip, 0};
    }
    // Clients take the address of the host from the guest's neighbour table,
    // where the gateway, the name servers and the host all sit in 10.0.2.0/24.
    // The port is tried on the host's loopback whichever of them was picked.
    constexpr std::string_view kNetcat = "nc -w 3 10.0.2.";
    if (service.substr(0, kNetcat.size()) != kNetcat) {
        return std::nullopt;
    }
    service.remove_prefix(kNetcat.size());
    if (!takeNumber(&service, 255) || service.empty() || service.front() != ' ') {
        return std::nullopt;
    }
    service.remove_prefix(1);
    const auto port = takeNumber(&service, 65535);
    if (!port || *port == 0 || !service.empty()) {
        return std::nullopt;
    }
    return Request{Request::Output::Netcat, *port};
}

// static
std::string AdbHostScreencap::capture() {
    std::optional<GuestDisplay> display;
    {
        std::lock_guard<std::mutex> lock(sDisplayLock);
        display = sDisplay;
    }
    const gfxstream::RendererPtr renderer = android_getOpenglesRenderer();
    // The host frame includes the display's color transform, which screenshots
    // in the guest leave out. It is checked on both sides of the readback.
    if (!display || !renderer || hasColorTransform()) {
        return {};
    }

    // The host rotation that shows the guest's logical display upright.
    const int rotation = (4 - display->rotation) % 4;
    std::string frame =
            capture(*display, [&renderer, rotation](unsigned int* width, unsigned int* height,
                                                    uint8_t* pixels, size_t* size) {
                return renderer->getScreenshot(4, width, height, pixels, size, 0, 0, 0,
                                               rotation);
            });
    if (hasColorTransform()) {
        return {};
    }
    return frame;
}

// static
std::string AdbHostScreencap::capture(const GuestDisplay& display, const Screenshot& screenshot) {
    unsigned int width = 0;
    unsigned int height = 0;
    size_t size = 0;
    if (screenshot(&width, &height, nullptr, &size) !=
        gfxstream::Renderer::GET_SCREENSHOT_RESULT_PIXELS_SIZE) {
        return {};
    }
    std::string out(kScreencapHeaderSize + size, '\0');
    if (screenshot(&width, &height, reinterpret_cast<uint8_t*>(&out[kScreencapHeaderSize]),
                   &size) != 0 ||
        static_cast<int>(width) != display.width ||
        static_cast<int>(height) != display.height || size != size_t{4} * width * height) {
        return {};
    }
    // The frame can have shrunk since the size was queried.
    out.resize(kScreencapHeaderSize + size);

    // screencap starts with the width, the height, the pixel format
    // (RGBA_8888) and the colour space (sRGB).
    const uint32_t header[] = {width, height, 1, 1};
    static_assert(sizeof(header) == kScreencapHeaderSize);
    memcpy(&out[0], header, sizeof(header));
    // The display is opaque.
    for (size_t i = kScreencapHeaderSize + 3; i < out.size(); i += 4) {
        out[i] = '\xff';
    }
    return out;
}

// static
std::string AdbHostScreencap::gzip(std::string_view data) {
    // Blocks are compressed independently on several threads and joined into
    // one deflate stream: every block but the last ends on a byte boundary
    // without closing the stream.
    constexpr size_t kBlockSize = 1 << 20;
    const size_t blocks = std::max<size_t>(1, (data.size() + kBlockSize - 1) / kBlockSize);
    std::vector<std::string> parts(blocks);
    std::vector<uLong> checksums(blocks);
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};

    const auto compress = [&] {
        for (size_t i; (i = next.fetch_add(1)) < blocks;) {
            const std::string_view in = data.substr(i * kBlockSize, kBlockSize);
            const bool last = i + 1 == blocks;
            z_stream z = {};
            if (deflateInit2(&z, 1, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                failed = true;
                return;
            }
            std::string& out = parts[i];
            out.resize(deflateBound(&z, static_cast<uLong>(in.size())) + 16);
            z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
            z.avail_in = static_cast<uInt>(in.size());
            z.next_out = reinterpret_cast<Bytef*>(&out[0]);
            z.avail_out = static_cast<uInt>(out.size());
            int result;
            do {
                if (z.avail_out == 0) {
                    const size_t used = out.size();
                    out.resize(used * 2);
                    z.next_out = reinterpret_cast<Bytef*>(&out[used]);
                    z.avail_out = static_cast<uInt>(used);
                }
                result = deflate(&z, last ? Z_FINISH : Z_SYNC_FLUSH);
            } while (result == Z_OK && (last || z.avail_in != 0 || z.avail_out == 0));
            if (result != (last ? Z_STREAM_END : Z_OK)) {
                failed = true;
            }
            out.resize(z.total_out);
            deflateEnd(&z);
            checksums[i] = crc32(0, reinterpret_cast<const Bytef*>(in.data()),
                                 static_cast<uInt>(in.size()));
        }
    };
    std::vector<std::thread> threads(
            std::min<size_t>(blocks, std::clamp(std::thread::hardware_concurrency(), 1u, 8u)) - 1);
    for (auto& thread : threads) {
        thread = std::thread(compress);
    }
    compress();
    for (auto& thread : threads) {
        thread.join();
    }
    if (failed) {
        return {};
    }

    // Header: magic, deflate, no flags, no time, fastest compression, Unix.
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x04\x03", 10);
    uLong checksum = 0;
    for (size_t i = 0; i < blocks; i++) {
        out += parts[i];
        checksum = crc32_combine(checksum, checksums[i],
                                 static_cast<z_off_t>(data.substr(i * kBlockSize, kBlockSize).size()));
    }
    const uint32_t trailer[] = {static_cast<uint32_t>(checksum), static_cast<uint32_t>(data.size())};
    out.append(reinterpret_cast<const char*>(trailer), sizeof(trailer));
    return out;
}

// static
void AdbHostScreencap::serve(const Request& request, std::function<void(Reply)> done) {
    using Job = std::function<void()>;
    // Never joined: a frame may still be on its way out when the process ends.
    static auto* const worker = [] {
        auto* thread = new base::WorkerThread<Job>([](Job&& job) {
            job();
            return base::WorkerProcessingResult::Continue;
        });
        thread->start();
        return thread;
    }();
    worker->enqueue([request, done = std::move(done)]() mutable {
        Reply reply;
        const std::string frame = capture();
        if (frame.empty()) {
            // Left to the guest.
        } else if (request.output == Request::Output::Netcat) {
            if (sendToLoopback(request.port, frame)) {
                reply.emplace();
            }
        } else if (std::string compressed = gzip(frame); !compressed.empty()) {
            reply = std::move(compressed);
        }
        base::ThreadLooper::runOnMainLooper(
                [done = std::move(done), reply = std::move(reply)]() mutable {
                    android::RecursiveScopedVmLock lock;
                    done(std::move(reply));
                });
    });
}

AdbHostScreencap::AdbHostScreencap(Sink toGuest, Sink toHost, Server server)
    : mToGuest(std::move(toGuest)), mToHost(std::move(toHost)), mServer(std::move(server)) {}

void AdbHostScreencap::onHostData(const char* data, size_t size) {
    if (mOpaque) {
        mToGuest(data, size);
        return;
    }
    std::string relayed;
    while (size != 0) {
        if (mHostHeaderSize < sizeof(mHostHeader)) {
            const size_t n = std::min(sizeof(mHostHeader) - mHostHeaderSize, size);
            memcpy(reinterpret_cast<char*>(&mHostHeader) + mHostHeaderSize, data, n);
            mHostHeaderSize += n;
            data += n;
            size -= n;
            if (mHostHeaderSize < sizeof(mHostHeader)) {
                break;
            }
            if (!isValid(mHostHeader) || mHostHeader.command == kAdbStls) {
                stopParsing();
                relayed.append(bytesOf(mHostHeader));
                relayed.append(data, size);
                break;
            }
            mHostPacket = classify(mHostHeader);
            mHostPayloadLeft = mHostHeader.data_length;
            if (mHostPacket == HostPacket::Relay) {
                relayed.append(bytesOf(mHostHeader));
            } else if (mHostPacket == HostPacket::Hold) {
                mHeld.assign(bytesOf(mHostHeader));
            }
        }
        const size_t n = std::min<size_t>(mHostPayloadLeft, size);
        if (mHostPacket == HostPacket::Relay) {
            relayed.append(data, n);
        } else if (mHostPacket == HostPacket::Hold) {
            mHeld.append(data, n);
        }
        mHostPayloadLeft -= static_cast<uint32_t>(n);
        data += n;
        size -= n;
        if (mHostPayloadLeft == 0) {
            finishHostPacket(&relayed);
            mHostHeaderSize = 0;
            relayed.append(mGuestQueue);
            mGuestQueue.clear();
        }
    }
    if (!relayed.empty()) {
        mToGuest(relayed.data(), relayed.size());
    }
}

void AdbHostScreencap::onGuestData(const char* data, size_t size) {
    if (mOpaque) {
        mToHost(data, size);
        return;
    }
    const char* relayed = data;
    while (size != 0) {
        if (mGuestHeaderSize < sizeof(mGuestHeader)) {
            const size_t n = std::min(sizeof(mGuestHeader) - mGuestHeaderSize, size);
            memcpy(reinterpret_cast<char*>(&mGuestHeader) + mGuestHeaderSize, data, n);
            mGuestHeaderSize += n;
            data += n;
            size -= n;
            if (mGuestHeaderSize < sizeof(mGuestHeader)) {
                break;
            }
            if (!isValid(mGuestHeader) || mGuestHeader.command == kAdbStls) {
                stopParsing();
                data += size;
                break;
            }
            if (mGuestHeader.command == ADB_CNXN) {
                mGuestVersion = mGuestHeader.arg0;
                mGuestMaxPayload = mGuestHeader.arg1;
            }
            mGuestPayloadLeft = mGuestHeader.data_length;
        }
        const size_t n = std::min<size_t>(mGuestPayloadLeft, size);
        mGuestPayloadLeft -= static_cast<uint32_t>(n);
        data += n;
        size -= n;
        if (mGuestPayloadLeft == 0) {
            mGuestHeaderSize = 0;
            if (!mHostQueue.empty()) {
                mToHost(relayed, data - relayed);
                relayed = data;
                mToHost(mHostQueue.data(), mHostQueue.size());
                mHostQueue.clear();
            }
        }
    }
    if (data != relayed) {
        mToHost(relayed, data - relayed);
    }
}

AdbHostScreencap::HostPacket AdbHostScreencap::classify(const amessage& header) {
    switch (header.command) {
        case ADB_CNXN:
            mHostVersion = header.arg0;
            mHostMaxPayload = header.arg1;
            return HostPacket::Relay;
        case ADB_OPEN:
            // A stream can only be answered once both sides have announced
            // their limits. A second argument asks for delayed
            // acknowledgements, which are left to the guest daemon.
            if (mHostMaxPayload != 0 && mGuestMaxPayload != 0 && header.arg1 == 0 &&
                header.data_length != 0 && header.data_length <= kMaxServiceSize &&
                mStreams.size() < kMaxStreams) {
                return HostPacket::Hold;
            }
            return HostPacket::Relay;
        case ADB_OKAY:
        case ADB_WRTE:
        case ADB_CLSE:
            return header.arg1 >= kFirstLocalId ? HostPacket::Local : HostPacket::Relay;
        default:
            return HostPacket::Relay;
    }
}

void AdbHostScreencap::finishHostPacket(std::string* relayed) {
    if (mHostPacket == HostPacket::Local) {
        onLocalPacket(mHostHeader);
        return;
    }
    if (mHostPacket != HostPacket::Hold) {
        return;
    }
    const auto request = parse(std::string_view(mHeld).substr(sizeof(amessage)));
    if (!request) {
        relayed->append(mHeld);
        mHeld.clear();
        return;
    }
    uint32_t id;
    do {
        id = kFirstLocalId + (mNextId++ & 0x0fffffff);
    } while (mStreams.count(id));
    Stream& stream = mStreams[id];
    stream.hostId = mHostHeader.arg0;
    stream.open = std::move(mHeld);
    mHeld.clear();
    mServer(*request, [this, id, alive = std::weak_ptr<bool>(mAlive)](Reply reply) {
        if (!alive.expired()) {
            onReply(id, std::move(reply));
        }
    });
}

void AdbHostScreencap::onLocalPacket(const amessage& header) {
    const auto it = mStreams.find(header.arg1);
    // Packets for a finished stream, such as the CLSE answering ours, need no
    // reply.
    if (it == mStreams.end() || it->second.hostId != header.arg0) {
        return;
    }
    const bool answered = it->second.open.empty();
    if (header.command == ADB_CLSE) {
        mStreams.erase(it);
    } else if (!answered) {
        // The host has nothing to acknowledge or send before the reply.
    } else if (header.command == ADB_OKAY) {
        advance(header.arg1);
    } else {
        sendToHost(ADB_OKAY, header.arg1, header.arg0);
    }
}

void AdbHostScreencap::onReply(uint32_t id, Reply reply) {
    const auto it = mStreams.find(id);
    if (it == mStreams.end()) {
        // The host has closed the stream in the meantime.
        return;
    }
    if (!reply) {
        const std::string open = std::move(it->second.open);
        mStreams.erase(it);
        sendToGuest(open);
        return;
    }
    it->second.open.clear();
    it->second.data = std::move(*reply);
    sendToHost(ADB_OKAY, id, it->second.hostId);
    advance(id);
}

// Sends the next part of the reply, or closes the stream once the host has
// acknowledged all of it.
void AdbHostScreencap::advance(uint32_t id) {
    Stream& stream = mStreams[id];
    if (stream.sent == stream.data.size()) {
        sendToHost(ADB_CLSE, id, stream.hostId);
        mStreams.erase(id);
        return;
    }
    const size_t n = std::min<size_t>(stream.data.size() - stream.sent,
                                      std::min({mHostMaxPayload, mGuestMaxPayload, kMaxPayload}));
    sendToHost(ADB_WRTE, id, stream.hostId, std::string_view(stream.data).substr(stream.sent, n));
    stream.sent += n;
}

// Gives up on telling packets apart, after which both streams are relayed as
// they come. Called when a stream is not valid ADB or is about to be encrypted.
void AdbHostScreencap::stopParsing() {
    mOpaque = true;
    mStreams.clear();
    mGuestQueue.clear();
    mHostQueue.clear();
    // Pass on what was held back from the guest while it was being examined.
    if (mHostHeaderSize < sizeof(mHostHeader)) {
        mHeld.assign(bytesOf(mHostHeader, mHostHeaderSize));
    } else if (mHostPacket != HostPacket::Hold) {
        mHeld.clear();
    }
    if (!mHeld.empty()) {
        mToGuest(mHeld.data(), mHeld.size());
        mHeld.clear();
    }
}

void AdbHostScreencap::sendToGuest(const std::string& bytes) {
    if (mHostHeaderSize == 0) {
        mToGuest(bytes.data(), bytes.size());
    } else {
        mGuestQueue.append(bytes);
    }
}

void AdbHostScreencap::sendToHost(uint32_t command,
                                  uint32_t localId,
                                  uint32_t hostId,
                                  std::string_view payload) {
    amessage header;
    header.command = command;
    header.arg0 = localId;
    header.arg1 = hostId;
    header.data_length = static_cast<unsigned>(payload.size());
    header.magic = command ^ 0xffffffff;
    if (std::min(mHostVersion, mGuestVersion) < A_VERSION_SKIP_CHECKSUM) {
        for (const unsigned char byte : payload) {
            header.data_check += byte;
        }
    }
    if (mGuestHeaderSize != 0) {
        mHostQueue.append(bytesOf(header));
        mHostQueue.append(payload);
        return;
    }
    mToHost(bytesOf(header).data(), sizeof(header));
    if (!payload.empty()) {
        mToHost(payload.data(), payload.size());
    }
}

}  // namespace emulation
}  // namespace android
