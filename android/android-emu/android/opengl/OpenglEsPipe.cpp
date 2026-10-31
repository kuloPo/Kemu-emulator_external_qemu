// Copyright 2016 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#include "android/opengl/OpenglEsPipe.h"


#include "aemu/base/Optional.h"
#include "aemu/base/async/Looper.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/StreamSerializing.h"
#include "aemu/base/threads/FunctorThread.h"
#include "android/console.h"
#include "android/hw-sensors.h"
#include "android/loadpng.h"
#include "android/opengl/GLProcessPipe.h"
#include "android/opengles-pipe.h"
#include "android/emulation/SharedReceiveQueue.h"
#include "android/emulation/SharedTransmitQueue.h"
#include "android/emulation/PipeDoorbellConsumer.h"
#include "host-common/opengles.h"
#include "android/snapshot/Loader.h"
#include "android/snapshot/Saver.h"
#include "android/snapshot/Snapshotter.h"
#include "android/snapshot/GfxstreamStreamAdapter.h"
#include "android/emulation/control/ScreenCapturer.h"

#include <atomic>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Set to 1 or 2 for debug traces
#define DEBUG 0

#if DEBUG >= 1
#define D(...) dprint(__VA_ARGS__), fflush(stdout)
#else
#define D(...) ((void)0)
#endif

#if DEBUG >= 2
#define DD(...) dprint(__VA_ARGS__), fflush(stdout)
#else
#define DD(...) ((void)0)
#endif

using ChannelBuffer = gfxstream::RenderChannel::Buffer;
using gfxstream::RenderChannel;
using gfxstream::RenderChannelPtr;
using ChannelState = gfxstream::RenderChannel::State;
using IoResult = gfxstream::RenderChannel::IoResult;
using android::snapshot::Snapshotter;


// Note: this is only used for a validation check, and mismatches would
// crash the emulator with a fatal error. For updating the snapshot file
// versions, make sure 'kVersion' in Snapshot.cpp is updated as that will
// be used to check if a snapshot file can be loaded safely.
#define OPENGL_SAVE_VERSION 2

namespace android {
namespace opengl {

// TODO (b/138549350): See if Android/Fuchsia pipe protocols can be unified
// to give the best performance in each guest OS.
enum class RecvMode {
    Android = 0,
    Fuchsia = 1,
    VirtioGpu = 2,
};

static RecvMode recvMode = RecvMode::Android;

namespace {

class EmuglPipe : public AndroidPipe {
public:

    //////////////////////////////////////////////////////////////////////////
    // The pipe service class for this implementation.
    class Service : public AndroidPipe::Service {
    public:
        Service() : AndroidPipe::Service("opengles") {}

        // Create a new EmuglPipe instance.
        AndroidPipe* create(void* hwPipe, const char* args, AndroidPipeFlags flags) override {
            return createPipe(hwPipe, this, args, flags);
        }

        bool canLoad() const override { return true; }

        virtual void preLoad(android::base::Stream* stream) override {
            const bool hasRenderer = stream->getByte();
            const auto& renderer = android_getOpenglesRenderer();
            if (hasRenderer != (bool)renderer) {
                // die?
                return;
            }
            if (!hasRenderer) {
                return;
            }
            const int version = stream->getBe32();
            if (version != OPENGL_SAVE_VERSION) {
                dfatal("Snapshot file version is not compatible!");
            }
            android::snapshot::GfxstreamStreamAdapter gfxstreamStream(stream);
            if (!renderer->load(&gfxstreamStream, Snapshotter::get().loader().textureLoader())) {
                // Should not try to continue loading the snapshot
                dfatal("Failed to load snapshot state for graphics!");
            }
        }

        void postLoad(android::base::Stream* stream) override {
            if (const auto& renderer = android_getOpenglesRenderer()) {
                renderer->resumeAll();
            }
        }

        void preSave(android::base::Stream* stream) override {
            if (const auto& renderer = android_getOpenglesRenderer()) {
                renderer->pauseAllPreSave();
                stream->putByte(1); // hasRenderer
                stream->putBe32(OPENGL_SAVE_VERSION);

                android::snapshot::GfxstreamStreamAdapter gfxstreamStream(stream);
                renderer->save(&gfxstreamStream,
                               Snapshotter::get().saver().textureSaver());

                writeScreenshot(*renderer);
            } else {
                stream->putByte(0);
            }
        }

        void postSave(android::base::Stream* stream) override {
            if (const auto& renderer = android_getOpenglesRenderer()) {
                renderer->resumeAll();
            }
        }

        virtual AndroidPipe* load(void* hwPipe,
                                  const char* args,
                                  android::base::Stream* stream) override {
            return createPipe(hwPipe, this, args, ANDROID_PIPE_DEFAULT, stream);
        }

    private:
        static AndroidPipe* createPipe(
                void* hwPipe,
                Service* service,
                const char* args,
                AndroidPipeFlags flags,
                android::base::Stream* loadStream = nullptr) {
            const auto& renderer = android_getOpenglesRenderer();
            if (!renderer) {
                // This should never happen, unless there is a bug in the
                // emulator's initialization, or the system image, or we're
                // loading from an incompatible snapshot.
                D("Trying to open the OpenGLES pipe without GPU emulation!");
                return nullptr;
            }

            auto pipe = new EmuglPipe(hwPipe, service, renderer, flags, loadStream);
            if (!pipe->mIsWorking) {
                delete pipe;
                pipe = nullptr;
            }
            return pipe;
        }

        void writeScreenshot(gfxstream::Renderer& renderer) {
            if (!mSnapshotCallbackRegistered) {
                // We have to wait for the screenshot saving thread, but
                // there's no need to join it too soon: it is ok to only
                // block when the rest of snapshot saving is complete.
                Snapshotter::get().addOperationCallback(
                        [this](Snapshotter::Operation op,
                               Snapshotter::Stage stage) {
                            if (op == Snapshotter::Operation::Save &&
                                stage == Snapshotter::Stage::End) {
                                if (mScreenshotSaver) {
                                    mScreenshotSaver->wait();
                                    mScreenshotSaver.clear();
                                }
                            }
                        });
                mSnapshotCallbackRegistered = true;
            }
            // always do 4 channel screenshot because swiftshader_indirect
            // has issues with 3 channels
            const unsigned int nChannels = 4;
            unsigned int width = 0;
            unsigned int height = 0;
            size_t cPixels = 0;
            std::vector<unsigned char> pixels;

            int displayId = 0;
            if (android_foldable_is_pixel_fold() && android_foldable_is_folded()) {
                displayId = android_foldable_pixel_fold_second_display_id();
            }
            // Fetch size of buffer.
            int screenshotRes = renderer.getScreenshot(
                    nChannels, &width, &height, nullptr, &cPixels, displayId);
            if (screenshotRes ==
                gfxstream::Renderer::GET_SCREENSHOT_RESULT_PIXELS_SIZE) {
                pixels.resize(cPixels);
                screenshotRes = renderer.getScreenshot(nChannels, &width,
                                                       &height, pixels.data(),
                                                       &cPixels, displayId);
            }
            if (screenshotRes != 0) {
                derror("Could not take screenshot, error: %d", screenshotRes);
                width = 0;
                height = 0;
            }

            if (width > 0 && height > 0) {
                std::string dataDir(Snapshotter::get().saver().snapshot().dataDir());
                mScreenshotSaver.emplace([nChannels, width, height,
                                          dataDir = std::move(dataDir),
                                          pixels = std::move(pixels)] {
                    std::string fileName = android::base::PathUtils::join(
                            dataDir, "screenshot.png");
                    // TODO: fix the screenshot rotation?
                    savepng(fileName.c_str(), nChannels, width, height,
                            SKIN_ROTATION_0,
                            const_cast<unsigned char*>(pixels.data()));
                });
                mScreenshotSaver->start();
            }
        }

        bool mSnapshotCallbackRegistered = false;
        base::Optional<base::FunctorThread> mScreenshotSaver;
    };

    /////////////////////////////////////////////////////////////////////////
    // Constructor, check that |mIsWorking| is true after this call to verify
    // that everything went well.
    EmuglPipe(void* hwPipe,
              Service* service,
              const gfxstream::RendererPtr& renderer,
              AndroidPipeFlags flags,
              android::base::Stream* loadStream = nullptr)
        : AndroidPipe(hwPipe, service) {
        bool isWorking = true;
        if (loadStream) {
            DD("%s: loading GLES pipe state for hwpipe=%p", __func__, mHwPipe);
            isWorking = (bool)loadStream->getBe32();
            android::base::loadBuffer(loadStream, &mDataForReading);
            mDataForReadingLeft = loadStream->getBe32();
        }

        uint32_t virtioGpuContextId = std::numeric_limits<uint32_t>::max();
        if (flags & ANDROID_PIPE_VIRTIO_GPU_BIT) {
            virtioGpuContextId = (uint32_t)(uintptr_t)hwPipe;
        }

        gfxstream::Stream* gfxstreamStream = nullptr;
        std::optional<android::snapshot::GfxstreamStreamAdapter> gfxstreamStreamAdapter;
        if (loadStream) {
            gfxstreamStreamAdapter.emplace(loadStream);
            gfxstreamStream = &*gfxstreamStreamAdapter;
        }

        mChannel = renderer->createRenderChannel(gfxstreamStream, virtioGpuContextId);
        if (!mChannel) {
            D("Failed to create an OpenGLES pipe channel!");
            return;
        }

        mIsWorking = isWorking;
        mChannel->setEventCallback([this](RenderChannel::State events) {
            onChannelHostEvent(events);
        });
    }

    //////////////////////////////////////////////////////////////////////////
    // Overriden AndroidPipe methods

    int setupSharedReceive(PipeRxHost* host, PipeRxGuest* guest, PipeRxDirty dirty,
                           bool restoring, void (*enter)(), void (*leave)()) {
        if (std::atomic_load(&mSharedReceive) || !mChannel || mDataForReadingLeft) return -1;
        mConsumerEnter = enter;
        mConsumerLeave = leave;
        std::atomic_store(&mSharedReceive, std::make_shared<android::SharedReceiveQueue>(
                host, guest, dirty, [this] { signalWake(PIPE_WAKE_READ); }, restoring));
        return 0;
    }

    int addSharedSegment(unsigned id, void* const* pages) {
        auto queue = std::atomic_load(&mSharedReceive);
        return queue && queue->add(id, pages) ? 0 : -1;
    }

    int activateSharedReceive() {
        auto queue = std::atomic_load(&mSharedReceive);
        if (!queue || !queue->activate()) return -1;
        auto enter = mConsumerEnter;
        auto leave = mConsumerLeave;
        mChannel->setReceiveSink([queue, enter, leave](const void* data, size_t size) {
            registerConsumerThread(enter, leave);
            return queue->write(data, size);
        });
        return 0;
    }

    void creditSharedReceive() {
        if (std::atomic_load(&mSharedReceive)) mChannel->notifyGuestWrite();
    }

    void setReceiveRunning(bool running) {
        if (auto queue = std::atomic_load(&mSharedReceive)) queue->setRunning(running);
        if (running) mChannel->notifyGuestWrite();
    }

    void armSharedReceive(uint32_t sequence) {
        auto queue = std::atomic_load(&mSharedReceive);
        if (queue) queue->arm(sequence);
    }

    void* guestWriteEvent() const { return mChannel->guestWriteEvent(); }

    int setupSharedTransmit(PipeTxGuest* guest, PipeTxHost* host, PipeRxDirty dirty,
                            bool restoring, void (*enter)(), void (*leave)()) {
        if (std::atomic_load(&mSharedTransmit) || !mChannel) return -1;
        mConsumerEnter = enter;
        mConsumerLeave = leave;
        std::atomic_store(&mSharedTransmit, std::make_shared<android::SharedTransmitQueue>(
                guest, host, dirty, [this] { signalWake(PIPE_WAKE_WRITE); }, restoring));
        return 0;
    }

    int addTransmitSegment(unsigned id, void* const* pages, unsigned count) {
        auto queue = std::atomic_load(&mSharedTransmit);
        return queue && queue->add(id, pages, count) ? 0 : -1;
    }

    int activateSharedTransmit() {
        auto queue = std::atomic_load(&mSharedTransmit);
        if (!queue || !queue->activate()) return -1;
        auto enter = mConsumerEnter;
        auto leave = mConsumerLeave;
        mChannel->setGuestMemorySource([queue, enter, leave](void* destination, size_t size) {
            registerConsumerThread(enter, leave);
            return queue->read(destination, size);
        });
        return 0;
    }

    void kickSharedTransmit() { mChannel->notifyGuestWrite(); }

    void setTransmitRunning(bool running) {
        if (auto queue = std::atomic_load(&mSharedTransmit)) queue->setRunning(running);
        if (running) mChannel->notifyGuestWrite();
    }

    void armSharedTransmit(uint32_t sequence) {
        if (auto queue = std::atomic_load(&mSharedTransmit)) queue->arm(sequence);
    }

    void detachSharedTransmit() {
        mChannel->setGuestMemorySource({});
        if (auto queue = std::atomic_exchange(&mSharedTransmit,
                std::shared_ptr<android::SharedTransmitQueue>{})) queue->close();
    }

    static void registerConsumerThread(void (*enter)(), void (*leave)()) {
        struct ThreadRegistration {
            explicit ThreadRegistration(void (*enter)(), void (*leave)()) : leave(leave) { enter(); }
            ~ThreadRegistration() { leave(); }
            void (*leave)();
        };
        static thread_local ThreadRegistration registration(enter, leave);
    }

    virtual void onSave(android::base::Stream* stream) override {
        DD("%s: saving GLES pipe state for hwpipe=%p", __FUNCTION__, mHwPipe);
        stream->putBe32(mIsWorking);
        android::base::saveBuffer(stream, mDataForReading);
        stream->putBe32(mDataForReadingLeft);

        android::snapshot::GfxstreamStreamAdapter gfxstreamStream(stream);
        mChannel->onSave(&gfxstreamStream);
    }

    virtual void onGuestClose(PipeCloseReason reason) override {
        D("%s", __func__);
        mIsWorking = false;
        mChannel->stop();
        if (auto queue = std::atomic_load(&mSharedReceive)) queue->close();
        if (auto queue = std::atomic_load(&mSharedTransmit)) queue->close();
        // Make sure there's no operation scheduled for this pipe instance to
        // run on the main thread.
        abortPendingOperation();
        delete this;
    }

    virtual unsigned onGuestPoll() const override {
        DD("%s", __func__);

        unsigned ret = 0;
        if (mDataForReadingLeft > 0) {
            ret |= PIPE_POLL_IN;
        }
        ChannelState state = mChannel->state();
        if ((state & ChannelState::CanRead) != 0) {
            ret |= PIPE_POLL_IN;
        }
        if ((state & ChannelState::CanWrite) != 0) {
            ret |= PIPE_POLL_OUT;
        }
        if ((state & ChannelState::Stopped) != 0) {
            ret |= PIPE_POLL_HUP;
        }
        DD("%s: returning %d", __func__, ret);
        return ret;
    }

    virtual int onGuestRecv(AndroidPipeBuffer* buffers,
                            int numBuffers) override {
        if (std::atomic_load(&mSharedReceive)) return PIPE_ERROR_INVAL;
        DD("%s", __func__);

        // Consume the pipe's dataForReading, then put the next received data
        // piece there. Repeat until the buffers are full or we're out of data
        // in the channel.
        int len = 0;
        size_t buffOffset = 0;

        static constexpr android::base::System::Duration kBlockReportIntervalUs = 1000000ULL;
        auto buff = buffers;
        const auto buffEnd = buff + numBuffers;
        while (buff != buffEnd) {
            if (mDataForReadingLeft == 0) {
                if (android::opengl::recvMode == android::opengl::RecvMode::Android) {
                    // No data left, read a new chunk from the channel.
                    int spinCount = 20;
                    for (;;) {

                        auto result = mChannel->tryRead(&mDataForReading);
                        if (result == IoResult::Ok) {
                            mDataForReadingLeft = mDataForReading.size();
                            break;
                        }
                        DD("%s: tryRead() failed with %d", __func__, (int)result);
                        if (len > 0) {
                            DD("%s: returning %d bytes", __func__, (int)len);
                            return len;
                        }
                        // This failed either because the channel was stopped
                        // from the host, or if there was no data yet in the
                        if (result == IoResult::Error) {
                            return PIPE_ERROR_IO;
                        }
                        // Spin a little before declaring there is nothing
                        // to read. Many GL calls are much faster than the
                        // whole host-to-guest-to-host transition.
                        if (--spinCount > 0) {
                            continue;
                        }
                        DD("%s: returning PIPE_ERROR_AGAIN", __func__);
                        return PIPE_ERROR_AGAIN;
                    }
                } else if (android::opengl::recvMode == android::opengl::RecvMode::Fuchsia) {
                    // No data left, return if we already received some data,
                    // otherwise read a new chunk from the channel.
                    if (len > 0) {
                        DD("%s: returning %d bytes", __func__, (int)len);
                        return len;
                    }
                    // Block a little before declaring there is nothing
                    // to read. This gives the render thread a chance to
                    // process pending data before we return control to
                    // the guest. The amount of time we block here should
                    // be kept at a minimum. It's preferred to instead have
                    // the guest block on work that takes a significant
                    // amount of time.

                    static constexpr android::base::System::Duration kBlockReportIntervalUs = 1000000ULL;

                    const RenderChannel::Duration kBlockAtMostUs = 100;
                    auto currTime = android::base::System::get()->getUnixTimeUs();
                    auto result = mChannel->readBefore(&mDataForReading, currTime + kBlockAtMostUs);

                    if (result != IoResult::Ok) {
                        DD("%s: tryRead() failed with %d", __func__, (int)result);
                        // This failed either because the channel was stopped
                        // from the host, or if there was no data yet in the
                        // channel.
                        if (len > 0) {
                            DD("%s: returning %d bytes", __func__, (int)len);
                            return len;
                        }
                        if (result == IoResult::Error) {
                            return PIPE_ERROR_IO;
                        }

                        DD("%s: returning PIPE_ERROR_AGAIN", __func__);
                        return PIPE_ERROR_AGAIN;
                    }
                    mDataForReadingLeft = mDataForReading.size();
                } else { // Virtio-gpu
                    // No data left, return if we already received some data,
                    // otherwise read a new chunk from the channel.
                    if (len > 0) {
                        DD("%s: returning %d bytes", __func__, (int)len);
                        return len;
                    }
                    // Block a little before declaring there is nothing
                    // to read. This gives the render thread a chance to
                    // process pending data before we return control to
                    // the guest. The amount of time we block here should
                    // be kept at a minimum. It's preferred to instead have
                    // the guest block on work that takes a significant
                    // amount of time.

                    static constexpr android::base::System::Duration kBlockReportIntervalUs = 1000000ULL;


                    const RenderChannel::Duration kBlockAtMostUs = 10000;
                    auto currTime = android::base::System::get()->getUnixTimeUs();
                    auto result = mChannel->readBefore(&mDataForReading, currTime + kBlockAtMostUs);

                    if (result != IoResult::Ok) {
                        DD("%s: tryRead() failed with %d", __func__, (int)result);
                        // This failed either because the channel was stopped
                        // from the host, or if there was no data yet in the
                        // channel.
                        if (len > 0) {
                            DD("%s: returning %d bytes", __func__, (int)len);
                            return len;
                        }
                        if (result == IoResult::Error) {
                            return PIPE_ERROR_IO;
                        }

                        DD("%s: returning PIPE_ERROR_AGAIN", __func__);
                        return PIPE_ERROR_AGAIN;
                    }
                    mDataForReadingLeft = mDataForReading.size();
                }
            }

            const size_t curSize = std::min<size_t>(buff->size - buffOffset,
                                                    mDataForReadingLeft);
            memcpy(buff->data + buffOffset,
                   mDataForReading.data() +
                           (mDataForReading.size() - mDataForReadingLeft),
                   curSize);

            len += curSize;
            mDataForReadingLeft -= curSize;
            buffOffset += curSize;
            if (buffOffset == buff->size) {
                ++buff;
                buffOffset = 0;
            }
        }

        DD("%s: received %d bytes", __func__, (int)len);
        return len;
    }

    virtual void waitGuestRecv() const override {
        mChannel->waitUntilReadable();
    }

    virtual int onGuestSend(const AndroidPipeBuffer* buffers,
                            int numBuffers,
                            void** newPipePtr) override {
        DD("%s", __func__);

        if (!mIsWorking) {
            DD("%s: pipe already closed!", __func__);
            return PIPE_ERROR_IO;
        }

        // Count the total bytes to send.
        int count = 0;
        for (int n = 0; n < numBuffers; ++n) {
            count += buffers[n].size;
        }

        // Copy everything into a single ChannelBuffer.
        ChannelBuffer outBuffer;
        outBuffer.resize_noinit(count);
        auto ptr = outBuffer.data();
        for (int n = 0; n < numBuffers; ++n) {
            memcpy(ptr, buffers[n].data, buffers[n].size);
            ptr += buffers[n].size;
        }

        D("%s: %p sending %d bytes to host", __func__, this, count);
        // Send it through the channel.
        auto result = mChannel->tryWrite(std::move(outBuffer));
        if (result != IoResult::Ok) {
            D("%s: tryWrite() failed with %d", __func__, (int)result);
            return result == IoResult::Error ? PIPE_ERROR_IO : PIPE_ERROR_AGAIN;
        }

        return count;
    }

    virtual void waitGuestSend() const override {
        mChannel->waitUntilWritable();
    }

    virtual void onGuestWantWakeOn(int flags) override {
        DD("%s: flags=%d", __func__, flags);

        // Translate |flags| into ChannelState flags.
        ChannelState wanted = ChannelState::Empty;
        if (flags & PIPE_WAKE_READ) {
            wanted |= ChannelState::CanRead;
        }
        if (flags & PIPE_WAKE_WRITE) {
            wanted |= ChannelState::CanWrite;
        }

        // Signal events that are already available now.
        ChannelState state = mChannel->state();
        ChannelState available = state & wanted;
        DD("%s: state=%d wanted=%d available=%d", __func__, (int)state,
           (int)wanted, (int)available);
        if (available != ChannelState::Empty) {
            DD("%s: signaling events %d", __func__, (int)available);
            signalState(available);
            wanted &= ~available;
        }

        // Ask the channel to be notified of remaining events.
        if (wanted != ChannelState::Empty) {
            DD("%s: waiting for events %d", __func__, (int)wanted);
            mChannel->setWantedEvents(wanted);
        }
    }

private:
    // Called to signal the guest that read/write wake events occured.
    // Note: this can be called from either the guest or host render
    // thread.
    void signalState(ChannelState state) {
        int wakeFlags = 0;
        if ((state & ChannelState::CanRead) != 0) {
            wakeFlags |= PIPE_WAKE_READ;
        }
        if ((state & ChannelState::CanWrite) != 0) {
            wakeFlags |= PIPE_WAKE_WRITE;
        }
        if (wakeFlags != 0) {
            this->signalWake(wakeFlags);
        }
    }

    // Called when an i/o event occurs on the render channel
    void onChannelHostEvent(ChannelState state) {
        D("%s: events %d (working %d)", __func__, (int)state, (int)mIsWorking);
        // NOTE: This is called from the host-side render thread.
        // but closeFromHost() and signalWake() can be called from
        // any thread.
        if (!mIsWorking) {
            return;
        }
        if ((state & ChannelState::Stopped) != 0) {
            if (auto queue = std::atomic_load(&mSharedReceive)) queue->close();
            if (auto queue = std::atomic_load(&mSharedTransmit)) queue->close();
            this->closeFromHost();
            return;
        }
        signalState(state);
    }

    // A RenderChannel pointer used for communication.
    RenderChannelPtr mChannel;
    std::shared_ptr<android::SharedReceiveQueue> mSharedReceive;
    std::shared_ptr<android::SharedTransmitQueue> mSharedTransmit;
    void (*mConsumerEnter)() = nullptr;
    void (*mConsumerLeave)() = nullptr;

    // Set to |true| if the pipe is in working state, |false| means we're not
    // initialized or the pipe is closed.
    bool mIsWorking = false;

    // These two variables serve as a reading buffer for the guest.
    // Each time we get a read request, first we extract a single chunk from
    // the |mChannel| into here, and then copy its content into the
    // guest-supplied memory.
    // If guest didn't have enough room for the whole buffer, we track the
    // number of remaining bytes in |mDataForReadingLeft| for the next read().
    uint32_t mDataForReadingLeft = 0;
    ChannelBuffer mDataForReading;

    DISALLOW_COPY_ASSIGN_AND_MOVE(EmuglPipe);
};

}  // namespace

void registerPipeService() {
    android::AndroidPipe::Service::add(std::make_unique<EmuglPipe::Service>());
    registerGLProcessPipeService();
}

void pipeSetRecvMode(int mode) {
    recvMode = (RecvMode)mode;
}

}  // namespace opengl
}  // namespace android

// Declared in android/opengles-pipe.h
void android_init_opengles_pipe() {
    android::opengl::registerPipeService();
}

void android_opengles_pipe_set_recv_mode(int mode) {
    android::opengl::pipeSetRecvMode(mode);
}

static android::opengl::EmuglPipe* sharedReceivePipe(void* opaque) {
    auto* pipe = static_cast<android::AndroidPipe*>(opaque);
    return pipe && !strcmp(pipe->name(), "opengles")
            ? static_cast<android::opengl::EmuglPipe*>(pipe) : nullptr;
}

int android_pipe_rx_setup(void* pipe, PipeRxHost* host, PipeRxGuest* guest,
                          PipeRxDirty dirty, bool restoring, void (*enter)(), void (*leave)()) {
    auto* service = sharedReceivePipe(pipe);
    return service ? service->setupSharedReceive(host, guest, dirty, restoring, enter, leave) : -1;
}
int android_pipe_rx_add_segment(void* pipe, unsigned id, void* const* pages) {
    auto* service = sharedReceivePipe(pipe);
    return service ? service->addSharedSegment(id, pages) : -1;
}
int android_pipe_rx_activate(void* pipe) {
    auto* service = sharedReceivePipe(pipe);
    return service ? service->activateSharedReceive() : -1;
}
void android_pipe_rx_credit(void* pipe) {
    auto* service = sharedReceivePipe(pipe);
    if (service) service->creditSharedReceive();
}
void android_pipe_rx_arm(void* pipe, uint32_t sequence) {
    auto* service = sharedReceivePipe(pipe);
    if (service) service->armSharedReceive(sequence);
}

void* android_pipe_guest_write_event(void* pipe) {
    auto* service = sharedReceivePipe(pipe);
    return service ? service->guestWriteEvent() : nullptr;
}

int android_pipe_tx_setup(void* pipe, PipeTxGuest* guest, PipeTxHost* host,
                          PipeRxDirty dirty, bool restoring, void (*enter)(), void (*leave)()) {
    auto* service = sharedReceivePipe(pipe);
    return service ? service->setupSharedTransmit(guest, host, dirty, restoring, enter, leave) : -1;
}

int android_pipe_tx_add_segment(void* pipe, unsigned id, void* const* pages, unsigned count) {
    auto* service = sharedReceivePipe(pipe);
    return service ? service->addTransmitSegment(id, pages, count) : -1;
}

int android_pipe_tx_activate(void* pipe) {
    auto* service = sharedReceivePipe(pipe);
    return service ? service->activateSharedTransmit() : -1;
}

void android_pipe_tx_kick(void* pipe) {
    if (auto* service = sharedReceivePipe(pipe)) service->kickSharedTransmit();
}

void android_pipe_tx_arm(void* pipe, uint32_t sequence) {
    if (auto* service = sharedReceivePipe(pipe)) service->armSharedTransmit(sequence);
}

void android_pipe_tx_detach(void* pipe) {
    if (auto* service = sharedReceivePipe(pipe)) service->detachSharedTransmit();
}

void android_pipe_tx_set_running(void* pipe, bool running) {
    if (auto* service = sharedReceivePipe(pipe)) service->setTransmitRunning(running);
}


void android_pipe_rx_set_running(void* pipe, bool running) {
    if (auto* service = sharedReceivePipe(pipe)) service->setReceiveRunning(running);
}
