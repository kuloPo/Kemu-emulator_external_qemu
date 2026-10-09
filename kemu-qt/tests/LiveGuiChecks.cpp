// Opt-in checks run inside the real emulator UI thread, using the real GPU
// child window. Not included in the normal release build.
#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/tool-window.h"
#include <windows.h>

namespace kemu {
class LiveGuiChecks final : public QObject {
public:
    LiveGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display)
        : QObject(root), mRoot(root), mDisplay(display) {
        mBefore = root->size();
        mElapsed.start();
        connect(&mTimer, &QTimer::timeout, this, [this] { tick(); });
        mTimer.start(16);
    }
private:
    QAbstractButton* button(const QString& name) {
        for (auto* button : mRoot->findChildren<QAbstractButton*>())
            if (button->accessibleName() == name && button->isVisible()) return button;
        fail("Missing visible button: " + name);
        return nullptr;
    }
    bool fitted() {
        HWND render = nullptr;
        EnumChildWindows(reinterpret_cast<HWND>(mRoot->winId()), [](HWND child, LPARAM out) -> BOOL {
            wchar_t name[80]{};
            GetClassNameW(child, name, 80);
            if (wcscmp(name, L"subWin") == 0) { *reinterpret_cast<HWND*>(out) = child; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&render));
        if (!render) return false;
        DWORD renderPid = 0;
        GetWindowThreadProcessId(render, &renderPid);
        if (renderPid != GetCurrentProcessId()) return false;
        RECT rect{};
        GetWindowRect(render, &rect);
        const double dpr = mRoot->devicePixelRatioF();
        const QPoint origin = mDisplay->mapToGlobal(QPoint());
        const QSize expected = QSize(1920,1080).scaled(mRoot->viewport()->size(), Qt::KeepAspectRatio);
        mGeometry = QJsonObject{{"renderX",int(rect.left)}, {"renderY",int(rect.top)},
            {"renderWidth",int(rect.right-rect.left)}, {"renderHeight",int(rect.bottom-rect.top)},
            {"expectedX",qRound(origin.x()*dpr)}, {"expectedY",qRound(origin.y()*dpr)},
            {"expectedWidth",qRound(expected.width()*dpr)}, {"expectedHeight",qRound(expected.height()*dpr)}};
        return qAbs(mDisplay->width() - expected.width()) <= 1 && qAbs(mDisplay->height() - expected.height()) <= 1 &&
            qAbs(rect.left - qRound(origin.x()*dpr)) <= 2 && qAbs(rect.top - qRound(origin.y()*dpr)) <= 2 &&
            qAbs((rect.right-rect.left) - qRound(expected.width()*dpr)) <= 3 &&
            qAbs((rect.bottom-rect.top) - qRound(expected.height()*dpr)) <= 3;
    }
    QString adb(QStringList arguments) {
        QProcess process;
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
        process.start(qEnvironmentVariable("KEMU_TEST_ADB"), QStringList{"-s","emulator-5554"} + arguments);
        if (!process.waitForFinished(10000)) { process.kill(); process.waitForFinished(); fail("ADB observation timed out"); return {}; }
        if (process.exitCode()) fail("ADB observation failed");
        return QString::fromUtf8(process.readAllStandardOutput());
    }
    QString muted() {
        return QRegularExpression("STREAM_MUSIC:[\\s\\S]*?Muted: (true|false)").match(adb({"shell","dumpsys","audio"})).captured(1);
    }
    void fail(const QString& error) {
        if (mFailed) return;
        mFailed = true;
        mTimer.stop();
        mRoot->screen()->grabWindow(mRoot->winId()).save(qEnvironmentVariable("KEMU_GUI_TEST_RESULTS") + ".png");
        save(error);
    }
    void save(const QString& error = {}) {
        QJsonObject data{{"passed",error.isEmpty()}, {"error",error}, {"qt",qVersion()},
            {"pid",qint64(GetCurrentProcessId())}, {"checks",mChecks}, {"geometry",mGeometry}};
        QFile file(qEnvironmentVariable("KEMU_GUI_TEST_RESULTS"));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(data).toJson());
    }
    void advance() { ++mStage; mElapsed.restart(); mStable=0; mFirstFit=-1; }
    void doubleClickTitle() {
        auto* title = mRoot->findChild<QWidget*>("KemuTitleBar");
        if (!title) { fail("Missing title drag surface"); return; }
        const QPointF local(150,20);
        QMouseEvent event(QEvent::MouseButtonDblClick, local, title->mapToGlobal(local.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(title, &event);
    }
    void tick() {
        if (mFailed) return;
        if (mElapsed.elapsed() > 5000) { fail(QString("Stage %1 timed out; surface %2x%3, viewport %4x%5")
            .arg(mStage).arg(mDisplay->width()).arg(mDisplay->height()).arg(mRoot->viewport()->width()).arg(mRoot->viewport()->height())); return; }
        if (mStage <= 7) {
            if (!fitted()) {
                if (mStable) fail("GPU geometry oscillated after settling");
                return;
            }
            if (mFirstFit < 0) mFirstFit = mElapsed.elapsed();
            if (++mStable < 40) return;
            mChecks.append(QJsonObject{{"stage",mStage}, {"firstFitMs",mFirstFit}, {"stableSamples",mStable},
                {"width",mRoot->width()}, {"height",mRoot->height()}});
            if (mStage == 0) {
                if (mRoot->windowTitle() != "Kemu" || mDisplay->toolWindow()->isVisible()) { fail("Old chrome remains"); return; }
                for (const auto* name : {"KemuTitleLogo", "KemuLoadingLogo"}) {
                    const auto* logo = mRoot->findChild<QLabel*>(name);
                    if (!logo || logo->pixmap().isNull()) { fail(QString("Missing logo: %1").arg(name)); return; }
                }
                const auto* loadingText = mRoot->findChild<QLabel*>("KemuLoadingText");
                if (!loadingText || QFontInfo(loadingText->font()).family() != "Microsoft YaHei UI") {
                    fail("Startup font did not resolve to Microsoft YaHei UI"); return;
                }
                mChecks.append("title/startup logos loaded; startup font resolved to Microsoft YaHei UI");
                if (auto* b = button(QStringLiteral("最大化"))) b->click();
            } else if (mStage == 1) {
                if (!mRoot->isMaximized()) { fail("Maximize state did not update"); return; }
                if (auto* b = button(QStringLiteral("还原"))) b->click();
            } else if (mStage == 2) {
                if (mRoot->isMaximized() || mRoot->size() != mBefore) { fail("Restore geometry mismatch"); return; }
                mRoot->resize(1189,762);
            } else if (mStage == 3) mRoot->resize(900,520);
            else if (mStage == 4) mRoot->resize(1510,930);
            else if (mStage == 5) mRoot->resize(1001,611);
            else if (mStage == 6) mRoot->resize(mBefore);
            else {
                mRoot->screen()->grabWindow(mRoot->winId()).save(qEnvironmentVariable("KEMU_GUI_TEST_RESULTS") + ".png");
                if (auto* b = button(QStringLiteral("最小化"))) b->click();
            }
            advance();
        } else if (mStage == 8) {
            if (!mRoot->isMinimized()) { fail("Minimize failed"); return; }
            mRoot->showNormal();
            advance();
        } else if (mStage == 9) {
            if (!fitted()) return;
            if (auto* b = button(QStringLiteral("Home"))) b->click();
            advance();
        } else if (mElapsed.elapsed() > 500) {
            if (mStage == 10) {
                const QString activity = adb({"shell","dumpsys","activity","activities"});
                if (!QRegularExpression("(?:mResumedActivity|topResumedActivity)[^\\n]*launcher", QRegularExpression::CaseInsensitiveOption).match(activity).hasMatch()) {
                    fail("Home did not resume launcher"); return;
                }
                if (auto* b = button(QStringLiteral("任务切换"))) b->click();
            } else if (mStage == 11) { if (auto* b = button(QStringLiteral("返回"))) b->click(); }
            else if (mStage == 12) { if (auto* b = button(QStringLiteral("音量＋"))) b->click(); }
            else if (mStage == 13) { if (auto* b = button(QStringLiteral("音量－"))) b->click(); }
            else if (mStage == 14) {
                mMutedBefore = muted();
                if (mMutedBefore.isEmpty()) { fail("Cannot observe Android mute state"); return; }
                if (auto* b = button(QStringLiteral("静音／取消静音"))) b->click();
            } else if (mStage == 15) {
                auto* b = button(QStringLiteral("静音／取消静音"));
                if (!b || !b->isEnabled() || muted() == mMutedBefore) return;
                b->click();
            } else if (mStage == 16) {
                auto* b = button(QStringLiteral("静音／取消静音"));
                if (!b || !b->isEnabled() || muted() != mMutedBefore) return;
                mChecks.append("minimize/restore; six internal key buttons; Home resumed launcher; mute toggled and restored");
                doubleClickTitle();
            } else if (mStage == 17) {
                if (!mRoot->isMaximized() || !fitted()) { fail("Title double-click did not maximize"); return; }
                doubleClickTitle();
            } else {
                if (mRoot->isMaximized() || !fitted()) { fail("Title double-click did not restore"); return; }
                mChecks.append("title double-click maximized and restored");
                mTimer.stop();
                save();
                QTimer::singleShot(200, mRoot, [this] { mRoot->close(); });
                return;
            }
            advance();
        }
    }
    EmulatorContainer* mRoot;
    EmulatorQtWindow* mDisplay;
    QTimer mTimer;
    QElapsedTimer mElapsed;
    QSize mBefore;
    QJsonArray mChecks;
    QJsonObject mGeometry;
    QString mMutedBefore;
    int mStage=0, mStable=0, mFirstFit=-1;
    bool mFailed=false;
};
void startLiveGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display) {
    new LiveGuiChecks(root, display);
}
}  // namespace kemu
