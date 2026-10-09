#include "AndroidMute.h"

#include <QThread>
#include <chrono>
#include <mutex>
#include <string>
#include "android/emulation/control/adb/AdbConnection.h"

namespace kemu {
using android::emulation::AdbConnection;
using android::emulation::AdbStream;
using namespace std::chrono_literals;

struct AndroidMute::Request {
    std::mutex mutex;
    bool cancelled = false;
    std::shared_ptr<AdbStream> stream;
    QString error;
};

AndroidMute::AndroidMute(QObject* parent) : QObject(parent) {
    mDeadline.setSingleShot(true);
    mDeadline.setInterval(5000);
    connect(&mDeadline, &QTimer::timeout, this, &AndroidMute::cancel);
}

AndroidMute::~AndroidMute() {
    cancel();
    if (mThread) {
        disconnect(mThread, nullptr, this, nullptr);
        mThread->wait();
        delete mThread;
    }
}

void AndroidMute::cancel() {
    mDeadline.stop();
    if (!mRequest) return;
    std::shared_ptr<AdbStream> stream;
    {
        std::lock_guard<std::mutex> lock(mRequest->mutex);
        mRequest->cancelled = true;
        stream = mRequest->stream;
    }
    // Close only our stream, never the shared connection used by upstream.
    if (stream) stream->close();
}

bool AndroidMute::send(std::function<void(QString)> completed) {
    if (mThread) return false;
    mRequest = std::make_shared<Request>();
    const auto request = mRequest;
    mThread = QThread::create([request] {
        const auto connection = AdbConnection::connection(2000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (request->cancelled) return;
        }
        if (!connection) { request->error = QStringLiteral("Android 连接不可用"); return; }
        // Fixed command: the marker reports the guest command's exit status
        // even when using the shell v1 protocol. No host adb.exe is launched.
        const auto stream = connection->open("shell:input keyevent 164; echo __KEMU_KEY_EXIT:$?", 2000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->stream = stream;
            if (request->cancelled) { if (stream) stream->close(); return; }
        }
        if (!stream || !stream->good()) { request->error = QStringLiteral("Android 按键通道不可用"); return; }
        std::string response;
        char ch;
        while (response.size() < 4096 && stream->get(ch)) response.push_back(ch);
        stream->close();
        if (response.find("__KEMU_KEY_EXIT:0") == std::string::npos)
            request->error = QStringLiteral("Android 未完成静音按键");
    });
    connect(mThread, &QThread::finished, this, [this, request, completed = std::move(completed)] {
        mDeadline.stop();
        mThread->deleteLater();
        mThread = nullptr;
        QString error;
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            error = request->cancelled ? QStringLiteral("Android 静音按键超时或已取消") : request->error;
        }
        mRequest.reset();
        completed(error);
    });
    mDeadline.start();
    mThread->start();
    return true;
}
}  // namespace kemu
