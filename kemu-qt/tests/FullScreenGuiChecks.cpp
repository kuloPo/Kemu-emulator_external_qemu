// Set KEMU_FULLSCREEN_TEST_RESULTS to a JSON path, then launch Kemu.exe.
#include "../src/IconButton.h"
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-qt-window.h"
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QScreen>
#include <QTimer>
#include <stdexcept>
#include <windows.h>

namespace kemu {
class FullScreenGuiChecks final : public QObject {
public:
    FullScreenGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display)
        : QObject(root), mRoot(root), mDisplay(display), mOriginal(root->geometry()),
          mCursor(QCursor::pos()) {
        mTitle = root->findChild<QWidget*>("KemuTitleBar");
        mSidebar = root->findChild<QWidget*>("KemuSidebar");
        mButton = root->findChild<IconButton*>("KemuFullScreen");
        root->raise();
        root->activateWindow();
        centerCursor();
        connect(&mTimer, &QTimer::timeout, this, [this] {
            try { tick(); }
            catch (const std::exception& error) { finish(QString::fromUtf8(error.what())); }
        });
        mTimer.start(1000);
    }

private:
    void require(bool condition, const char* error) {
        if (!condition) throw std::runtime_error(error);
    }
    void key(QWidget* receiver, bool repeat = false) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_F11, Qt::NoModifier, {}, repeat);
        QApplication::sendEvent(receiver, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_F11, Qt::NoModifier, {}, repeat);
        QApplication::sendEvent(receiver, &release);
    }
    void cursor(QPoint point) { QCursor::setPos(mRoot->mapToGlobal(point)); }
    void centerCursor() { cursor(mRoot->rect().center()); }
    void edgeCursor(int edge) {
        const HWND window = reinterpret_cast<HWND>(mRoot->winId());
        RECT bounds{};
        require(GetClientRect(window, &bounds), "Cannot read native client bounds");
        const POINT positions[] = {
            {bounds.right - 1, 0},
            {bounds.right - 1, bounds.bottom / 4},
            {bounds.right - 1, bounds.bottom / 2},
            {bounds.right - 1, bounds.bottom * 3 / 4},
            {bounds.right - 1, bounds.bottom - 1},
            {bounds.right / 2, 0},
            {0, 0}};
        POINT point = positions[edge];
        require(ClientToScreen(window, &point) && SetCursorPos(point.x, point.y),
            "Cannot place the cursor at the physical screen edge");
        POINT actual{};
        require(GetCursorPos(&actual) && actual.x == point.x && actual.y == point.y,
            "Cursor did not reach the physical screen edge");
    }
    void checkFullScreen() {
        require(mRoot->isFullScreen(), "Window did not enter full screen");
        require(mButton->iconType() == Icon::ExitFullScreen, "Full-screen mode is not active");
        require(mRoot->geometry() == mRoot->screen()->geometry(), "Full screen did not cover the monitor");
        require(mRoot->viewport()->geometry() == mRoot->rect(), "Chrome still reserves viewport space");
        const QSize fitted = mDisplay->backingSurfaceSize().scaled(mRoot->size(), Qt::KeepAspectRatio);
        require(qAbs(fitted.width() - mDisplay->width()) <= 1 &&
                qAbs(fitted.height() - mDisplay->height()) <= 1, "Rendered surface did not fit full screen");
    }
    void checkOverlay(QWidget* overlay) {
        require(overlay->isVisible(), "Edge did not reveal the toolbar");
        const QPoint point = overlay->mapTo(mRoot, overlay->rect().center());
        POINT pixel{qRound(point.x() * mRoot->devicePixelRatioF()),
                    qRound(point.y() * mRoot->devicePixelRatioF())};
        const HWND child = ChildWindowFromPointEx(reinterpret_cast<HWND>(mRoot->winId()), pixel,
            CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        require(child == reinterpret_cast<HWND>(overlay->winId()), "Renderer covers the toolbar");
        checkFullScreen();
    }
    void finish(const QString& error = {}) {
        mTimer.stop();
        if (mButton && mButton->iconType() == Icon::ExitFullScreen) mButton->click();
        mRoot->showNormal();
        mRoot->setGeometry(mOriginal);
        QCursor::setPos(mCursor);
        QFile file(qEnvironmentVariable("KEMU_FULLSCREEN_TEST_RESULTS"));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
            {"passed", error.isEmpty()}, {"error", error}, {"step", mStep},
            {"edgeStep", mEdgeStep}, {"dpr", mRoot->devicePixelRatioF()}, {"checks", mChecks}}).toJson());
        if (qEnvironmentVariableIsSet("KEMU_FULLSCREEN_TEST_EXIT")) mRoot->close();
    }
    void tick() {
        switch (mStep++) {
        case 0: {
            require(mTitle && mSidebar && mButton, "Missing full-screen controls");
            auto* separator = mRoot->findChild<QWidget*>("KemuFullScreenSeparator");
            auto* minimize = mRoot->findChild<IconButton*>("KemuMinimize");
            require(separator && minimize && mButton->geometry().right() < separator->x() &&
                separator->geometry().right() < minimize->x(), "Full-screen button or separator is misplaced");
            mButton->click();
            break;
        }
        case 1: {
            checkFullScreen();
            require(!mTitle->isVisible() && !mSidebar->isVisible(), "Chrome was not hidden on entry");
            require(mButton->iconType() == Icon::ExitFullScreen, "Full-screen exit icon was not set");
            const HWND window = reinterpret_cast<HWND>(mRoot->winId());
            POINT edge{};
            ClientToScreen(window, &edge);
            require(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(edge.x, edge.y)) == HTCLIENT,
                "Full-screen edge still starts a resize");
            key(mDisplay, true);
            require(mRoot->isFullScreen(), "Auto-repeat toggled full screen");
            mChecks.append("Button entry, monitor bounds, render fit, hidden chrome, no resize or key repeat");
            cursor(QPoint(mRoot->width() / 2, 2));
            break;
        }
        case 2:
            checkOverlay(mTitle);
            require(!mSidebar->isVisible(), "Top edge unexpectedly revealed the sidebar");
            cursor(mTitle->geometry().center());
            break;
        case 3:
            checkOverlay(mTitle);
            centerCursor();
            break;
        case 4:
            require(!mTitle->isVisible(), "Title did not hide after leaving");
            edgeCursor(2);
            break;
        case 5:
            checkOverlay(mSidebar);
            require(!mTitle->isVisible(), "Right edge unexpectedly revealed the title");
            cursor(mSidebar->geometry().center());
            break;
        case 6:
            checkOverlay(mSidebar);
            centerCursor();
            break;
        case 7:
            require(!mSidebar->isVisible(), "Sidebar did not hide after leaving");
            mChecks.append("Top and right hover, pointer retention, delayed hiding, native overlay stacking");
            key(mDisplay);
            break;
        case 8:
            require(!mRoot->isFullScreen() && mRoot->geometry() == mOriginal, "F11 did not restore normal geometry");
            require(mTitle->isVisible() && mSidebar->isVisible(), "Normal chrome did not return");
            mChecks.append("F11 exit restores normal geometry and chrome");
            mRoot->showMaximized();
            break;
        case 9:
            require(mTitle->isVisible() && mSidebar->isVisible() && mButton->iconType() == Icon::FullScreen,
                "Maximizing unexpectedly entered full-screen mode");
            key(mRoot);
            require(mButton->iconType() == Icon::ExitFullScreen, "F11 on the container did not enter full screen");
            centerCursor();
            break;
        case 10:
            checkFullScreen();
            mRoot->showMinimized();
            break;
        case 11:
            require(mRoot->isMinimized(), "Minimize failed in full screen");
            mRoot->setWindowState(mRoot->windowState() & ~Qt::WindowMinimized);
            mRoot->raise();
            mRoot->activateWindow();
            break;
        case 12:
            checkFullScreen();
            mButton->click();
            break;
        case 13: {
            require(mRoot->isMaximized() && mButton->iconType() == Icon::FullScreen, "Maximized state did not return");
            require(mTitle->isVisible() && mSidebar->isVisible(), "Maximized chrome did not return");
            require(mRoot->geometry() == mRoot->screen()->availableGeometry(), "Maximized bounds changed");
            mChecks.append("F11 entry, minimize/restore full screen, button exit restores maximized state");
            QDialog dialog(mRoot);
            key(&dialog);
            require(mButton->iconType() == Icon::FullScreen, "Dialog F11 changed the main window");
            mRoot->showNormal();
            break;
        }
        case 14:
            require(mRoot->geometry() == mOriginal, "Maximized full-screen cycle lost normal geometry");
            mChecks.append("Dialog isolation and normal geometry preserved after maximized cycle");
            mButton->click();
            centerCursor();
            break;
        case 15: {
            const int edge = mEdgeStep / 3;
            switch (mEdgeStep++ % 3) {
            case 0:
                require(!mTitle->isVisible() && !mSidebar->isVisible(), "Chrome did not hide before edge check");
                edgeCursor(edge);
                break;
            case 1:
            case 2:
                if (edge == 0 || edge >= 5) checkOverlay(mTitle);
                else require(!mTitle->isVisible(), "Right edge unexpectedly revealed the title");
                if (edge <= 4) checkOverlay(mSidebar);
                else require(!mSidebar->isVisible(), "Top edge unexpectedly revealed the sidebar");
                if (mEdgeStep % 3 == 0) centerCursor();
                break;
            }
            if (mEdgeStep == 21) {
                mChecks.append("Exact physical right edge at five heights and both top corners, including hover retention");
                finish();
            } else {
                --mStep;
            }
            break;
        }
        }
    }

    EmulatorContainer* mRoot;
    EmulatorQtWindow* mDisplay;
    QWidget* mTitle;
    QWidget* mSidebar;
    IconButton* mButton;
    QRect mOriginal;
    QPoint mCursor;
    QTimer mTimer;
    QJsonArray mChecks;
    int mStep = 0;
    int mEdgeStep = 0;
};

void startFullScreenGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display) {
    new FullScreenGuiChecks(root, display);
}
}  // namespace kemu
