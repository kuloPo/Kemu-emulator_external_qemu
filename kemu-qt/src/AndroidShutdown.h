#pragma once

#include <QObject>
#include <QTimer>
#include <memory>

class QThread;

namespace kemu {
// Requests guest poweroff; only the guest's poweroff may stop the VM.
class AndroidShutdown final : public QObject {
public:
    explicit AndroidShutdown(QObject* parent);
    ~AndroidShutdown() override;
    void start();

private:
    void request();
    void cancelRequest();
    struct Request;
    std::shared_ptr<Request> mRequest;
    QThread* mThread = nullptr;
    QTimer mRetry;
    QTimer mDeadline;
    bool mStarted = false;
};
}  // namespace kemu
