// Requires the rotation probe APK and KEMU_TEST_ADB. Enabled only in test builds.
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-qt-window.h"
#include <windows.h>

namespace kemu {
class RotationGuiChecks final : public QObject {
public:
    RotationGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display)
        : QObject(root), mRoot(root), mDisplay(display) {
        connect(&mTimer, &QTimer::timeout, this, [this] { tick(); });
        startStep();
        mTimer.start(100);
    }
private:
    QString adb(const QStringList& args) {
        QProcess process;
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
        process.start(qEnvironmentVariable("KEMU_TEST_ADB"), QStringList{"-s", "emulator-5554"} + args);
        if (!process.waitForFinished(10000)) { process.kill(); process.waitForFinished(); finish("ADB timed out"); }
        return QString::fromUtf8(process.readAllStandardOutput());
    }
    void finish(const QString& error = {}) {
        mTimer.stop();
        mFinished = true;
        QFile file(qEnvironmentVariable("KEMU_ROTATION_TEST_RESULTS"));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
            {"passed",error.isEmpty()}, {"error",error}, {"checks",mChecks}}).toJson());
    }
    void startStep() {
        if (mStep == 4 || mStep == 9) mRoot->showMaximized();
        if (mStep == 6 || mStep == 12) mRoot->showNormal();
        if (mStep == 13) mRoot->showMinimized();
        adb({"shell", "am", "force-stop", "org.kemu.test.rotation"});
        adb({"shell", "am", "start", "-n", "org.kemu.test.rotation/.RotationActivity", "--ei", "orientation", QString::number(mOrientations[mStep])});
        mStable = 0;
        mTouched = false;
        mElapsed.restart();
    }
    void tick() {
        if (mFinished) return;
        if (mElapsed.elapsed() > 12000) { finish(QString("Rotation step %1 timed out").arg(mStep)); return; }
        if (mStep == 13 && mRoot->isMinimized()) {
            if (mElapsed.elapsed() >= 1500) mRoot->showNormal();
            return;
        }
        const bool portrait = mOrientations[mStep] == 1 || mOrientations[mStep] == 9;
        const QSize expected = (portrait ? QSize(1080,1920) : QSize(1920,1080)).scaled(mRoot->viewport()->size(), Qt::KeepAspectRatio);
        if (qAbs(mDisplay->width()-expected.width()) > 2 || qAbs(mDisplay->height()-expected.height()) > 2) { mStable=0; return; }
        if (++mStable < 12) return;
        if (!mTouched) {
            const bool maximized = mStep == 4 || mStep == 5 || (mStep >= 9 && mStep <= 11);
            if (mRoot->isMaximized() != maximized) { finish("Rotation changed the maximize state"); return; }
            if (!maximized && (mRoot->width() < mRoot->height()) != portrait) { finish("Window did not follow the display orientation"); return; }
            if (mStep == 0) mPortraitGeometry = mRoot->geometry();
            if (mStep == 1) mLandscapeGeometry = mRoot->geometry();
            if (!maximized && mStep >= 6 &&
                mRoot->geometry() != (portrait ? mPortraitGeometry : mLandscapeGeometry)) {
                finish(QString("Restored geometry drifted at step %1: %2x%3")
                    .arg(mStep).arg(mRoot->width()).arg(mRoot->height())); return;
            }
            adb({"shell", "log", "-t", "RotationProbe", QString("RotationStep%1").arg(mStep)});
            const QPointF local(mDisplay->width() * .25, mDisplay->height() * .25);
            const QPointF global(mDisplay->mapToGlobal(local.toPoint()));
            QMouseEvent down(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent up(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(mDisplay, &down);
            QApplication::sendEvent(mDisplay, &up);
            mRoot->screen()->grabWindow(mRoot->winId()).save(qEnvironmentVariable("KEMU_ROTATION_TEST_RESULTS") + QString(".%1.png").arg(mStep));
            mTouched = true;
            mStable = 0;
            return;
        }
        const QString logs = adb({"logcat", "-d", "-s", "RotationProbe:I", "*:S"});
        const auto match = QRegularExpression("TOUCH ([0-9.]+) ([0-9.]+)").match(logs.mid(logs.lastIndexOf(QString("RotationStep%1").arg(mStep))));
        if (!match.hasMatch()) return;
        if (qAbs(match.captured(1).toDouble()-.25) > .05 || qAbs(match.captured(2).toDouble()-.25) > .05) {
            finish("Touch coordinates did not follow the rotated display: " + match.captured()); return;
        }
        mChecks.append(QJsonObject{{"orientation",mOrientations[mStep]}, {"maximized",mRoot->isMaximized()},
            {"width",mRoot->width()}, {"height",mRoot->height()}, {"touch",match.captured()}});
        if (++mStep == sizeof(mOrientations) / sizeof(mOrientations[0])) { finish(); return; }
        startStep();
    }
    EmulatorContainer* mRoot;
    EmulatorQtWindow* mDisplay;
    QTimer mTimer;
    QElapsedTimer mElapsed;
    QJsonArray mChecks;
    QRect mPortraitGeometry;
    QRect mLandscapeGeometry;
    int mOrientations[15] = {1,0,9,8,0,1,1,0,1,0,1,0,0,1,0};
    int mStep=0, mStable=0;
    bool mTouched=false, mFinished=false;
};
void startRotationGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display) {
    new RotationGuiChecks(root, display);
}
}  // namespace kemu
