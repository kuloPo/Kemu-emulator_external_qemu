#pragma once

#include <QLabel>
#ifdef _WIN32
#include <windows.h>
#endif

namespace kemu {
class FrameRateLabel final : public QLabel {
public:
    explicit FrameRateLabel(QWidget* parent)
        : QLabel("FPS: --", parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NativeWindow);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
#ifdef _WIN32
        if (result && static_cast<MSG*>(message)->message == WM_NCHITTEST) {
            // Let Windows target the display below this native sibling.
            *result = HTTRANSPARENT;
            return true;
        }
#endif
        return QLabel::nativeEvent(type, message, result);
    }
};
}  // namespace kemu
