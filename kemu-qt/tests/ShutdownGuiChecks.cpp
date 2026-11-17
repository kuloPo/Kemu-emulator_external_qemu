// Opt-in integration trigger for the production close button and restart flow.
#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QDialog>
#include <QFile>
#include <QMessageBox>
#include <QTimer>
#include <windows.h>
#include "android/skin/qt/emulator-container.h"

namespace kemu {
void startShutdownGuiChecks(EmulatorContainer* root) {
    const auto path = qEnvironmentVariable("KEMU_SHUTDOWN_TEST_REQUEST");
    auto* timer = new QTimer(root);
    QObject::connect(timer, &QTimer::timeout, root, [root, timer, path] {
        QFile request(path);
        if (!request.open(QIODevice::ReadOnly)) return;
        const auto fields = request.readAll().simplified().split(' ');
        if (fields.size() != 2 || fields[0].toUInt() != GetCurrentProcessId()) return;
        timer->stop();
        const auto report = [path](const QByteArray& text) {
            QFile result(path + ".result");
            if (result.open(QIODevice::WriteOnly)) result.write(text);
        };
        if (fields[1] == "close") {
            auto* close = root->findChild<QAbstractButton*>("KemuClose");
            if (!close || !close->isEnabled()) { report("Close unavailable"); return; }
            report("Close invoked");
            close->click();
            return;
        }
        if (fields[1] != "restart") { report("Invalid action"); return; }
        auto* settings = root->findChild<QAbstractButton*>("KemuSettingsButton");
        if (!settings || !settings->isEnabled()) { report("Settings unavailable"); return; }
        settings->click();
        QDialog* dialog = nullptr;
        for (auto* window : QApplication::topLevelWidgets()) {
            if (window->objectName() == "KemuSettings") dialog = qobject_cast<QDialog*>(window);
        }
        auto* resolution = dialog ? dialog->findChild<QButtonGroup*>("KemuResolution") : nullptr;
        if (!resolution || resolution->buttons().size() < 2) { report("Resolution unavailable"); return; }
        const int next = (resolution->checkedId() + 1) % resolution->buttons().size();
        resolution->button(next)->click();
        auto* accept = new QTimer(root);
        QObject::connect(accept, &QTimer::timeout, root, [accept, report] {
            for (auto* window : QApplication::topLevelWidgets()) {
                auto* prompt = qobject_cast<QMessageBox*>(window);
                if (!prompt || prompt->objectName() != "KemuPerformanceRestart") continue;
                for (auto* button : prompt->buttons()) {
                    if (prompt->buttonRole(button) == QMessageBox::AcceptRole) {
                        accept->stop();
                        report("Restart now invoked");
                        button->click();
                        return;
                    }
                }
            }
        });
        accept->start(20);
        dialog->close();
    });
    timer->start(50);
}
}  // namespace kemu
