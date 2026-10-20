#include "AndroidMute.h"
#include <QCoreApplication>

#include <QThread>
#include <chrono>
#include <mutex>
#include <string>
#include "android/emulation/control/adb/AdbConnection.h"

namespace kemu {
using android::emulation::AdbConnection;
using android::emulation::AdbStream;
using namespace std::chrono_literals;

namespace {
// Control STREAM_MUSIC through AudioService so foreground apps cannot consume
// the request. Read the guest state on every click and verify the change.
constexpr char kToggleMuteCommand[] = R"SH(shell:(
music_mute() {
    dumpsys audio | sed -n '/^- STREAM_MUSIC:/,/^$/s/^[[:space:]]*Muted: //p'
}
case "$(music_mute)" in
    false) action=adj-mute; expected=true ;;
    true) action=adj-unmute; expected=false ;;
    *) exit 1 ;;
esac
cmd audio "$action" 3 || exit 1
[ "$(music_mute)" = "$expected" ]
); printf '\n__KEMU_MUTE_EXIT:%s\n' "$?")SH";
}  // namespace

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
        if (!connection) { request->error = QCoreApplication::translate("Kemu", "Android connection is unavailable"); return; }
        // Fixed command: the marker reports the guest command's exit status
        // even when using the shell v1 protocol. No host adb.exe is launched.
        const auto stream = connection->open(kToggleMuteCommand, 2000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->stream = stream;
            if (request->cancelled) { if (stream) stream->close(); return; }
        }
        if (!stream || !stream->good()) { request->error = QCoreApplication::translate("Kemu", "Android audio control channel is unavailable"); return; }
        std::string response;
        char ch;
        while (response.size() < 4096 && stream->get(ch)) response.push_back(ch);
        stream->close();
        if (response.find("\n__KEMU_MUTE_EXIT:0\n") == std::string::npos &&
            response.find("\r\n__KEMU_MUTE_EXIT:0\r\n") == std::string::npos)
            request->error = QCoreApplication::translate("Kemu", "Android did not change the media mute state");
    });
    connect(mThread, &QThread::finished, this, [this, request, completed = std::move(completed)] {
        mDeadline.stop();
        mThread->deleteLater();
        mThread = nullptr;
        QString error;
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            error = request->cancelled ? QCoreApplication::translate("Kemu", "Android mute request timed out or was cancelled") : request->error;
        }
        mRequest.reset();
        completed(error);
    });
    mDeadline.start();
    mThread->start();
    return true;
}
}  // namespace kemu
