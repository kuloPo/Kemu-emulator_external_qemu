// Opt-in integration checks using the real sidebar and guest sensor events.
#include <QAbstractButton>
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
#include <cmath>
#include "IconButton.h"
#include "android/console.h"
#include "android/hw-sensors.h"
#include "host-common/hw-config.h"
#include "android/emulation/control/adb/AdbInterface.h"
#include "android/skin/qt/emulator-container.h"
#include "android/skin/qt/emulator-qt-window.h"
#include <windows.h>

namespace kemu {
class SensorRotationGuiChecks final : public QObject {
public:
    SensorRotationGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display)
        : QObject(root), mRoot(root), mDisplay(display) {
        mButton = root->findChild<IconButton*>("KemuRotateDevice");
        mSerial = QString::fromStdString(display->getAdbInterface()->serialString());
        connect(&mTimer, &QTimer::timeout, this, [this] {
            try { tick(); } catch (const QString& error) { finish(error); }
        });
        mTimer.start(200);
    }
private:
    QString adb(const QStringList& args) {
        QProcess process;
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
        process.start(qEnvironmentVariable("KEMU_TEST_ADB"), QStringList{"-s", mSerial} + args);
        if (!process.waitForFinished(20000)) {
            process.kill(); process.waitForFinished(); throw QString("ADB timed out");
        }
        if (process.exitCode()) throw QString("ADB failed: ") + process.readAllStandardError();
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }
    void setting(const QString& key, const QString& value) {
        if (value == "null") adb({"shell", "settings", "delete", "system", key});
        else adb({"shell", "settings", "put", "system", key, value});
    }
    void finish(QString error = {}) {
        mTimer.stop();
        if (mSaved) {
            getConsoleAgents()->sensors->setPhysicalParameterTarget(
                PHYSICAL_PARAMETER_ROTATION, mOriginalPose, 3, PHYSICAL_INTERPOLATION_STEP);
            mButton->setIconType(mInitialPortrait ? Icon::RotatePortrait : Icon::RotateLandscape);
            try {
                setting("accelerometer_rotation", mAutoRotate);
                setting("user_rotation", mUserRotation);
                adb({"shell", "am", "force-stop", "org.kemu.test.rotation"});
            } catch (const QString& cleanupError) { error += " Cleanup: " + cleanupError; }
        }
        QFile file(qEnvironmentVariable("KEMU_SENSOR_ROTATION_TEST_RESULTS"));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
            {"passed", error.isEmpty()}, {"error", error}, {"initialPortrait", mInitialPortrait}, {"checks", mChecks}}).toJson());
    }
    void startMode() {
        setting("accelerometer_rotation", mMode == 1 ? "0" : "1");
        setting("user_rotation", "0");
        const int orientations[] = {10, 2, 1, 0, 10};
        adb({"shell", "am", "force-stop", "org.kemu.test.rotation"});
        adb({"shell", "am", "start", "-W", "-n", "org.kemu.test.rotation/.RotationActivity",
            "--ei", "orientation", QString::number(orientations[mMode])});
        mClicks = 0;
        mWaitingForBaseline = true;
        mElapsed.restart();
    }
    QString marker() const { return QString("SensorRotation%1_%2").arg(mMode).arg(mClicks); }
    void click() {
        adb({"shell", "log", "-t", "RotationProbe", marker()});
        const int count = mMode == 4 ? 3 : 1;
        for (int i = 0; i < count; ++i) {
            mButton->click();
            mRotated = !mRotated;
            float x = 0.f, y = 0.f, z = 0.f;
            float* out[] = {&x, &y, &z};
            if (getConsoleAgents()->sensors->getPhysicalParameter(PHYSICAL_PARAMETER_ROTATION,
                    out, 3, PARAMETER_VALUE_TYPE_TARGET) != PHYSICAL_PARAMETER_STATUS_OK ||
                std::abs(x) > .01f || std::abs(y) > .01f || std::abs(z - (mRotated ? 90.f : 0.f)) > .01f)
                throw QString("Button did not toggle between 0 and 90 degrees");
            checkIcon();
        }
        mTouched = false;
        mElapsed.restart();
    }
    void checkIcon() {
        const bool portrait = mInitialPortrait != mRotated;
        if (mButton->iconType() != (portrait ? Icon::RotatePortrait : Icon::RotateLandscape))
            throw QString("Rotation icon does not match the simulated posture");
    }
    void tick() {
        if (!mSaved) {
            if (!mButton || !mButton->isEnabled() || !mButton->isVisible()) throw QString("Rotation button unavailable");
            const auto* hardware = getConsoleAgents()->settings->hw();
            mInitialPortrait = qstrcmp(hardware->hw_initialOrientation, "portrait") == 0;
            checkIcon();
            mButton->grab().save(qEnvironmentVariable("KEMU_SENSOR_ROTATION_TEST_RESULTS") + ".initial-button.png");
            const auto apk = qEnvironmentVariable("KEMU_SENSOR_ROTATION_TEST_APK");
            if (!apk.isEmpty()) adb({"install", "-r", apk});
            mAutoRotate = adb({"shell", "settings", "get", "system", "accelerometer_rotation"});
            mUserRotation = adb({"shell", "settings", "get", "system", "user_rotation"});
            float* out[] = {&mOriginalPose[0], &mOriginalPose[1], &mOriginalPose[2]};
            if (getConsoleAgents()->sensors->getPhysicalParameter(PHYSICAL_PARAMETER_ROTATION,
                    out, 3, PARAMETER_VALUE_TYPE_TARGET) != PHYSICAL_PARAMETER_STATUS_OK)
                throw QString("Cannot save physical pose");
            mSaved = true;
            const float upright[] = {0.f, 0.f, 0.f};
            getConsoleAgents()->sensors->setPhysicalParameterTarget(
                PHYSICAL_PARAMETER_ROTATION, upright, 3, PHYSICAL_INTERPOLATION_STEP);
            startMode();
            return;
        }
        if (mElapsed.elapsed() > 12000) throw QString("Sensor rotation timed out in mode %1, click %2").arg(mMode).arg(mClicks);
        if (mElapsed.elapsed() < 1500) return;
        QString logs = adb({"logcat", "-d", "-s", "RotationProbe:I", "*:S"});
        if (!mWaitingForBaseline) logs = logs.mid(logs.lastIndexOf(marker()));
        const QRegularExpression pattern("STATE (\\S+) (\\S+) (\\S+) GYRO (\\S+) ROT ([0-3]) SIZE (\\d+) (\\d+)");
        auto matches = pattern.globalMatch(logs);
        QRegularExpressionMatch state;
        float gyroPeak = 0.f;
        while (matches.hasNext()) {
            state = matches.next();
            gyroPeak = qMax(gyroPeak, state.captured(4).toFloat());
        }
        if (!state.hasMatch()) return;
        const int rotation = state.captured(5).toInt();
        if (mWaitingForBaseline) {
            mBaselineRotation = rotation;
            mWaitingForBaseline = false;
            click();
            return;
        }
        const double angle = mRotated ? 3.141592653589793 / 2 : 0;
        if (std::abs(state.captured(1).toFloat() - 9.81 * std::sin(angle)) > .5 ||
            std::abs(state.captured(2).toFloat() - 9.81 * std::cos(angle)) > .5 ||
            std::abs(state.captured(3).toFloat()) > .5 || gyroPeak < .1f) return;
        const int expected = (mMode == 0 || mMode == 4)
            ? (mBaselineRotation + int(mRotated)) % 4 : mBaselineRotation;
        if (rotation != expected) return;
        const QSize guest(state.captured(6).toInt(), state.captured(7).toInt());
        const QSize fitted = guest.scaled(mRoot->viewport()->size(), Qt::KeepAspectRatio);
        if (qAbs(fitted.width() - mDisplay->width()) > 2 || qAbs(fitted.height() - mDisplay->height()) > 2)
            throw QString("Host display did not follow the guest orientation");
        if (!mTouched) {
            const QPointF local(mDisplay->width() * .25, mDisplay->height() * .25);
            const QPointF global(mDisplay->mapToGlobal(local.toPoint()));
            QMouseEvent down(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent up(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(mDisplay, &down);
            QApplication::sendEvent(mDisplay, &up);
            mTouched = true;
            return;
        }
        const auto touch = QRegularExpression("TOUCH ([0-9.]+) ([0-9.]+)").match(logs);
        if (!touch.hasMatch()) return;
        if (qAbs(touch.captured(1).toDouble() - .25) > .05 || qAbs(touch.captured(2).toDouble() - .25) > .05)
            throw QString("Touch coordinates did not follow the guest orientation: ") + touch.captured();
        if (!mButton->parentWidget()->rect().contains(mButton->geometry())) throw QString("Rotation button clipped");
        checkIcon();
        if (adb({"shell", "settings", "get", "system", "accelerometer_rotation"}) != (mMode == 1 ? "0" : "1") ||
            adb({"shell", "settings", "get", "system", "user_rotation"}) != "0") throw QString("Rotation button changed Android rotation settings");
        mChecks.append(QJsonObject{{"mode", mMode}, {"click", mClicks}, {"state", state.captured()},
            {"gyroPeak", gyroPeak}, {"touch", touch.captured()}, {"portraitIcon", mInitialPortrait != mRotated}});
        mRoot->screen()->grabWindow(mRoot->winId()).save(qEnvironmentVariable("KEMU_SENSOR_ROTATION_TEST_RESULTS") + QString(".%1.%2.png").arg(mMode).arg(mClicks));
        mButton->grab().save(qEnvironmentVariable("KEMU_SENSOR_ROTATION_TEST_RESULTS") + QString(".%1.%2.button.png").arg(mMode).arg(mClicks));
        if (++mClicks < (mMode == 4 ? 1 : 4)) click();
        else if (++mMode < 5) startMode();
        else finish();
    }
    EmulatorContainer* mRoot;
    EmulatorQtWindow* mDisplay;
    IconButton* mButton = nullptr;
    QTimer mTimer;
    QElapsedTimer mElapsed;
    QString mSerial, mAutoRotate, mUserRotation;
    QJsonArray mChecks;
    float mOriginalPose[3]{};
    int mMode = 0, mClicks = 0, mBaselineRotation = 0;
    bool mInitialPortrait = false, mRotated = false;
    bool mSaved = false, mWaitingForBaseline = false, mTouched = false;
};
void startSensorRotationGuiChecks(EmulatorContainer* root, EmulatorQtWindow* display) {
    new SensorRotationGuiChecks(root, display);
}
}  // namespace kemu
