#include "../src/UpdateController.h"
#include "../src/UiLanguage.h"
#include "../src/PerformancePage.h"
#include "../src/WindowSizing.h"
#include "../src/FrameRateCounter.h"
#include "../src/FrameRateLabel.h"
#include "../launcher/StorageConfig.h"
#include <QComboBox>
#include <QStackedWidget>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QLineEdit>
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
class CloseProbe : public QWidget {
public:
    int closes = 0;
protected:
    void closeEvent(QCloseEvent* event) override { ++closes; event->accept(); }
};
static void checkFrameRateHitTesting() {
    using kemu::update::require;
    QWidget window(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.resize(240, 140);
    QWidget display(&window);
    display.setAttribute(Qt::WA_NativeWindow);
    display.setGeometry(0, 0, 240, 140);
    kemu::FrameRateLabel overlay(&window);
    overlay.setStyleSheet("background: #15191f; color: #b8f59b;");
    overlay.setGeometry(8, 8, 100, 30);
    window.show();
    display.show();
    for (const auto* text : {"FPS: 60", "FPS: 120"}) {
        overlay.setText(text);
        overlay.show();
        overlay.raise();
        QEventLoop exposed;
        QTimer::singleShot(100, &exposed, &QEventLoop::quit);
        exposed.exec();
        const auto handle = reinterpret_cast<HWND>(overlay.winId());
        RECT rect{};
        require(GetWindowRect(handle, &rect), "Cannot locate frame rate overlay");
        const POINT point{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
        if (WindowFromPoint(point) != reinterpret_cast<HWND>(display.winId())) {
            std::cerr << "Frame rate hit test: actual=" << WindowFromPoint(point)
                      << " display=" << reinterpret_cast<HWND>(display.winId())
                      << " overlay=" << handle << " parent=" << reinterpret_cast<HWND>(window.winId())
                      << " visible=" << IsWindowVisible(handle)
                      << " style=" << GetWindowLongPtrW(handle, GWL_EXSTYLE) << '\n';
        }
        require(WindowFromPoint(point) == reinterpret_cast<HWND>(display.winId()),
                "Native mouse hit testing must pass through the visible frame rate overlay");
        require(window.childAt(overlay.geometry().center()) == &display,
                "Qt mouse hit testing must pass through the frame rate overlay");
        overlay.hide();
        window.resize(260, 160);
    }
}
static void checkPerformanceLifecycle(const QString& root) {
    using kemu::update::require;
    {
        QWidget parent;
        kemu::PerformancePage reopened(&parent);
        require(reopened.findChild<QSpinBox*>("KemuWidth")->value()==1200 && reopened.findChild<QSpinBox*>("KemuFps")->value()==144, "Reopening lost custom settings");
        reopened.findChild<QButtonGroup*>("KemuResolution")->button(1)->click();
        reopened.findChild<QButtonGroup*>("KemuResolution")->button(4)->click();
        require(reopened.findChild<QSpinBox*>("KemuHeight")->value()==2560, "Reopening lost remembered custom dimensions");
    }
    for (const bool succeeds : {true,false}) {
        CloseProbe window; window.setAttribute(Qt::WA_DontShowOnScreen); window.show();
        QDialog dialog(&window); dialog.setAttribute(Qt::WA_DontShowOnScreen);
        int attempts=0; bool savedBeforeRestart=false, sawPrompt=false, sawFailure=false;
        kemu::PerformancePage page(&dialog,[&](const QString& installRoot) {
            ++attempts;
            const auto config=kemu::PerformanceConfig::load(std::filesystem::path((root+"/performance.ini").toStdWString()));
            savedBeforeRestart=installRoot==root && config.fps==page.findChild<QSpinBox*>("KemuFps")->value();
            return succeeds;
        });
        auto* fps=page.findChild<QSpinBox*>("KemuFps"); fps->findChild<QLineEdit*>()->setText(QString::number(fps->value()+1));
        QTimer closer;
        QObject::connect(&closer,&QTimer::timeout,[&] {
            for (auto* box : window.findChildren<QMessageBox*>()) {
                if (!box->isVisible()) continue;
                if (box->objectName()=="KemuPerformanceRestart") {
                    sawPrompt=true;
                    for (auto* button:box->buttons()) if (box->buttonRole(button)==QMessageBox::AcceptRole) button->click();
                } else { sawFailure=true; box->accept(); }
            }
        });
        closer.start(10); page.closed(&dialog,&window,root); closer.stop();
        require(sawPrompt && attempts==1 && savedBeforeRestart,"Restart must run once after saving typed input");
        require(window.closes==(succeeds?1:0) && sawFailure==!succeeds,"Restart failure must keep the emulator open");
    }
    const auto previous=qgetenv("ANDROID_AVD_HOME");
    const auto blocked=root+"/blocked";
    require(QDir().mkpath(blocked+"/performance.ini"),"Cannot prepare failed-save fixture");
    qputenv("ANDROID_AVD_HOME",blocked.toUtf8());
    {
        QWidget window; QDialog dialog(&window); dialog.setAttribute(Qt::WA_DontShowOnScreen);
        int attempts=0; kemu::PerformancePage page(&dialog,[&](const QString&) { ++attempts; return true; });
        page.findChild<QButtonGroup*>("KemuResolution")->button(0)->click();
        bool warned=false; QTimer closer;
        QObject::connect(&closer,&QTimer::timeout,[&] {
            for (auto* box:window.findChildren<QMessageBox*>()) if(box->isVisible()) { warned=true; box->accept(); }
        });
        closer.start(10); page.closed(&dialog,&window,root); closer.stop();
        require(warned && dialog.isVisible() && attempts==0,"Failed save must retain settings and never restart");
    }
    qputenv("ANDROID_AVD_HOME",previous);
}
static void checkFullInstallerPrompt() {
    using namespace kemu::update;
    for (const bool intermediate : {false, true}) {
        for (const bool automatic : {false, true}) {
            QTemporaryDir temporary;
            const fs::path root(temporary.path().toStdWString());
            qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8());
            qputenv("ANDROID_AVD_HOME",temporary.path().toUtf8());
            writeText(root/"VERSION.txt","v0.1.0\n");
            Json history = {{"schema_version",1},{"releases",Json::array({
                {{"version","v0.1.0"},{"files",Json::array()}},
                {{"version","v0.2.0"},{"files",{"a.dll"}}}})}};
            const auto stage = root/".kemu-update";
            prepareUpdateTask(root,history);
            writeText(stage/"payload/a.dll","cached");
            writeText(stage/"v0.2.0/sha256sum.txt",sha256(stage/"payload/a.dll")+"  Windows/a.dll\n");
            saveDownloadedUpdate(stage,resolve(history,"v0.1.0"));
            history["releases"][1]["files"] = Json::array();
            if (intermediate) history["releases"].push_back({{"version","v0.3.0"},{"files",{"a.dll"}}});
            kemu::setUiLanguage(automatic ? "en" : "zh-CN");
            QWidget window;
            kemu::UpdateController controller(&window,[]{return QString();},[]{return QString();},[&]{return history;});
            auto* dialog = window.findChild<QDialog*>("KemuSettings");
            dialog->setAttribute(Qt::WA_DontShowOnScreen);
            dialog->findChild<QCheckBox*>("KemuAutoUpdate")->setChecked(automatic);
            bool offered = false, correct = false;
            QEventLoop wait;
            QTimer poll, deadline;
            QObject::connect(&poll,&QTimer::timeout,[&] {
                if (auto* box = window.findChild<QMessageBox*>()) {
                    offered = box->objectName() == "KemuFullInstallerRequired";
                    correct = box->text() == (automatic
                        ? QStringLiteral("A major update is available. Please download the full installer.")
                        : QStringLiteral("\u6709\u91cd\u5927\u66f4\u65b0\uff0c\u8bf7\u524d\u5f80\u4e0b\u8f7d\u5b8c\u6574\u7248\u3002"));
                    correct = correct && box->buttons().size() == 2;
                    box->reject(); wait.quit();
                }
            });
            QObject::connect(&deadline,&QTimer::timeout,[&] {
                for (auto* box : window.findChildren<QMessageBox*>()) box->reject();
                wait.quit();
            });
            poll.start(10); deadline.start(5000);
            dialog->findChild<QPushButton*>("KemuCheckUpdate")->click();
            wait.exec(); controller.stop();
            require(offered && correct,"Manual check must offer the translated full installer instead of cached installation");
            require(readVersion(root)=="v0.1.0" && !fs::exists(root/".kemu-update-pending") &&
                    !fs::exists(stage/"job.json") && !fs::exists(root/".kemu-update-history.tmp"),
                    "Full installer check must not prepare an installation or leave downloaded history");
        }
    }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    if (argc > 1 && QString::fromUtf8(argv[1]) == "major") {
        try { checkFullInstallerPrompt(); std::cout << "Full installer UI checks passed\n"; return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc == 5 && QString::fromUtf8(argv[1]) == "growth") {
        qputenv("ANDROID_AVD_HOME",QByteArray(argv[2]));
        qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8());
        QSettings(QSettings::defaultFormat(),QSettings::UserScope,"Kemu","Kemu").setValue("updates/automatic",false);
        const std::filesystem::path avd=std::filesystem::u8path(argv[2]);
        QWidget window; window.setAttribute(Qt::WA_DontShowOnScreen);
        kemu::UpdateController controller(&window,[&] { return QString::fromUtf8(argv[3]); },
                                           [&] { return QString::fromUtf8(argv[4]); });
        QTimer poll;
        QObject::connect(&poll,&QTimer::timeout,[&] {
            if (auto* error=window.findChild<QMessageBox*>()) {
                std::cerr << error->text().toStdString() << '\n'; error->reject(); app.exit(1);
            } else if (!std::filesystem::exists(kemu::storage::requestPath(avd))) {
                std::cout << "Guest filesystem expansion verified and request completed\n"; app.exit(0);
            }
        });
        poll.start(100);
        QTimer::singleShot(240000,&app,[&] { std::cerr << "Expansion integration test timed out\n"; app.exit(1); });
        return app.exec();
    }
    using kemu::update::require;
    {
        using namespace std::chrono;
        const kemu::FrameRateCounter::Clock::time_point start{};
        kemu::FrameRateCounter counter(start);
        for (int frame = 0; frame < 30; ++frame) counter.recordFrame();
        require(counter.sample(start) == 0.0, "Zero elapsed time must not consume frames");
        require(counter.sample(start + milliseconds(500)) == 60.0, "Frame rate must use elapsed seconds");
        require(counter.sample(start + seconds(1)) == 0.0, "Idle frame rate must return to zero");
        for (int frame = 0; frame < 90; ++frame) counter.recordFrame();
        require(counter.sample(start + milliseconds(2500)) == 60.0, "Delayed UI timers must use actual elapsed time");
    }
    for (const auto screen : {QSize(1920,1040),QSize(1280,720),QSize(960,540)}) {
        for (const auto display : {QSize(1920,1080),QSize(1080,1920),QSize(1200,2560)}) {
            const auto sizing=kemu::windowSizing(display,screen);
            const auto viewport=sizing.initial-QSize(54,42);
            require(sizing.initial.width()<=screen.width() && sizing.initial.height()<=screen.height(),"Initial window exceeds screen bounds");
            require(sizing.minimum.width()<=sizing.initial.width() && sizing.minimum.height()<=sizing.initial.height(),"Window minimum overrides initial orientation");
            require(qAbs(double(viewport.width())/viewport.height()-double(display.width())/display.height())<0.005,"Initial window aspect ratio does not match display");
            if (display.width()<display.height()) require(sizing.initial.width()<sizing.initial.height(),"Portrait display still opens a landscape window");
        }
    }
    for (const auto* locale : {"zh_CN", "zh_SG", "zh_Hans"})
        require(kemu::defaultUiLanguage(QLocale(locale)) == "zh-CN", "Simplified Chinese detection failed");
    for (const auto* locale : {"en_US", "ja_JP", "zh_TW", "zh_HK", "zh_Hant", "fr_FR"})
        require(kemu::defaultUiLanguage(QLocale(locale)) == "en", "Non-Simplified locale must use English");
    QSettings preferences(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
    require(kemu::initializeUiLanguage(preferences, QLocale("en_US")) == "en", "First launch default failed");
    require(kemu::initializeUiLanguage(preferences, QLocale("zh_CN")) == "en", "Saved language was overwritten");
    preferences.remove("general/language");
    require(kemu::initializeUiLanguage(preferences, QLocale("zh_CN")) == "zh-CN", "Chinese first launch default failed");
    kemu::setUiLanguage("zh-CN");
    require(QFile::exists(":/kemu/translations/kemu_zh_CN.qm"), "Embedded translation catalog missing");
    const auto translatedSettings = QCoreApplication::translate("Kemu", "Settings");
    require(translatedSettings == QStringLiteral("\u8bbe\u7f6e"), "Qt translator did not load Chinese");
    require(QCoreApplication::translate("Kemu", "Version %1 is available; %2 files need updating. ")
        .arg("v0.2.0").arg(3).contains("v0.2.0"), "Translation placeholders were lost");
    for (int i = 0; i < 3; ++i) {
        kemu::setUiLanguage("en");
        require(QCoreApplication::translate("Kemu", "Settings") == "Settings", "English source fallback failed");
        require(kemu::nativeUiLanguageName("zh-CN") == QStringLiteral("\u7b80\u4f53\u4e2d\u6587"), "Native language name changed");
        kemu::setUiLanguage("zh-CN");
        require(QCoreApplication::translate("Kemu", "Settings") == translatedSettings, "Translator reinstall failed");
    }
    qputenv("ANDROID_AVD_HOME",temporary.path().toUtf8());
    QDir().mkpath(temporary.filePath("kemu.avd"));
    {
        QFile image(temporary.filePath("kemu.avd/userdata-qemu.img"));
        require(image.open(QIODevice::WriteOnly) && image.write(QByteArray(4096,'A'))==4096,
                "Cannot create storage UI fixture");
    }
    const std::filesystem::path storageRoot(temporary.path().toStdWString());
    kemu::storage::requestGrowth(storageRoot,128);
    kemu::storage::applyGrowth(storageRoot);
    kemu::storage::completeGrowth(storageRoot,kemu::storage::readRequest(storageRoot));
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
            checkFrameRateHitTesting();
            auto* dialog=window.findChild<QDialog*>("KemuSettings"); require(dialog && dialog->isVisible(),"Settings dialog missing");
            require(dialog->findChild<QListWidget*>("KemuSettingsTabs")->count()==3,"Update navigation missing");
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
            require(automatic->text() == QStringLiteral("自动更新"), "Automatic update checkbox was not translated");
            auto* showFrameRate = dialog->findChild<QCheckBox*>("KemuShowFrameRate");
            require(showFrameRate && !showFrameRate->isChecked() && showFrameRate->isVisible(),
                    "Frame rate option must default to off on the General page");
            require(showFrameRate->text() == QStringLiteral("\u663e\u793a\u5e27\u7387"), "Frame rate option was not translated");
            int frameRateChanges = 0;
            bool frameRateVisible = false;
            controller.showFrameRateChanged = [&](bool show) { ++frameRateChanges; frameRateVisible = show; };
            for (const bool show : {true, false}) {
                showFrameRate->click();
                require(frameRateVisible == show && frameRateChanges == (show ? 1 : 2),
                        "Frame rate option must apply immediately");
                QWidget reopenedWindow;
                kemu::UpdateController reopened(&reopenedWindow, [] { return QString(); }, [] { return QString(); });
                require(reopenedWindow.findChild<QCheckBox*>("KemuShowFrameRate")->isChecked() == show,
                        "Frame rate option must persist across controller recreation");
            }
            controller.showFrameRateChanged = {};
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
            auto* languages = dialog->findChild<QComboBox*>("KemuLanguage");
            auto* tabs = dialog->findChild<QListWidget*>("KemuSettingsTabs");
            require(languages && languages->count() == 2, "Language choices missing");
            require(tabs->currentRow() == 0 && dialog->findChild<QStackedWidget*>()->currentIndex() == 0, "General must be the default page");
            bool languageChanged = false;
            controller.languageChanged = [&] { languageChanged = true; };
            languages->setCurrentIndex(languages->findData("en"));
            require(languageChanged && dialog->windowTitle() == "Settings" && tabs->item(0)->text() == "General", "Live English switch failed");
            require(automatic->text() == "Automatic updates", "Update page was not translated");
            require(showFrameRate->text() == "Show frame rate", "Frame rate option did not switch to English");
            QSettings saved(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
            require(saved.value("general/language").toString() == "en", "Language was not saved");
            require(kemu::initializeUiLanguage(saved, QLocale("zh_CN")) == "en", "Relaunch must preserve choice");
            if (argc > 2) require(dialog->grab().save(QString::fromUtf8(argv[2]) + ".english.png"), "Cannot save English UI rendering");
            languages->setCurrentIndex(languages->findData("zh-CN"));
            require(kemu::uiLanguage() == "zh-CN" && dialog->windowTitle() != "Settings", "Switch back failed");
            tabs->setCurrentRow(2);
            require(dialog->findChild<QStackedWidget*>()->currentWidget()->isAncestorOf(dialog->findChild<QPushButton*>("KemuCheckUpdate")), "Update navigation failed");
            tabs->setCurrentRow(0);
            if (argc>2) require(dialog->grab().save(QString::fromUtf8(argv[2])),"Cannot save UI rendering");
            tabs->setCurrentRow(1);
            require(dialog->findChild<QStackedWidget*>()->currentWidget()->isAncestorOf(dialog->findChild<QSpinBox*>("KemuCpu")), "Performance navigation failed");
            auto* expand = dialog->findChild<QPushButton*>("KemuExpandDisk");
            auto* storage = dialog->findChild<QLabel*>("KemuStorageUsage");
            require(expand && expand->isEnabled() && storage && storage->text()=="0.0G/128G","Storage capacity or allocated size is incorrect");
            bool confirmed=false;
            QTimer::singleShot(0,[&] {
                auto* prompt=dialog->findChild<QDialog*>("KemuStorageExpand");
                confirmed=prompt && prompt->findChild<QPushButton*>("KemuStorageConfirm");
                if (prompt) {
                    auto* size=prompt->findChild<QSpinBox*>("KemuStorageCapacity");
                    require(size && size->minimum()==129 && size->value()==256,"Expansion must only offer larger capacities");
                    if (argc>2) prompt->grab().save(QString::fromUtf8(argv[2])+".storage.png");
                    prompt->reject();
                }
            });
            expand->click();
            require(confirmed && !kemu::storage::readRequest(storageRoot).target &&
                    kemu::storage::usage(storageRoot).capacity==128*kemu::storage::GiB,
                    "Cancelling confirmation changed the disk or queued a restart");
            auto* cpu = dialog->findChild<QSpinBox*>("KemuCpu");
            auto* cpuSlider = dialog->findChild<QSlider*>("KemuCpuSlider");
            auto* ram = dialog->findChild<QSpinBox*>("KemuRam");
            auto* ramSlider = dialog->findChild<QSlider*>("KemuRamSlider");
            require(cpu->minimum()==1 && cpu->maximum()==kemu::hostCpuCount(), "CPU range mismatch");
            require(ram->minimum()==4 && ram->maximum()==kemu::hostRamGb(), "RAM range mismatch");
            cpuSlider->setValue(cpuSlider->maximum()); require(cpu->value()==cpuSlider->maximum(), "CPU slider did not update input");
            cpu->setValue(1); require(cpuSlider->value()==1, "CPU input did not update slider");
            ramSlider->setValue(ramSlider->maximum()); require(ram->value()==ramSlider->maximum(), "RAM slider did not update input");
            ram->setValue(1); require(ramSlider->value()==4, "RAM input did not update slider");
            auto* resolution = dialog->findChild<QButtonGroup*>("KemuResolution");
            auto* orientation = dialog->findChild<QButtonGroup*>("KemuOrientation");
            resolution->button(2)->click(); orientation->button(1)->click();
            require(dialog->findChild<QSpinBox*>("KemuWidth")->value()==1440 && dialog->findChild<QSpinBox*>("KemuHeight")->value()==2560, "Portrait resolution mismatch");
            auto* fpsMode = dialog->findChild<QButtonGroup*>("KemuFpsMode");
            fpsMode->button(4)->click();
            require(dialog->findChild<QSpinBox*>("KemuFps")->value()==120 && !dialog->findChild<QSpinBox*>("KemuFps")->isVisible(), "120 FPS preset must hide custom input");
            fpsMode->button(2)->click();
            require(dialog->findChild<QSpinBox*>("KemuFps")->value()==qRound(dialog->screen()->refreshRate()), "Screen FPS mismatch");
            fpsMode->button(3)->click(); dialog->findChild<QSpinBox*>("KemuFps")->setValue(144);
            resolution->button(4)->click(); dialog->findChild<QSpinBox*>("KemuWidth")->setValue(1200);
            dialog->findChild<QSpinBox*>("KemuHeight")->setValue(2560);
            require(!dialog->findChild<QComboBox*>("KemuResolution") && resolution->buttons().size()==5 && fpsMode->buttons().size()==5 && orientation->buttons().size()==2, "Performance choices must use radio boxes");
            require(resolution->button(0)->text()=="720x1280" && resolution->button(2)->text()=="1440x2560", "Portrait resolution labels mismatch");
            resolution->button(0)->click(); resolution->button(4)->click();
            require(dialog->findChild<QSpinBox*>("KemuWidth")->value()==1200 && dialog->findChild<QSpinBox*>("KemuHeight")->value()==2560, "Custom resolution lost after selecting a preset");
            fpsMode->button(0)->click(); fpsMode->button(3)->click();
            require(dialog->findChild<QSpinBox*>("KemuFps")->value()==144, "Custom FPS lost after selecting a preset");
            if (argc>2) require(dialog->grab().save(QString::fromUtf8(argv[2])+".performance.png"), "Cannot save performance rendering");
            bool restartOffered = false, restartStyled = false, restartOrdered = false;
            QTimer::singleShot(0,[&] {
                auto* prompt = window.findChild<QMessageBox*>("KemuPerformanceRestart");
                restartOffered = prompt && prompt->buttons().size()==2;
                if (prompt) {
                    restartStyled = prompt->parentWidget()==dialog && prompt->palette().color(QPalette::Window)==QColor("#1b2028");
                    auto* buttons=prompt->findChild<QDialogButtonBox*>();
                    QAbstractButton *later=nullptr, *now=nullptr;
                    for (auto* button:prompt->buttons()) {
                        if (prompt->buttonRole(button)==QMessageBox::RejectRole) later=button;
                        if (prompt->buttonRole(button)==QMessageBox::AcceptRole) now=button;
                    }
                    restartOrdered = later && now && buttons && later->x()<now->x() && qAbs(now->geometry().right()-buttons->rect().right())<=2;
                    if (argc>2) prompt->grab().save(QString::fromUtf8(argv[2])+".restart.png");
                    prompt->reject();
                }
            });
            dialog->close(); require(restartOffered, "Performance restart prompt missing");
            require(restartStyled, "Restart prompt must inherit the settings theme");
            require(restartOrdered, "Restart buttons must be Later then Now and right aligned");
            const auto performance = kemu::PerformanceConfig::load(std::filesystem::path(temporary.filePath("performance.ini").toStdWString()));
            require(performance.width==1200 && performance.height==2560 && performance.fps==144 && performance.cores==1 && performance.ramGb==4, "Performance settings were not saved");
            controller.showSettings();
            bool unexpectedPrompt = false;
            QTimer::singleShot(0,[&] { if (auto* prompt=window.findChild<QMessageBox*>("KemuPerformanceRestart")) { unexpectedPrompt=true; prompt->reject(); } });
            dialog->close(); app.processEvents(); require(!unexpectedPrompt,"Unchanged settings prompted for restart");
            checkPerformanceLifecycle(temporary.path());
            std::cout<<"Update settings UI checks passed\n"; app.exit(0);
        } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; app.exit(1); }
    });
    return app.exec();
}
