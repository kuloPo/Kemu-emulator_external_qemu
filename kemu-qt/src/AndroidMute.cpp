#include "AndroidMute.h"
#include <QCoreApplication>
#include <QSettings>

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
constexpr char kMuteCommand[] = R"SH((
music_mute() {
    dumpsys audio | sed -n '/^- STREAM_MUSIC:/,/^$/s/^[[:space:]]*Muted: //p'
}
current=$(music_mute)
case "$current" in true|false) ;; *) exit 1 ;; esac
case "$desired:$current" in
    toggle:false|true:*) action=adj-mute; expected=true ;;
    toggle:true|false:*) action=adj-unmute; expected=false ;;
    *) exit 1 ;;
esac
if [ "$current" != "$expected" ]; then
    cmd audio "$action" 3 || exit 1
fi
[ "$(music_mute)" = "$expected" ] || exit 1
printf '\n__KEMU_MUTE_STATE:%s\n' "$expected"
); printf '\n__KEMU_MUTE_EXIT:%s\n' "$?")SH";
constexpr char kMuteSetting[] = "audio/mediaMuted";
}  // namespace

struct AndroidMute::Request {
    std::mutex mutex;
    bool cancelled = false;
    std::shared_ptr<AdbStream> stream;
    QString error;
    bool muted = false;
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
    return run("toggle", true, std::move(completed));
}

bool AndroidMute::restore(std::function<void(QString)> completed) {
    if (mThread) return false;
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
    if (!settings.contains(kMuteSetting)) {
        completed({});
        return true;
    }
    return run(settings.value(kMuteSetting).toBool() ? "true" : "false",
               false, std::move(completed));
}

bool AndroidMute::run(const char* desired, bool persist, std::function<void(QString)> completed) {
    if (mThread) return false;
    mRequest = std::make_shared<Request>();
    const auto request = mRequest;
    const std::string command = std::string("shell:desired=") + desired + "; " + kMuteCommand;
    mThread = QThread::create([request, command] {
        const auto connection = AdbConnection::connection(2000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (request->cancelled) return;
        }
        if (!connection) { request->error = QCoreApplication::translate("Kemu", "Android connection is unavailable"); return; }
        // Fixed command: the marker reports the guest command's exit status
        // even when using the shell v1 protocol. No host adb.exe is launched.
        const auto stream = connection->open(command, 2000ms);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->stream = stream;
            if (request->cancelled) { if (stream) stream->close(); return; }
        }
        if (!stream || !stream->good()) { request->error = QCoreApplication::translate("Kemu", "Android audio control channel is unavailable"); return; }
        std::string response;
        char ch;
        // AdbStreambuf implements xsgetn, so read() waits for the reply.
        while (response.size() < 4096 && stream->read(&ch, 1)) response.push_back(ch);
        stream->close();
        const auto hasMarker = [&response](const char* marker) {
            return response.find(std::string("\n") + marker + "\n") != std::string::npos ||
                   response.find(std::string("\r\n") + marker + "\r\n") != std::string::npos;
        };
        request->muted = hasMarker("__KEMU_MUTE_STATE:true");
        if (!hasMarker("__KEMU_MUTE_EXIT:0") ||
            (!request->muted && !hasMarker("__KEMU_MUTE_STATE:false")))
            request->error = QCoreApplication::translate("Kemu", "Android did not change the media mute state");
    });
    connect(mThread, &QThread::finished, this, [this, request, persist, completed = std::move(completed)] {
        mDeadline.stop();
        mThread->deleteLater();
        mThread = nullptr;
        QString error;
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            error = request->cancelled ? QCoreApplication::translate("Kemu", "Android mute request timed out or was cancelled") : request->error;
        }
        mRequest.reset();
        if (persist && error.isEmpty()) {
            QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
            settings.setValue(kMuteSetting, request->muted);
            settings.sync();
            if (settings.status() != QSettings::NoError)
                qWarning("Kemu could not save the media mute preference");
        }
        completed(error);
    });
    mDeadline.start();
    mThread->start();
    return true;
}
}  // namespace kemu
