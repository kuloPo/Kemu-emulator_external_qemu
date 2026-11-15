// Run consecutive Kemu.exe launches with KEMU_MUTE_TEST_EXPECTED=true/false.
// KEMU_MUTE_TEST_TOGGLE=1 clicks the sidebar button before closing the emulator.
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include "android/skin/qt/emulator-container.h"
#include <windows.h>

namespace kemu {
class MuteGuiChecks final : public QObject {
public:
    explicit MuteGuiChecks(EmulatorContainer* root) : QObject(root), mRoot(root) {
        mExpected = qEnvironmentVariable("KEMU_MUTE_TEST_EXPECTED");
        mToggle = qEnvironmentVariable("KEMU_MUTE_TEST_TOGGLE") == "1";
        mElapsed.start();
        connect(&mTimer, &QTimer::timeout, this, [this] { tick(); });
        mTimer.start(250);
    }
private:
    void finish(const QString& error = {}) {
        mTimer.stop();
        QFile file(qEnvironmentVariable("KEMU_MUTE_TEST_RESULTS"));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(QJsonObject{{"passed", error.isEmpty()},
                {"error", error}, {"restored", mExpected}, {"toggled", mToggle},
                {"volumes", mVolumes}}).toJson());
        }
        if (error.isEmpty()) QTimer::singleShot(200, mRoot, &QWidget::close);
    }
    void tick() {
        if (mElapsed.elapsed() > 30000) { finish("Mute restore or toggle timed out"); return; }
        QAbstractButton* mute = nullptr;
        for (auto* button : mRoot->findChildren<QAbstractButton*>()) {
            if (button->property("kemuLabel").toString() == "Mute / Unmute") mute = button;
        }
        if (!mute) { finish("Missing mute button"); return; }
        if (!mute->isEnabled()) return;
        QProcess process;
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NO_WINDOW;
        });
        process.start(qEnvironmentVariable("KEMU_TEST_ADB"),
                      {"-s", "emulator-5554", "shell", "dumpsys", "audio"});
        if (!process.waitForFinished(10000) || process.exitCode()) {
            process.kill();
            process.waitForFinished();
            finish("Cannot observe Android audio state");
            return;
        }
        const QString audio = QString::fromUtf8(process.readAllStandardOutput());
        const QString section = QRegularExpression("- STREAM_MUSIC:([^\\r]*?)(?:\\n\\n|$)")
            .match(QString(audio).remove('\r')).captured(1);
        const QString muted = QRegularExpression("Muted: (true|false)").match(section).captured(1);
        const QString volumes = QRegularExpression("Current: ([^\\n]+)").match(section).captured(1);
        const QString expected = mClicked ? (mExpected == "true" ? "false" : "true") : mExpected;
        if (muted != expected || volumes.isEmpty()) {
            finish("Unexpected Android media state: " + muted + ", expected " + expected);
            return;
        }
        if (!mClicked) mVolumes = volumes;
        if (volumes != mVolumes) { finish("Mute changed the saved volume"); return; }
        if (mToggle && !mClicked) {
            mClicked = true;
            mute->click();
            return;
        }
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
        if (!settings.contains("audio/mediaMuted") ||
            settings.value("audio/mediaMuted").toBool() != (expected == "true")) {
            finish(QString("Saved mute preference does not match Android: file=%1, value=%2, status=%3, button=%4")
                .arg(settings.fileName(), settings.value("audio/mediaMuted").toString())
                .arg(int(settings.status())).arg(mute->toolTip()));
            return;
        }
        finish();
    }
    EmulatorContainer* mRoot;
    QTimer mTimer;
    QElapsedTimer mElapsed;
    QString mExpected;
    QString mVolumes;
    bool mToggle = false;
    bool mClicked = false;
};

void startMuteGuiChecks(EmulatorContainer* root) { new MuteGuiChecks(root); }
}  // namespace kemu
