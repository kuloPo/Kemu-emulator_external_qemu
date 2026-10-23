#include "AndroidRotation.h"
#include "DisplayRotation.h"

#include <QThread>
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
    std::optional<int> rotation;
};

AndroidRotation::AndroidRotation(QObject* parent) : QObject(parent) {
    mPoll.setSingleShot(true);
    mPoll.setInterval(500);
    mDeadline.setSingleShot(true);
    mDeadline.setInterval(3000);
    connect(&mPoll, &QTimer::timeout, this, &AndroidRotation::poll);
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
    poll();
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
    mPoll.stop();
    cancelRequest();
    // Finish using the ADB transport before the emulator stops its I/O loop.
    if (mThread) mThread->wait();
}

void AndroidRotation::poll() {
    if (!mActive || mThread) return;
    mRequest = std::make_shared<Request>();
    const auto request = mRequest;
    mThread = QThread::create([request] {
        const auto connection = AdbConnection::connection(1000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (request->cancelled) return;
        }
        if (!connection) return;
        // Input viewports report the actual display rotation, including app
        // orientation requests, without changing the virtual device sensors.
        const auto stream = connection->open(
            "shell:dumpsys input | grep 'Viewport .*displayId=0,'", 1000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->stream = stream;
            if (request->cancelled) { if (stream) stream->close(); return; }
        }
        if (!stream || !stream->good()) return;
        std::string response;
        char ch;
        while (response.size() < 16384 && stream->read(&ch, 1)) response.push_back(ch);
        const bool complete = stream->eof();
        stream->close();
        if (complete) request->rotation = displayRotation(QString::fromStdString(response));
    });
    connect(mThread, &QThread::finished, this, [this, request] {
        mDeadline.stop();
        mThread->deleteLater();
        mThread = nullptr;
        bool cancelled;
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            cancelled = request->cancelled;
        }
        mRequest.reset();
        if (!mActive) return;
        if (!cancelled && request->rotation) mChanged(*request->rotation);
        mPoll.start();
    });
    mDeadline.start();
    mThread->start();
}
}  // namespace kemu
