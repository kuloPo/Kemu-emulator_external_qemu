#include "../src/UpdateController.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFile>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
int main(int argc, char** argv) {
    QApplication app(argc, argv); QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8()); bool missing=argc>1 && QString::fromUtf8(argv[1])=="missing";
    if (!missing) { QFile v(temporary.filePath("VERSION.txt")); v.open(QIODevice::WriteOnly); v.write("v0.1.0\n"); }
    bool ready = argc > 1 && QString::fromUtf8(argv[1]) == "ready";
    if (ready) {
        using namespace kemu::update;
        fs::path root(temporary.path().toStdWString()), stage=root/".kemu-update";
        writeText(stage/"payload/a.dll","new");
        Json history={{"schema_version",1},{"releases",Json::array({
            {{"version","v0.1.0"},{"files",Json::array()}},
            {{"version","v0.2.0"},{"files",{"a.dll"}}}})}};
        prepareUpdateTask(root,history);
        writeText(stage/"payload/a.dll","new");
        writeText(stage/"v0.2.0/sha256sum.txt",sha256(stage/"payload/a.dll")+"  Windows/a.dll\n");
        saveDownloadedUpdate(stage,resolve(history,"v0.1.0"));
    }
    QWidget window; kemu::UpdateController controller(&window,[]{return QString();},[]{return QString();}, [&] { return kemu::update::readJson(kemu::update::fs::path(temporary.path().toStdWString()) / ".kemu-update/history.json"); });
    window.findChild<QDialog*>("KemuSettings")->setAttribute(Qt::WA_DontShowOnScreen);
    controller.showSettings();
    auto* initial = window.findChild<QCheckBox*>("KemuAutoUpdate");
    if (!initial->isChecked()) return 2;
    // Suppress automatic network checks in the isolated UI test.
    if (!ready) initial->setChecked(false);
    bool startupPrompt = false;
    QTimer promptCloser;
    QObject::connect(&promptCloser, &QTimer::timeout, [&] {
        if (auto* prompt = window.findChild<QMessageBox*>()) {
            startupPrompt = prompt->windowTitle() == QStringLiteral("更新已就绪");
            prompt->reject();
        }
    });
    if (ready) promptCloser.start(20);
    QTimer::singleShot(2400,&app,[&] {
        try {
            using kemu::update::require;
            auto* dialog=window.findChild<QDialog*>("KemuSettings"); require(dialog && dialog->isVisible(),"Settings dialog missing");
            require(dialog->findChild<QListWidget*>("KemuSettingsTabs")->count()==1,"Update navigation missing");
            require(dialog->findChild<QPushButton*>("KemuCheckUpdate")->isEnabled(),"Check update disabled");
            require(!dialog->findChild<QPushButton*>("KemuInstallUpdate") && !dialog->findChild<QPushButton*>("KemuDownloadUpdate"),"Obsolete buttons remain");
            if (ready) {
                promptCloser.stop(); require(startupPrompt, "Cached automatic update not offered on next startup");
                require(!QFile::exists(temporary.filePath(".kemu-update-pending")), "Later choice must not install");
                for (bool enabled : {false, true}) {
                    initial->setChecked(enabled); bool offered = false;
                    QEventLoop wait; QTimer poll;
                    QObject::connect(&poll, &QTimer::timeout, [&] { if (auto* box = window.findChild<QMessageBox*>()) { offered = box->windowTitle() == QStringLiteral("更新已就绪"); box->reject(); wait.quit(); } });
                    poll.start(10); QTimer::singleShot(5000, &wait, &QEventLoop::quit);
                    dialog->findChild<QPushButton*>("KemuCheckUpdate")->click();
                    wait.exec();
                    require(offered, "Manual update depends on automatic checkbox");
                }
            }
            auto* status=dialog->findChild<QLabel*>("KemuUpdateStatus");
            if (missing) require(status->text().isEmpty(),"Missing VERSION guidance");
            auto* automatic=dialog->findChild<QCheckBox*>("KemuAutoUpdate");
            require(dialog->findChildren<QCheckBox*>().size() == 1 && automatic->text() == QStringLiteral("自动更新"), "Expected single automatic update checkbox");
            if (missing) {
                bool prompted = false;
                QTimer::singleShot(0, [&] {
                    auto* prompt = dialog->findChild<QMessageBox*>();
                    prompted = prompt && prompt->text().contains(QStringLiteral("点击确定后前往下载完整安装包"));
                    if (prompt) {
                        for (auto* child : prompt->findChildren<QWidget*>()) {
                            prompted = prompted && child->font().family() == "Microsoft YaHei" &&
                                (child->font().styleStrategy() & QFont::ForceOutline) &&
                                (child->font().styleStrategy() & QFont::PreferAntialias);
                        }
                        if (argc > 2) prompt->grab().save(QString::fromUtf8(argv[2]) + ".prompt.png");
                        prompt->reject();
                    }
                });
                dialog->findChild<QPushButton*>("KemuCheckUpdate")->click();
                require(prompted, "Missing version confirmation not shown");
            }
            automatic->setChecked(true);
            for (auto* widget : dialog->findChildren<QWidget*>()) {
                const auto font = widget->font();
                require(font.family() == "Microsoft YaHei", "Settings font fell back to another family");
                require((font.styleStrategy() & QFont::ForceOutline) && (font.styleStrategy() & QFont::PreferAntialias), "Settings stylesheet reset font rendering strategy");
            }
            if (argc>2) require(dialog->grab().save(QString::fromUtf8(argv[2])),"Cannot save UI rendering");
            std::cout<<"Update settings UI checks passed\n"; app.exit(0);
        } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; app.exit(1); }
    });
    return app.exec();
}
