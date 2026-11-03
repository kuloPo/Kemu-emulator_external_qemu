// Opt-in checks exercise native sizing messages and the real Qt/rendered viewport.
#include "../src/WindowSizing.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTimer>
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
            {"dpr",mRoot->devicePixelRatioF()}, {"viewportWidth",viewport.width()},
            {"viewportHeight",viewport.height()}, {"surfaceWidth",surface.width()},
            {"surfaceHeight",surface.height()}}).toJson());
        mRoot->showNormal();
        mRoot->setGeometry(mOriginal);
        if (qEnvironmentVariableIsSet("KEMU_RESIZE_TEST_EXIT")) mRoot->close();
    }
    void tick() {
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
    QRegion mAllowed;
    QSize mPrevious, mRestored;
    QTimer mTimer;
    QElapsedTimer mElapsed;
    int mCase = 0, mStep = 0, mChecks = 0;
};

void startResizeGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display) {
    new ResizeGuiChecks(root, display);
}
}  // namespace kemu
