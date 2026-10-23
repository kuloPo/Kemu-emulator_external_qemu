#pragma once

#include <QObject>
#include <QPoint>
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
class AndroidRotation;
class UpdateController;
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
    void rotateDevice();
    void writeDiagnostics();
    void checkAdb();
    void applyPendingWindowSize();
    EmulatorContainer* mContainer;
    EmulatorQtWindow* mDisplay;
    QWidget* mTitle = nullptr;
    QWidget* mSidebar = nullptr;
    QWidget* mLoading = nullptr;
    QLabel* mLoadingText = nullptr;
    IconButton* mMaximize = nullptr;
    IconButton* mRotate = nullptr;
    AndroidMute* mMute = nullptr;
    AndroidRotation* mRotation = nullptr;
    UpdateController* mUpdates = nullptr;
    QVector<IconButton*> mButtons;
    QTimer mResize;
    QTimer mBootPoll;
    QSize mRequestedSize;
    QSize mNormalSizes[2];
    QSize mPendingWindowSize;
    QPoint mPendingWindowCenter;
    bool mSurfacePortrait = false;
    int mGuestRotation = -1;
    bool mLayoutActive = false;
    bool mHaveSurface = false;
    bool mReady = false;
    bool mClosing = false;
    bool mAdbChecked = false;
    QString mAdbPath;
    void* mInstanceHandle = nullptr;
};
}  // namespace kemu
