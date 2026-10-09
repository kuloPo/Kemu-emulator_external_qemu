#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

class QThread;

namespace kemu {
// Injects Android VOLUME_MUTE using the emulator's existing in-process ADB
// transport. The image's qwerty2 layout has no Linux KEY_MUTE mapping.
class AndroidMute final : public QObject {
public:
    explicit AndroidMute(QObject* parent);
    ~AndroidMute() override;
    bool send(std::function<void(QString)> completed);
    void cancel();
private:
    struct Request;
    std::shared_ptr<Request> mRequest;
    QThread* mThread = nullptr;
    QTimer mDeadline;
};
}  // namespace kemu
