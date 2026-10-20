#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

class QThread;

namespace kemu {
// Toggles Android's media mute state through AudioService using the emulator's
// existing in-process ADB transport.
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
