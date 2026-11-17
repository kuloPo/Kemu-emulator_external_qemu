#include "AndroidShutdown.h"

#include <QDebug>
#include <QThread>
#include <chrono>
#include <mutex>
#include "android/emulation/control/adb/AdbConnection.h"

namespace kemu {
using android::emulation::AdbConnection;
using android::emulation::AdbStream;
using namespace std::chrono_literals;

struct AndroidShutdown::Request {
    std::mutex mutex;
    bool cancelled = false;
    std::shared_ptr<AdbStream> stream;
};

AndroidShutdown::AndroidShutdown(QObject* parent) : QObject(parent) {
    mRetry.setSingleShot(true);
    mRetry.setInterval(3000);
    connect(&mRetry, &QTimer::timeout, this, &AndroidShutdown::request);
    mDeadline.setSingleShot(true);
    mDeadline.setInterval(10000);
    connect(&mDeadline, &QTimer::timeout, this, &AndroidShutdown::cancelRequest);
}

AndroidShutdown::~AndroidShutdown() {
    mRetry.stop();
    cancelRequest();
    if (mThread) {
        disconnect(mThread, nullptr, this, nullptr);
        mThread->wait();
        delete mThread;
    }
}

void AndroidShutdown::start() {
    if (mStarted) return;
    mStarted = true;
    qInfo("Kemu is waiting for Android to power off");
    request();
}

void AndroidShutdown::cancelRequest() {
    mDeadline.stop();
    if (!mRequest) return;
    std::shared_ptr<AdbStream> stream;
    {
        std::lock_guard<std::mutex> lock(mRequest->mutex);
        mRequest->cancelled = true;
        stream = mRequest->stream;
    }
    // A stalled ADB request is not permission to terminate the guest.
    if (stream) stream->close();
}

void AndroidShutdown::request() {
    if (mThread) return;
    mRequest = std::make_shared<Request>();
    const auto request = mRequest;
    mThread = QThread::create([request] {
        const auto connection = AdbConnection::connection(2000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (request->cancelled) return;
        }
        if (!connection) return;
        // Let Android stop services, sync and unmount before powering off.
        // Before the framework boots, ask init to perform the same disk teardown.
        const auto stream = connection->open(
                "shell:if [ \"$(getprop sys.boot_completed)\" = 1 ]; then "
                "svc power shutdown; else reboot -p; fi", 2000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->stream = stream;
            if (request->cancelled) {
                if (stream) stream->close();
                return;
            }
        }
        if (!stream) return;
        char buffer[256];
        while (stream->read(buffer, sizeof(buffer))) {}
        stream->close();
    });
    connect(mThread, &QThread::finished, this, [this] {
        mDeadline.stop();
        mThread->deleteLater();
        mThread = nullptr;
        mRequest.reset();
        // ADB can disappear before guest storage is flushed, or be unavailable
        // during boot. Keep waiting and retry; VM completion closes the window.
        mRetry.start();
    });
    mDeadline.start();
    mThread->start();
}
}  // namespace kemu
