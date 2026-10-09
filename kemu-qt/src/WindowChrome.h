#pragma once

#include <QObject>
#include <QSize>
#include <QTimer>
#include <QVector>

class EmulatorContainer;
class EmulatorQtWindow;
class IconButton;
class QLabel;
class QWidget;
class QEvent;

namespace kemu {
class AndroidMute;
bool enabled();

// Owns only Kemu's chrome. EmulatorContainer remains the real top-level window;
// EmulatorQtWindow and its native GPU surface retain their original ownership.
class WindowChrome final : public QObject {
public:
    WindowChrome(EmulatorContainer* container, EmulatorQtWindow* display);
    ~WindowChrome() override;
    void layout();
    void surfaceChanged();
    void updateWindowState();
    void beginClose();
    bool nativeEvent(void* message, qintptr* result);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void toggleMaximized();
    void requestResize();
    void pollBoot();
    void writeDiagnostics();
    void checkAdb();
    EmulatorContainer* mContainer;
    EmulatorQtWindow* mDisplay;
    QWidget* mTitle = nullptr;
    QWidget* mSidebar = nullptr;
    QWidget* mLoading = nullptr;
    QLabel* mLoadingText = nullptr;
    IconButton* mMaximize = nullptr;
    AndroidMute* mMute = nullptr;
    QVector<IconButton*> mButtons;
    QTimer mResize;
    QTimer mBootPoll;
    QSize mRequestedSize;
    bool mLayoutActive = false;
    bool mHaveSurface = false;
    bool mReady = false;
    bool mClosing = false;
    bool mAdbChecked = false;
    QString mAdbPath;
    void* mInstanceHandle = nullptr;
};
}  // namespace kemu
