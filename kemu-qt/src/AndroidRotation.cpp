#include "AndroidRotation.h"
#include "RotationEvents.h"
#include "RotationObserverDex.h"

#include <QThread>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include "android/emulation/control/adb/AdbConnection.h"

namespace kemu {
using android::emulation::AdbConnection;
using android::emulation::AdbStream;
using namespace std::chrono_literals;

struct AndroidRotation::Request {
    std::mutex mutex;
    bool cancelled = false;
    std::shared_ptr<AdbStream> stream;
};

AndroidRotation::AndroidRotation(QObject* parent) : QObject(parent) {
    mRetry.setSingleShot(true);
    mDeadline.setSingleShot(true);
    mDeadline.setInterval(5000);
    connect(&mRetry, &QTimer::timeout, this, &AndroidRotation::listen);
    connect(&mDeadline, &QTimer::timeout, this, &AndroidRotation::cancelRequest);
}

AndroidRotation::~AndroidRotation() {
    stop();
    if (mThread) {
        disconnect(mThread, nullptr, this, nullptr);
        mThread->wait();
        delete mThread;
    }
}

void AndroidRotation::start(std::function<void(int)> changed) {
    mChanged = std::move(changed);
    mActive = true;
    mRetryDelay = 1000;
    listen();
}

void AndroidRotation::cancelRequest() {
    mDeadline.stop();
    if (!mRequest) return;
    std::shared_ptr<AdbStream> stream;
    {
        std::lock_guard<std::mutex> lock(mRequest->mutex);
        mRequest->cancelled = true;
        stream = mRequest->stream;
    }
    if (stream) stream->close();
}

void AndroidRotation::stop() {
    mActive = false;
    mRetry.stop();
    cancelRequest();
    // Finish using the ADB transport before the emulator stops its I/O loop.
    if (mThread) mThread->wait();
}

void AndroidRotation::listen() {
    if (!mActive || mThread) return;
    mRequest = std::make_shared<Request>();
    const auto request = mRequest;
    mThread = QThread::create([this, request] {
        const auto connection = AdbConnection::connection(1000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (request->cancelled) return;
        }
        if (!connection) return;
        const auto stream = connection->open("shell:sh", 1000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->stream = stream;
            if (request->cancelled) { if (stream) stream->close(); return; }
        }
        if (!stream || !stream->good()) return;
        // Cache the embedded dex per ADB user, including across root/unroot.
        // Verify cached contents and install atomically before executing them.
        const std::string command =
            "umask 077; dir=/data/local/tmp/kemu-rotation-$(id -u); "
            "mkdir -p \"$dir\" || exit 1; dex=\"$dir/" +
            std::string(kRotationObserverHash) + ".dex\"; "
            "if ! echo '" + kRotationObserverHash +
            "  '\"$dex\" | sha256sum -c - >/dev/null 2>&1; then "
            "tmp=$(mktemp \"$dir/.XXXXXX\") || exit 1; "
            "printf '%s' '" + kRotationObserverDex +
            "' | base64 -d >\"$tmp\" && chmod 444 \"$tmp\" && "
            "mv -f \"$tmp\" \"$dex\" || { rm -f \"$tmp\"; exit 1; }; fi; "
            "CLASSPATH=\"$dex\" exec app_process / org.kemu.rotation.RotationObserver\n";
        stream->setWriteTimeout(1000ms);
        stream->write(command.data(), command.size());
        stream->flush();
        RotationEvents events;
        char ch;
        while (stream->read(&ch, 1)) {
            if (const auto rotation = events.accept(ch)) {
                QMetaObject::invokeMethod(this, [this, request, rotation] {
                    if (!mActive || mRequest != request) return;
                    {
                        std::lock_guard<std::mutex> lock(request->mutex);
                        if (request->cancelled) return;
                    }
                    mDeadline.stop();
                    mRetryDelay = 1000;
                    mChanged(*rotation);
                }, Qt::QueuedConnection);
            }
        }
        stream->close();
    });
    connect(mThread, &QThread::finished, this, [this] {
        mDeadline.stop();
        mThread->deleteLater();
        mThread = nullptr;
        mRequest.reset();
        if (!mActive) return;
        mRetry.start(mRetryDelay);
        mRetryDelay = std::min(mRetryDelay * 2, 10000);
    });
    mDeadline.start();
    mThread->start();
}
}  // namespace kemu
