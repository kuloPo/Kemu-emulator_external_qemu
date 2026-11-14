// Set KEMU_FONT_TEST_RESULTS to a JSON path in a KEMU_GUI_TESTS build,
// then launch Kemu.exe to check fonts and capture tooltip renderings.
#include "../src/LegalInfoDialog.h"
#include "../src/StorageMoveDialog.h"
#include "android/skin/qt/emulator-container.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QToolTip>
#include <stdexcept>

namespace kemu {
void startFontGuiChecks(EmulatorContainer* container) {
    QJsonArray checks;
    const auto output = qEnvironmentVariable("KEMU_FONT_TEST_RESULTS");
    const bool fade = QApplication::isEffectEnabled(Qt::UI_FadeTooltip);
    const bool animate = QApplication::isEffectEnabled(Qt::UI_AnimateTooltip);
    QString error;
    try {
        const auto checkFont = [](const QFont& font, const QString& name) {
            if (font.family() != "Microsoft YaHei" ||
                !(font.styleStrategy() & QFont::ForceOutline) ||
                !(font.styleStrategy() & QFont::PreferAntialias) ||
                font.hintingPreference() != QFont::PreferNoHinting) {
                throw std::runtime_error((name + ": " + font.toString() +
                    ", strategy=" + QString::number(font.styleStrategy()) +
                    ", hinting=" + QString::number(font.hintingPreference())).toStdString());
            }
        };
        const auto checkTree = [&](QWidget* root, const QString& name) {
            if (!root) throw std::runtime_error(("Missing widget: " + name).toStdString());
            root->ensurePolished();
            auto widgets = root->findChildren<QWidget*>();
            widgets.prepend(root);
            for (auto* widget : widgets) {
                widget->ensurePolished();
                checkFont(widget->font(), name + "/" + widget->metaObject()->className() + "/" + widget->objectName());
            }
            checks.append(name);
        };
        checkFont(QApplication::font(), "Application");
        checkFont(QToolTip::font(), "Tooltip default");
        for (const char* name : {"KemuTitleBar", "KemuSidebar", "KemuSettings"})
            checkTree(container->findChild<QWidget*>(name), name);
        auto* language = container->findChild<QComboBox*>("KemuLanguage");
        if (!language) throw std::runtime_error("Missing language selector");
        checkTree(language->view(), "Language popup");

        const auto text = QStringLiteral("\u8bbe\u7f6e / Install APK");
        QMessageBox message(QMessageBox::Warning, "Kemu", text, QMessageBox::Ok);
        message.setStyleSheet(container->styleSheet());
        checkTree(&message, "Late message box");
        message.setInformativeText(text);
        message.addButton(QStringLiteral("\u53d6\u6d88"), QMessageBox::RejectRole);
        checkTree(&message, "Late message box children");
        QProgressDialog progress(text, QStringLiteral("\u53d6\u6d88"), 0, 0);
        progress.setStyleSheet(container->styleSheet());
        checkTree(&progress, "APK and file progress");
        QMenu menu;
        menu.addAction(text);
        checkTree(&menu, "Context menu");
        QFileDialog picker;
        picker.setOption(QFileDialog::DontUseNativeDialog);
        checkTree(&picker, "Qt file picker");
        StorageMoveDialog moving(QStringLiteral("D:/\u6d4b\u8bd5/userdata-qemu.img"));
        checkTree(&moving, "Storage move window");
        LegalInfoDialog legal(container, qEnvironmentVariable("KEMU_INSTALL_ROOT"), 0);
        checkTree(&legal, "Legal information");
        auto* heading = container->findChild<QLabel*>("KemuLoadingText");
        if (!heading || heading->font().pointSize() != 13 || heading->font().weight() != QFont::Medium)
            throw std::runtime_error("Startup heading lost its size or weight");
        checks.append("Custom heading size and weight");

        QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
        QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
        const struct { const char* name; QString text; } tips[] = {
            {"Settings", QStringLiteral("\u8bbe\u7f6e")},
            {"RecentApps", QStringLiteral("\u4efb\u52a1\u5207\u6362")},
            {"InstallAPK", QStringLiteral("\u5b89\u88c5 APK")},
            {"English", QStringLiteral("Install APK")}};
        for (const auto& tip : tips) {
            QToolTip::showText(container->mapToGlobal(QPoint(80, 80)),
                tip.text, container);
            QWidget* tooltip = nullptr;
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* label = qobject_cast<QLabel*>(widget);
                if (widget->windowType() == Qt::ToolTip && label && label->text() == tip.text) {
                    tooltip = widget; break;
                }
            }
            checkTree(tooltip, QString("Visible tooltip: ") + tip.name);
            if (!tooltip->grab().save(output + "." + tip.name + ".png"))
                throw std::runtime_error("Cannot save tooltip rendering");
        }
        checks.append("Resolved family: " + QFontInfo(QApplication::font()).family());
    } catch (const std::exception& failure) {
        error = QString::fromUtf8(failure.what());
    }
    QToolTip::hideText();
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, fade);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, animate);
    QFile file(output);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{{"passed", error.isEmpty()},
            {"error", error}, {"checks", checks}}).toJson());
    }
}
}
