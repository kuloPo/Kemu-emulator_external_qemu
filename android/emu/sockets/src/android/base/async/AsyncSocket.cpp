// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "android/base/async/AsyncSocket.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>

#include "absl/log/log.h"

#include "aemu/base/sockets/SocketUtils.h"
#include "android/base/system/System.h"

namespace android {
namespace base {

// This callback is called whenever an I/O event happens on the mSocket
// connecting the mSocket to the host ADB server.
static void socket_watcher(void* opaque, int fd, unsigned events) {
    const auto socket = static_cast<AsyncSocket*>(opaque);
    if (!socket) {
        return;
    }
    if ((events & Looper::FdWatch::kEventRead) != 0) {
        socket->onRead();
    } else if ((events & Looper::FdWatch::kEventWrite) != 0) {
        socket->onWrite();
    }
}

AsyncSocket::~AsyncSocket() {
    dispose();
}

AsyncSocket::AsyncSocket(Looper* looper, int port)
    : mPort(port), mLooper(looper), mWriteQueue(WRITE_BUFFER_SIZE, mWriteQueueLock) {}

AsyncSocket::AsyncSocket(Looper* looper, ScopedSocket socket)
    : mSocket(std::move(socket)),
      mPort(-1),
      mLooper(looper),
      mWriteQueue(WRITE_BUFFER_SIZE, mWriteQueueLock) {
    socketSetNonBlocking(mSocket.get());
    mFdWatch = std::unique_ptr<Looper::FdWatch>(
            mLooper->createFdWatch(mSocket.get(), socket_watcher, this));
    wantRead();
}

void AsyncSocket::scheduleCallback(std::function<void()> callback) {
    auto guarded = [state = mCallbackState, cb = std::move(callback)] {
        std::lock_guard<std::recursive_mutex> lock(state->mutex);
        if (state->active) cb();
    };
    if (mLooper->onLooperThread()) {
        guarded();
        return;
    }
    mLooper->scheduleCallback(std::move(guarded));
};

void AsyncSocket::wantRead() {
    scheduleCallback([this]() {
        if (mFdWatch) {
            mFdWatch->wantRead();
        }
    });
}

ssize_t AsyncSocket::recv(char* buffer, uint64_t bufferSize) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> watchLock(mWatchLock);
        if (mFdWatch) {
            fd = mFdWatch->fd();
        }
        if (fd == -1) {
            return 0;
        }
        wantRead();
    }

    // It is still possible that the fd is no longer open
    ssize_t read = socketRecv(fd, buffer, bufferSize);
    if (read == 0) {
        // A read of 0, means the socket was closed, so clean up
        // everything properly.
        close();
        return 0;
    }
    return read;
}

void AsyncSocket::onRead() {
    if (!connected() || !mListener) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mListenerLock);
    mListener->onRead(this);
}

void AsyncSocket::wantWrite() {
    scheduleCallback([this]() {
        if (mFdWatch) {
            mFdWatch->wantWrite();
        }
    });
}

ssize_t AsyncSocket::send(const char* buffer, uint64_t bufferSize) {
    if (mDisposed || !mFdWatch) {
        errno = ECONNRESET;
        return -1;
    }
    {
        AutoLock alock(mWriteQueueLock);
        if (mWriteQueue.pushLocked(std::string(buffer, bufferSize)) != BufferQueueResult::Ok) {
            return 0;
        }
        mSendBuffer += bufferSize;
    }
    VLOG(1) << "Pushed " << bufferSize;
    wantWrite();
    return bufferSize;
}

bool AsyncSocket::connected() {
    return mFdWatch.get() != nullptr;
}

void AsyncSocket::onWrite() {
    if (!connected()) {
        return;
    }
    auto delta = mAsyncWriter.written();
    auto status = mAsyncWriter.run();
    delta = mAsyncWriter.written() - delta;
    {
        std::unique_lock<std::mutex> lock(mSendBufferMutex);
        mSendBuffer -= delta;
        mSendBufferCv.notify_all();
    }
    switch (status) {
        case kAsyncCompleted:
            if (mWriteQueue.canPopLocked()) {
                AutoLock alock(mWriteQueueLock);
                if (mWriteQueue.popLocked(&mWriteBuffer) == BufferQueueResult::Ok) {
                    mAsyncWriter.reset(mWriteBuffer.data(), mWriteBuffer.size(), mFdWatch.get());
                }
            }
            return;
        case kAsyncError:
            return;
        case kAsyncAgain:
            return;
    }
}

void AsyncSocket::dispose() {
    // The flag rejects new I/O; every caller still waits for cleanup.
    mDisposed = true;
    close();
    {
        std::lock_guard<std::mutex> connectLock(mConnectThreadLock);
        if (mConnectThread && mConnectThread->joinable()) mConnectThread->join();
    }
    {
        std::unique_lock<std::mutex> watchLock(mWatchLock);
        mWatchLockCv.wait(watchLock, [this] { return mFdWatch == nullptr; });
    }
    // Wait only for a running callback, not callbacks queued on a stopped loop.
    std::lock_guard<std::recursive_mutex> callbackLock(mCallbackState->mutex);
    mCallbackState->active = false;
    setSocketEventListener(nullptr);
}

// Wait at most duration for the send buffer to be cleared
bool AsyncSocket::waitForSend(const std::chrono::milliseconds& rel_time) {
    std::unique_lock<std::mutex> lock(mSendBufferMutex);
    return mSendBufferCv.wait_for(lock, rel_time, [this] { return mSendBuffer == 0; });
}

void AsyncSocket::close() {
    uint64_t generation;
    {
        std::lock_guard<std::mutex> watchLock(mWatchLock);
        generation = ++mConnectGeneration;
        mConnecting = false;
        mPendingSocket.close();
        mWatchLockCv.notify_all();
        if (!mFdWatch) return;
    }
    scheduleCallback([this, generation]() {
        {
            std::lock_guard<std::mutex> watchLock(mWatchLock);
            if (generation != mConnectGeneration) return;
            mFdWatch.reset();
            mSocket.close();
            mWatchLockCv.notify_all();
        }
        if (mListener) {
            mListener->onClose(this, errno);
        }
    });
}

bool AsyncSocket::connect() {
    std::lock_guard<std::mutex> connectLock(mConnectThreadLock);
    if (mDisposed) return false;
    if (mConnecting) {
        return true;
    }
    {
        std::lock_guard<std::mutex> watchLock(mWatchLock);
        if (connected()) return true;
    }

    // A completed connection thread remains joinable until it is collected.
    // Replacing it directly would terminate the process on reconnect.
    if (mConnectThread) {
        mConnectThread->join();
    }
    std::lock_guard<std::mutex> watchLock(mWatchLock);
    if (mDisposed) return false;
    mConnecting = true;
    const auto generation = ++mConnectGeneration;
    mConnectThread = std::make_unique<std::thread>(
            [this, generation] { connectToPort(generation); });
    return true;
}

bool AsyncSocket::connectSync(std::chrono::milliseconds timeout) {
    if (!this->connect()) return false;

    std::unique_lock<std::mutex> watchLock(mWatchLock);
    const auto done = [this] { return mDisposed || !mConnecting; };
    if (timeout == std::chrono::milliseconds::max()) {
        mWatchLockCv.wait(watchLock, done);
    } else {
        mWatchLockCv.wait_for(watchLock, timeout, done);
    }
    return !mDisposed && !mConnecting && connected();
}

void AsyncSocket::connectToPort(uint64_t generation) {
    int socket = -1;
    const auto cancelled = [this, generation] {
        return mDisposed || generation != mConnectGeneration;
    };
    while (socket < 0 && !cancelled()) {
        socket = socketTcp4LoopbackClient(mPort);
        if (socket < 0 && !cancelled()) {
            socket = socketTcp6LoopbackClient(mPort);
        }
        if (socket < 0) {
            std::unique_lock<std::mutex> watchLock(mWatchLock);
            mWatchLockCv.wait_for(watchLock, std::chrono::seconds(1), cancelled);
        }
    }
    if (socket < 0) {
        return;
    }

    ScopedSocket pendingSocket(socket);
    std::lock_guard<std::mutex> watchLock(mWatchLock);
    if (cancelled()) return;
    mPendingSocket = std::move(pendingSocket);
    // Finish on the owner loop without making the connector wait for it.
    // The generation prevents a cancelled callback from reviving a connection.
    scheduleCallback([this, generation, cancelled] {
        {
            std::lock_guard<std::mutex> watchLock(mWatchLock);
            if (cancelled()) return;
            mSocket = std::move(mPendingSocket);
            socketSetNonBlocking(mSocket.get());
            mFdWatch.reset(mLooper->createFdWatch(mSocket.get(), socket_watcher, this));
            mFdWatch->wantRead();
        }
        if (mListener) {
            mListener->onConnected(this);
        }
        std::lock_guard<std::mutex> watchLock(mWatchLock);
        if (generation == mConnectGeneration) mConnecting = false;
        mWatchLockCv.notify_all();
    });
}

}  // namespace base
}  // namespace android
