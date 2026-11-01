// Opt-in integration test for the actual settings-save and restart path.
#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QMessageBox>
#include <QSpinBox>
#include <QTimer>
#include <windows.h>
#include "android/skin/qt/emulator-container.h"

namespace kemu {
void startAffinityGuiChecks(EmulatorContainer* root) {
    const auto requestPath = qEnvironmentVariable("KEMU_AFFINITY_TEST_REQUEST");
    auto* timer = new QTimer(root);
    QObject::connect(timer, &QTimer::timeout, root, [root, timer, requestPath] {
        QFile request(requestPath);
        if (!request.open(QIODevice::ReadOnly)) return;
        const auto fields = request.readAll().simplified().split(' ');
        if (fields.size() != 2 || fields[0].toUInt() != GetCurrentProcessId()) return;
        timer->stop();
        const auto report = [requestPath](const QByteArray& text) {
            QFile result(requestPath + ".result");
            if (result.open(QIODevice::WriteOnly)) result.write(text);
        };
        auto* settings = root->findChild<QAbstractButton*>("KemuSettingsButton");
        if (!settings || !settings->isEnabled()) { report("Settings unavailable"); return; }
        settings->click();
        QDialog* dialog = nullptr;
        for (auto* window : QApplication::topLevelWidgets()) {
            if (window->objectName() == "KemuSettings") dialog = qobject_cast<QDialog*>(window);
        }
        auto* cores = dialog ? dialog->findChild<QSpinBox*>("KemuCpu") : nullptr;
        const int count = fields[1].toInt();
        if (!cores || count < cores->minimum() || count > cores->maximum() || count == cores->value()) {
            report("Invalid CPU change"); return;
        }
        cores->setValue(count);
        auto* accept = new QTimer(root);
        QObject::connect(accept, &QTimer::timeout, root, [accept, report] {
            for (auto* window : QApplication::topLevelWidgets()) {
                auto* prompt = qobject_cast<QMessageBox*>(window);
                if (!prompt || prompt->objectName() != "KemuPerformanceRestart") continue;
                for (auto* button : prompt->buttons()) {
                    if (prompt->buttonRole(button) == QMessageBox::AcceptRole) {
                        accept->stop(); report("Restart now invoked"); button->click(); return;
                    }
                }
            }
        });
        accept->start(20);
        dialog->close();
    });
    timer->start(500);
}
} // namespace kemu
