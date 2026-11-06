#include "../../src/LegalInfoDialog.h"
#include "../../src/UpdateController.h"
#include "../../src/UiLanguage.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static QString noticeText(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Cannot read notice text");
    return QString::fromUtf8(file.readAll());
}
static void checkNoticeText(QTextBrowser* browser, const QString& path) {
    QTextDocument expected;
    expected.setPlainText(noticeText(path));
    if (browser->toPlainText() != expected.toPlainText())
        std::cerr << "Notice: " << path.toStdString() << '\n';
    check(browser->toPlainText() == expected.toPlainText(), "Rendered notice differs from its plain text");
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        check(argc == 3, "Usage: Kemu <staged installation> <screenshot directory>");
        QTemporaryDir temporary;
        check(temporary.isValid(), "Cannot create test directory");
        const auto root = temporary.filePath(QStringLiteral("\u8bb8\u53ef files"));
        QDir().mkpath(root);
        const QDir staged(QString::fromLocal8Bit(argv[1]));
        QDirIterator files(staged.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto source = files.next();
            const auto target = QDir(root).filePath(staged.relativeFilePath(source));
            QDir().mkpath(QFileInfo(target).path());
            check(QFile::copy(source, target), "Cannot stage a legal file");
        }
        const auto literalNotice = QStringLiteral(
            "\nCopyright Example <author@example.org>\n\n"
            "  Keep <year>, <name>, & and &lt; literally.\n"
            "<b>This is text, not markup.</b>\n"
            "\tUnicode: \u4e2d\u6587 \u00a9\n\n");
        QFile literalFile(QDir(root).filePath("legal/literal.txt"));
        check(literalFile.open(QIODevice::WriteOnly), "Cannot write literal notice fixture");
        literalFile.write(literalNotice.toUtf8().replace("\n", "\r\n")); literalFile.close();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Kemu", "Kemu");
        settings.setValue("updates/automatic", false); settings.sync();
        check(_wputenv_s(L"KEMU_INSTALL_ROOT", root.toStdWString().c_str()) == 0, "Cannot set Unicode install root");
        check(qEnvironmentVariable("KEMU_INSTALL_ROOT") == root, "Unicode environment round trip failed");
        qputenv("ANDROID_AVD_HOME", temporary.filePath("avd").toUtf8());
        QFile version(QDir(root).filePath("VERSION.txt"));
        check(version.open(QIODevice::WriteOnly), "Cannot write version fixture");
        version.write("v0.5.1\n"); version.close();
        QWidget parent;
        kemu::UpdateController controller(&parent, [] { return QString(); }, [] { return QString(); });
        auto* settingsDialog = parent.findChild<QDialog*>("KemuSettings");
        auto* language = settingsDialog->findChild<QComboBox*>("KemuLanguage");
        auto* pages = settingsDialog->findChild<QStackedWidget*>();
        controller.showSettings();
        const QDir pictures(QString::fromLocal8Bit(argv[2]));
        for (const char* locale : {"en", "zh-CN"}) {
            language->setCurrentIndex(language->findData(locale));
            settingsDialog->findChild<QListWidget*>("KemuSettingsTabs")->setCurrentRow(3);
            QApplication::processEvents();
            auto* licenses = settingsDialog->findChild<QPushButton*>("KemuOpenSourceLicenses");
            check(licenses && licenses->isVisible(), "License entry is missing");
            check(licenses->text() == QCoreApplication::translate("Kemu", "Open-source licenses"), "Untranslated license entry");
            for (auto* label : pages->currentWidget()->findChildren<QLabel*>()) {
                check(pages->currentWidget()->rect().contains(label->geometry()), "About label extends beyond the page");
            }
            check(settingsDialog->grab().save(pictures.filePath(QString("about-%1.png").arg(locale))), "Cannot save About rendering");
            licenses->click();
            QApplication::processEvents();
            auto* legal = settingsDialog->findChild<QDialog*>("KemuLegalInformation");
            check(legal && legal->isVisible(), "Legal viewer did not open");
            auto* text = legal->findChild<QTextBrowser*>("KemuLegalText");
            if (!text->toPlainText().contains("Copyright (c) 2026 Kemu contributors"))
                std::cerr << "Source: " << text->source().toString().toStdString()
                          << "\nText: " << text->toPlainText().toStdString() << '\n';
            check(text->toPlainText().contains("Copyright (c) 2026 Kemu contributors"), "Offline license overview missing");
            check(text->toPlainText().contains("GPL-2.0-only"), "Qt GPLv2 selection missing");
            const auto overview = text->toPlainText();
            text->anchorClicked(QUrl("literal.txt"));
            check(text->toPlainText() == literalNotice, "Plain notice content was changed");
            const auto literalUrl = text->source();
            text->anchorClicked(QUrl("licenses/LICENSE.LGPLv3"));
            checkNoticeText(text, QDir(root).filePath("legal/licenses/LICENSE.LGPLv3"));
            text->backward();
            check(text->source() == literalUrl && text->toPlainText() == literalNotice, "Back navigation changed plain notice content");
            text->backward();
            check(text->toPlainText() == overview, "Back navigation did not restore the HTML overview");
            text->forward();
            check(text->source() == literalUrl && text->toPlainText() == literalNotice, "Forward navigation changed plain notice content");
            text->backward();
            text->anchorClicked(QUrl("Qt-6.5.3-THIRD-PARTY.txt"));
            check(text->toPlainText().contains("FreeType") && text->toPlainText().contains("Selected license: GPL-2.0-only"), "Qt third-party license selection missing");
            check(text->toPlainText().contains("WebGradients") && text->toPlainText().contains("JavaScriptCore Macro Assembler"), "Qt module notices missing");
            check(text->toPlainText().contains("<sneves@dei.uc.pt>"), "Copyright contact was removed");
            checkNoticeText(text, QDir(root).filePath("legal/Qt-6.5.3-THIRD-PARTY.txt"));
            text->backward();
            text->anchorClicked(QUrl("QtWebEngine-6.5.3-Chromium-THIRD-PARTY.txt"));
            check(text->toPlainText().contains("ef090c47d08c734ae2b3c1db508982aca0f3ee1e") && text->toPlainText().contains("V8 JavaScript Engine"), "Qt WebEngine Chromium notices missing");
            checkNoticeText(text, QDir(root).filePath("legal/QtWebEngine-6.5.3-Chromium-THIRD-PARTY.txt"));
            text->backward();
            text->anchorClicked(QUrl("qt-sources.html"));
            check(text->toPlainText().contains("11487595") && text->toPlainText().contains("Outstanding verification"), "Qt source references or verification status missing");
            text->backward();
            text->anchorClicked(QUrl("licenses/LICENSE.LGPLv3"));
            check(text->toPlainText().contains("6. Revised Versions"), "Full LGPLv3 text did not load");
            text->backward();
            check(text->toPlainText().contains("Component notices"), "Back navigation failed");
            text->anchorClicked(QUrl("../NOTICE.txt"));
            check(text->toPlainText().contains("Notices for file(s)"), "Host notices did not load");
            checkNoticeText(text, QDir(root).filePath("NOTICE.txt"));
            text->backward();
            text->anchorClicked(QUrl("../NOTICE.csv"));
            checkNoticeText(text, QDir(root).filePath("NOTICE.csv"));
            text->backward();
            text->anchorClicked(QUrl("android/index.html"));
            check(text->toPlainText().contains("Android system notices"), "Android notice index missing");
            text->anchorClicked(QUrl("system.txt"));
            check(text->toPlainText().contains("Copyright"), "Android license text missing");
            checkNoticeText(text, QDir(root).filePath("legal/android/system.txt"));
            auto* sections = legal->findChild<QListWidget*>("KemuLegalSections");
            sections->setCurrentRow(1);
            check(text->toPlainText().contains("matching release"), "Source instructions missing");
            sections->setCurrentRow(2);
            check(text->toPlainText().contains("Fabrice Bellard") && text->toPlainText().contains("Google LLC"), "Trademark text missing");
            checkNoticeText(text, QDir(root).filePath("legal/TRADEMARKS.txt"));
            check(legal->grab().save(pictures.filePath(QString("trademarks-%1.png").arg(locale))), "Cannot save trademark rendering");
            sections->setCurrentRow(0);
            check(legal->grab().save(pictures.filePath(QString("licenses-%1.png").arg(locale))), "Cannot save license rendering");
            text->anchorClicked(QUrl("missing.txt"));
            check(text->toPlainText() == QCoreApplication::translate("Kemu", "This notice file is not included in this installation."), "Missing notice has no readable message");
            sections->setCurrentRow(2);
            QFile outside(temporary.filePath("outside.txt"));
            check(outside.open(QIODevice::WriteOnly), "Cannot write outside-file fixture");
            outside.write("Outside installation content"); outside.close();
            text->anchorClicked(QUrl::fromLocalFile(temporary.filePath("outside.txt")));
            check(text->toPlainText() == QCoreApplication::translate("Kemu", "This notice file is not included in this installation."), "Viewer opened a file outside the installation");
            legal->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        controller.stop();
        std::cout << "Offline notices, translations, navigation and About layout passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
