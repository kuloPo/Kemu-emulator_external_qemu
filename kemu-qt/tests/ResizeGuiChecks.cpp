// Opt-in checks exercise native sizing messages and the real Qt/rendered viewport.
#include "../src/WindowSizing.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-qt-window.h"
#include <windows.h>

namespace kemu {
class ResizeGuiChecks final : public QObject {
public:
    ResizeGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display)
        : QObject(root), mRoot(root), mDisplay(display), mOriginal(root->geometry()) {
        const QRect work = root->screen()->availableGeometry();
        const QSize size = fitWindowSize(QSizeF(root->size()) * 0.85, display->backingSurfaceSize(),
            QSize(54,42), root->minimumSize(), work.size());
        mBase = QRect(work.center() - QPoint(size.width()/2, size.height()/2), size);
        connect(&mTimer, &QTimer::timeout, this, [this] { tick(); });
        mTimer.start(50);
    }
private:
    HWND handle() const { return reinterpret_cast<HWND>(mRoot->winId()); }
    QRect nativeRect() const {
        RECT rect{};
        GetWindowRect(handle(), &rect);
        return QRect(rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top);
    }
    void finish(const QString& error = {}) {
        mTimer.stop();
        SendMessageW(handle(), WM_EXITSIZEMOVE, 0, 0);
        const QSize viewport = mRoot->viewport()->size(), surface = mDisplay->size();
        QFile file(qEnvironmentVariable("KEMU_RESIZE_TEST_RESULTS"));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
            {"passed",error.isEmpty()}, {"error",error}, {"steps",mChecks},
            {"displayRecoveryChecks",mRecoveryCase},
            {"dpr",mRoot->devicePixelRatioF()}, {"viewportWidth",viewport.width()},
            {"viewportHeight",viewport.height()}, {"surfaceWidth",surface.width()},
            {"surfaceHeight",surface.height()}}).toJson());
        mRoot->showNormal();
        mRoot->setGeometry(mOriginal);
        if (qEnvironmentVariableIsSet("KEMU_RESIZE_TEST_EXIT")) mRoot->close();
    }
    enum RecoveryCase {
        DisplayBeforeResize, DisplayAfterResize, WindowScreenChanged, ScreenGeometryChanged,
        WorkAreaChanged, LogicalDpiChanged, PhysicalDpiChanged, Minimized, Maximized,
        MaximizedScreenChanged, FullScreen, UnchangedOffscreen, FlushEdges, NoDisplayChange,
        ActiveSurfaceUpdates, RecoveryCaseCount
    };
    bool recoveryMaximized() const {
        return mRecoveryCase == Maximized || mRecoveryCase == MaximizedScreenChanged;
    }
    void tickRecovery() {
        if (!mRecoveryStarted) {
            mRecoveryStarted = true;
            mRecoveryRestored = false;
            mRoot->showNormal();
            mRoot->setGeometry(mBase);
            const QSize damaged = mBase.size() + (mRecoveryCase == DisplayAfterResize ? QSize(80,0) : QSize(0,80));
            const QRect work = mRoot->screen()->availableGeometry();
            if (recoveryMaximized()) mRoot->showMaximized();
            else if (mRecoveryCase == FullScreen) mRoot->showFullScreen();
            else if (mRecoveryCase == UnchangedOffscreen)
                mRoot->move(mBase.x(), work.bottom() + 1 - mBase.height()/2);
            else if (mRecoveryCase == FlushEdges)
                mRoot->setGeometry(QRect(work.bottomRight() + QPoint(1,1) - QPoint(damaged.width(), damaged.height()), damaged));
            else if (mRecoveryCase != DisplayBeforeResize) mRoot->resize(damaged);
            if (mRecoveryCase == Minimized) mRoot->showMinimized();
            mRecoveryExpected = mBase;
            if (mRecoveryCase == UnchangedOffscreen || mRecoveryCase == NoDisplayChange)
                mRecoveryExpected = mRoot->geometry();
            else if (mRecoveryCase == FlushEdges)
                mRecoveryExpected.moveBottomRight(mRoot->geometry().bottomRight());
            QScreen* screen = mRoot->screen();
            // A stale minimum would let WM_GETMINMAXINFO grow the maximized window past the work area.
            const QSize minimum = windowSizing(mDisplay->backingSurfaceSize(), screen->availableGeometry().size()).minimum;
            if (mRecoveryCase == MaximizedScreenChanged) mRoot->setMinimumSize(minimum - QSize(1,1));
            switch (mRecoveryCase) {
                case WindowScreenChanged:
                case MaximizedScreenChanged: mRoot->windowHandle()->screenChanged(screen); break;
                case ScreenGeometryChanged: screen->geometryChanged(screen->geometry()); break;
                case WorkAreaChanged: screen->availableGeometryChanged(screen->availableGeometry()); break;
                case LogicalDpiChanged: screen->logicalDotsPerInchChanged(screen->logicalDotsPerInch()); break;
                case PhysicalDpiChanged: screen->physicalDotsPerInchChanged(screen->physicalDotsPerInch()); break;
                case NoDisplayChange: break;
                default:
                    SendMessageW(handle(), WM_DISPLAYCHANGE, 32,
                        MAKELPARAM(qRound(screen->size().width() * mRoot->devicePixelRatioF()),
                                   qRound(screen->size().height() * mRoot->devicePixelRatioF())));
                    break;
            }
            if (mRecoveryCase == MaximizedScreenChanged && mRoot->minimumSize() != minimum) {
                finish("Screen change left a stale minimum size on a maximized window"); return;
            }
            if (mRecoveryCase == DisplayBeforeResize)
                QTimer::singleShot(80, this, [this, damaged] { mRoot->resize(damaged); });
            mElapsed.restart();
            return;
        }
        if (mRecoveryCase == ActiveSurfaceUpdates) mRoot->kemuSurfaceChanged();
        if (mElapsed.elapsed() < 600) return;
        if (mRecoveryCase == Minimized && !mRecoveryRestored) {
            if (!mRoot->isMinimized()) { finish("Display recovery restored a minimized window"); return; }
            mRecoveryRestored = true;
            mRoot->showNormal();
            mElapsed.restart();
            return;
        }
        if ((recoveryMaximized() && !mRoot->isMaximized()) ||
            (mRecoveryCase == FullScreen && !mRoot->isFullScreen())) {
            finish("Display recovery changed the window state"); return;
        }
        if (recoveryMaximized() || mRecoveryCase == FullScreen) {
            MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromWindow(handle(), MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT bounds = recoveryMaximized() ? monitor.rcWork : monitor.rcMonitor;
            const QRect expected(bounds.left, bounds.top, bounds.right-bounds.left, bounds.bottom-bounds.top);
            if (nativeRect() != expected) { finish("Display recovery changed maximized or full-screen bounds"); return; }
        } else if (mRoot->geometry() != mRecoveryExpected) {
            finish(QString("Display recovery changed or failed to repair geometry at case %1").arg(mRecoveryCase)); return;
        }
        if (!recoveryMaximized() && mRecoveryCase != FullScreen && mRecoveryCase != NoDisplayChange) {
            const QSize gap = mRoot->viewport()->size() - mDisplay->size();
            if (qAbs(gap.width()) > 1 || qAbs(gap.height()) > 1) {
                if (mElapsed.elapsed() < 10000) return;
                finish(QString("Display recovery left black bars at case %1").arg(mRecoveryCase)); return;
            }
        }
        ++mRecoveryCase;
        mRecoveryStarted = false;
        mRoot->showNormal();
        mRoot->setGeometry(mBase);
    }
    void tick() {
        if (mRecoveryCase < RecoveryCaseCount) { tickRecovery(); return; }
        static const int positions[] = {0,3,6,9,12,15,12,9,6,3,0,-3,-6,-9,-12,-15,3000,-3000};
        if (mCase >= 48) {
            if (mCase == 48) {
                if (mElapsed.elapsed() > 10000) { finish("Rendered viewport did not settle"); return; }
                const QSize gap = mRoot->viewport()->size() - mDisplay->size();
                if (qAbs(gap.width()) > 1 || qAbs(gap.height()) > 1) return;
                mRoot->showMaximized();
                ++mCase;
                mElapsed.restart();
            } else if (mCase == 49 && mElapsed.elapsed() >= 500) {
                MONITORINFO monitor{sizeof(MONITORINFO)};
                GetMonitorInfoW(MonitorFromWindow(handle(), MONITOR_DEFAULTTONEAREST), &monitor);
                const QSize work(monitor.rcWork.right-monitor.rcWork.left, monitor.rcWork.bottom-monitor.rcWork.top);
                if (!mRoot->isMaximized() || nativeRect().size() != work) {
                    finish("Maximized window did not fill the work area"); return;
                }
                RECT proposed{100,100,1100,900};
                SendMessageW(handle(), WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&proposed));
                if (proposed.left != 100 || proposed.top != 100 || proposed.right != 1100 || proposed.bottom != 900) {
                    finish("Aspect constraint modified a maximized window"); return;
                }
                mRestored = mRoot->normalGeometry().size();
                mRoot->showNormal();
                ++mCase;
                mElapsed.restart();
            } else if (mCase == 50 && mElapsed.elapsed() >= 500) {
                finish(mRoot->size() == mRestored ? QString() : QString("Maximize/restore changed the normal size"));
            }
            return;
        }
        const int edge = (mCase % 24) / 3 + 1, direction = mCase % 3;
        if (mStep == 0) {
            QRect base = mBase;
            if (mCase >= 24) base.moveTop(mRoot->screen()->availableGeometry().bottom() + 1 - base.height() * 2 / 3);
            mRoot->setGeometry(base);
            mStart = nativeRect();
            mPrevious = mStart.size();
            mAllowed = QRegion(mStart);
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                MONITORINFO info{sizeof(MONITORINFO)};
                if (GetMonitorInfoW(monitor, &info))
                    *reinterpret_cast<QRegion*>(data) += QRect(info.rcWork.left, info.rcWork.top,
                        info.rcWork.right-info.rcWork.left, info.rcWork.bottom-info.rcWork.top);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&mAllowed));
            SendMessageW(handle(), WM_ENTERSIZEMOVE, 0, 0);
        }
        const int delta = positions[mStep];
        const int dx = direction == 1 ? 0 : delta, dy = direction == 0 ? 0 : delta;
        RECT rect{mStart.x(), mStart.y(), mStart.x()+mStart.width(), mStart.y()+mStart.height()};
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) rect.left -= dx;
        if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT) rect.right += dx;
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) rect.top -= dy;
        if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) rect.bottom += dy;
        SendMessageW(handle(), WM_SIZING, edge, reinterpret_cast<LPARAM>(&rect));
        const QSize constrained(rect.right-rect.left, rect.bottom-rect.top);
        const bool left = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
        const bool top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
        if ((left ? rect.right != mStart.right()+1 : rect.left != mStart.left()) ||
            (top ? rect.bottom != mStart.bottom()+1 : rect.top != mStart.top())) {
            finish("Resize moved an anchored edge"); return;
        }
        if (delta == 0 && constrained != mStart.size()) { finish("Resize changed the starting size"); return; }
        if (mStep > 0) {
            const int sign = delta >= positions[mStep-1] ? 1 : -1;
            if (sign * (constrained.width()-mPrevious.width()) < 0 || sign * (constrained.height()-mPrevious.height()) < 0) {
                finish(QString("Resize reversed direction at case %1 step %2").arg(mCase).arg(mStep)); return;
            }
        }
        SetWindowPos(handle(), nullptr, rect.left, rect.top, constrained.width(), constrained.height(), SWP_NOZORDER | SWP_NOACTIVATE);
        if (nativeRect().size() != constrained) { finish("Windows clipped the constrained size"); return; }
        if (!QRegion(nativeRect()).subtracted(mAllowed).isEmpty()) { finish("Resize added off-screen pixels"); return; }
        const QSize viewport = mRoot->viewport()->size(), display = mDisplay->backingSurfaceSize();
        if (qAbs(qint64(viewport.width())*display.height() - qint64(viewport.height())*display.width()) >
            qMax(display.width(),display.height())/2) { finish("Qt viewport lost the display aspect ratio"); return; }
        mPrevious = constrained;
        ++mChecks;
        if (++mStep == sizeof(positions)/sizeof(positions[0])) {
            SendMessageW(handle(), WM_EXITSIZEMOVE, 0, 0);
            mStep = 0;
            ++mCase;
            if (mCase == 48) mRoot->setGeometry(mBase);
            mElapsed.restart();
        }
    }
    EmulatorContainer* mRoot;
    EmulatorQtWindow* mDisplay;
    QRect mOriginal, mBase, mStart;
    QRect mRecoveryExpected;
    QRegion mAllowed;
    QSize mPrevious, mRestored;
    QTimer mTimer;
    QElapsedTimer mElapsed;
    int mCase = 0, mStep = 0, mChecks = 0;
    int mRecoveryCase = 0;
    bool mRecoveryStarted = false, mRecoveryRestored = false;
};

void startResizeGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display) {
    new ResizeGuiChecks(root, display);
}
}  // namespace kemu
