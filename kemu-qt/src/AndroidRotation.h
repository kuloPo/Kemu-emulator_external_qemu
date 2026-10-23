#pragma once

#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

class QThread;

namespace kemu {
class AndroidRotation final : public QObject {
public:
    explicit AndroidRotation(QObject* parent);
    ~AndroidRotation() override;
    void start(std::function<void(int)> changed);
    void stop();

private:
    struct Request;
    void poll();
    void cancelRequest();
    std::shared_ptr<Request> mRequest;
    QThread* mThread = nullptr;
    QTimer mPoll;
    QTimer mDeadline;
    std::function<void(int)> mChanged;
    bool mActive = false;
};
}  // namespace kemu
