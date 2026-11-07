#include "../src/UpdateController.h"
#include "../src/UiLanguage.h"
#include "../src/PerformancePage.h"
#include "../src/StorageMoveDialog.h"
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
#include <QFileDialog>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <iostream>
#include <tuple>
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
static void waitUntil(const std::function<bool()>& done, int timeout = 8000) {
    QEventLoop wait; QTimer poll, deadline;
    QObject::connect(&poll, &QTimer::timeout, [&] { if (done()) wait.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
    poll.start(10); deadline.setSingleShot(true); deadline.start(timeout);
    if (!done()) wait.exec();
    kemu::update::require(done(), "Update UI did not reach the expected state");
}
static void confirmDownload(QWidget& window, bool accept) {
    waitUntil([&] { return window.findChild<QMessageBox*>("KemuConfirmDownload") != nullptr; });
    auto* prompt = window.findChild<QMessageBox*>("KemuConfirmDownload");
    kemu::update::require(prompt->text() == QCoreApplication::translate("Kemu", "Version %1 is available. Download the update now?").arg("v0.10.0"),
                          "Full and incremental updates must use the same download confirmation");
    for (auto* button : prompt->buttons()) {
        if (prompt->buttonRole(button) == (accept ? QMessageBox::AcceptRole : QMessageBox::RejectRole)) {
            button->click(); break;
        }
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
static void checkDownloadFlow(const kemu::update::fs::path& fixtures) {
    using namespace kemu::update;
    for (const bool full : {false, true}) {
        for (const bool automatic : {false, true}) {
            QTemporaryDir temporary;
            const fs::path root(temporary.path().toStdWString()), stage = root/".kemu-update";
            qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8());
            qputenv("ANDROID_AVD_HOME",temporary.path().toUtf8());
            writeText(root/"VERSION.txt","v0.1.0\n");
            const Json history = {{"schema_version",1},{"releases",Json::array({
                {{"version","v0.10.0"},{"files",{"a.dll"}}},
                {{"version","v0.1.0"},{"files",Json::array()}},
                {{"version","v0.2.0"},{"files",full ? Json::array() : Json::array({"a.dll"})}}
            })}};
            const auto name = "Kemu-v0.10.0-Setup-x64.exe";
            writeText(root/"installer-fixture","installer payload");
            const Json metadata = {{"tag_name","v0.10.0"},{"draft",false},{"assets",Json::array({{
                {"name",name},{"browser_download_url",std::string(ReleasesUrl)+"/download/v0.10.0/"+name},
                {"size",fs::file_size(root/"installer-fixture")},{"digest","sha256:"+sha256(root/"installer-fixture")}}})}};
            std::vector<std::string> transfers;
            bool corrupt = false;
            bool pauseDownload = false;
            std::atomic<bool> downloading{false};
            const Download transfer = [&](const std::string& url, const fs::path& output, uint64_t,
                                         const std::function<bool()>& cancelled, const std::function<void(uint64_t,uint64_t)>& progress) {
                require(!cancelled(), "Cancelled before download");
                transfers.push_back(url);
                if (pauseDownload && url.find("api.github.com") == url.npos) {
                    writeText(output,"partial"); downloading = true;
                    while (!cancelled()) QThread::msleep(10);
                    throw std::runtime_error("Update cancelled");
                }
                if (url.find("api.github.com") != url.npos) writeJson(output, metadata);
                else if (full) writeText(output, corrupt ? "corrupted payload" : "installer payload");
                else fs::copy_file(fixtures/"good.tar.xz",output,fs::copy_options::overwrite_existing);
                if (progress) progress(fs::file_size(output),fs::file_size(output));
            };
            kemu::setUiLanguage(automatic ? "zh-CN" : "en");
            QSettings(QSettings::defaultFormat(),QSettings::UserScope,"Kemu","Kemu").setValue("updates/automatic",automatic);
            auto window = std::make_unique<QWidget>();
            auto* controller = new kemu::UpdateController(window.get(),[]{return QString();},[]{return QString();},[&]{return history;},transfer);
            auto* dialog = window->findChild<QDialog*>("KemuSettings"); dialog->setAttribute(Qt::WA_DontShowOnScreen);
            auto* button = dialog->findChild<QPushButton*>("KemuCheckUpdate");
            if (automatic) {
                waitUntil([&] { return dialog->isVisible() && button->isEnabled(); });
                require(transfers.empty() && !window->findChild<QMessageBox*>(), "Startup must show updates without downloading or opening a prompt");
                require(dialog->findChild<QListWidget*>("KemuSettingsTabs")->currentRow()==2, "Startup must select the Updates page");
            }
            button->click();
            confirmDownload(*window, false);
            require(transfers.empty(), "Rejecting the download must not transfer any update packages");
            require(dialog->findChild<QLabel*>("KemuUpdateStatus")->text() ==
                    QCoreApplication::translate("Kemu", "Version %1 is available. Click Check for updates to confirm the download.").arg("v0.10.0"),
                    "Update availability must use a shared message without package counts");
            button->click();
            confirmDownload(*window, true);
            if (!automatic) {
                // Delay UI completion delivery, then deliver a cancellation click queued at the boundary.
                int waits = 0;
                while (!fs::exists(stage/"downloaded.json") && waits++ < 800) QThread::msleep(10);
                require(fs::exists(stage/"downloaded.json"),"Download did not commit while UI completion was pending");
                require(QMetaObject::invokeMethod(dialog->findChild<QPushButton*>("KemuCancelUpdate"),"clicked",Qt::DirectConnection),
                        "Cannot deliver a late cancellation click");
                require(dialog->findChild<QLabel*>("KemuUpdateStatus")->text() != "Cancelling download...",
                        "A late cancel click must not replace the completed download state");
            }
            waitUntil([&] { return window->findChild<QMessageBox*>("KemuUpdateReady") != nullptr; });
            QPointer<QMessageBox> prompt = window->findChild<QMessageBox*>("KemuUpdateReady");
            require(prompt->buttons().size()==2 && button->isEnabled(), "Completed download needs Update now and Later");
            require(prompt->text() == QCoreApplication::translate("Kemu",full
                ? "The full installer has been downloaded and verified. Updating now will close the emulator and open the installer."
                : "The update has been downloaded and verified. Updating now will close and restart the emulator."), "Wrong installation prompt");
            if (full) require(transfers.size()==2 && transfers.back().find(name)!=std::string::npos, "Full update must download only the latest installer");
            else require(transfers.size()==2 && transfers[0].find("v0.2.0/")!=std::string::npos && transfers[1].find("v0.10.0/")!=std::string::npos,
                         "Download every incremental package in numeric order, even when its files are overwritten");
            prompt->reject(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
            require(prompt.isNull() && fs::exists(stage/"downloaded.json") && !fs::exists(root/".kemu-update-pending"), "Later must retain the verified download");
            button->click(); waitUntil([&] { return window->findChild<QMessageBox*>("KemuUpdateReady") != nullptr; });
            require(transfers.size()==2, "A verified cached update must not download again");
            controller->stop(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
            require(!window->findChild<QMessageBox*>("KemuUpdateReady"), "Stopping must dismiss the installation prompt");
            window.reset();
            if (full && !automatic) {
                for (const int cacheState : {0, 1, 2}) {
                    if (cacheState == 1) writeText(stage/"downloaded.json", "broken marker");
                    else fs::remove(stage/"downloaded.json");
                    if (cacheState == 2) fs::rename(stage/name,stage/(std::string(name)+".part"));
                    QWidget cacheWindow;
                    kemu::UpdateController cached(&cacheWindow,[]{return QString();},[]{return QString();},[&]{return history;},transfer);
                    auto* cachedDialog = cacheWindow.findChild<QDialog*>("KemuSettings");
                    cachedDialog->setAttribute(Qt::WA_DontShowOnScreen);
                    const auto before = transfers.size();
                    cachedDialog->findChild<QPushButton*>("KemuCheckUpdate")->click();
                    confirmDownload(cacheWindow, true);
                    waitUntil([&] { return cacheWindow.findChild<QMessageBox*>("KemuUpdateReady") != nullptr; });
                    require(transfers.size() == before + 1 && transfers.back().find("api.github.com") != std::string::npos,
                            "A complete installer cache, including a .part file, must only re-fetch release metadata");
                    cached.stop(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
                }
                {
                    fs::remove(stage/"downloaded.json"); fs::create_directory(stage/"downloaded.json.tmp");
                    QWidget cacheWindow;
                    kemu::UpdateController cached(&cacheWindow,[]{return QString();},[]{return QString();},[&]{return history;},transfer);
                    auto* cachedDialog = cacheWindow.findChild<QDialog*>("KemuSettings");
                    cachedDialog->setAttribute(Qt::WA_DontShowOnScreen);
                    auto* cachedButton = cachedDialog->findChild<QPushButton*>("KemuCheckUpdate");
                    const auto before = transfers.size();
                    cachedButton->click(); confirmDownload(cacheWindow,true);
                    waitUntil([&] { return cachedButton->isEnabled(); });
                    require(transfers.size() == before + 1 && !fs::exists(stage/"downloaded.json") && fs::exists(stage/name),
                            "A ready-record write failure must keep the installer and must not start another download");
                    require(!cacheWindow.findChild<QMessageBox*>("KemuUpdateReady"),"Failed finalization must not offer installation");
                    fs::remove(stage/"downloaded.json.tmp"); cached.stop();
                }
                writeText(stage/name,"invalid cache");
                corrupt = true; transfers.clear();
                QWidget retryWindow;
                kemu::UpdateController retry(&retryWindow,[]{return QString();},[]{return QString();},[&]{return history;},transfer);
                auto* retryDialog = retryWindow.findChild<QDialog*>("KemuSettings"); retryDialog->setAttribute(Qt::WA_DontShowOnScreen);
                auto* retryButton = retryDialog->findChild<QPushButton*>("KemuCheckUpdate");
                retryButton->click();
                confirmDownload(retryWindow, true);
                waitUntil([&] { return retryButton->isEnabled() && retryDialog->isVisible(); });
                require(!retryWindow.findChild<QMessageBox*>() && !fs::exists(stage/"downloaded.json"), "A corrupt installer must not be offered");
                corrupt = false; retryButton->click();
                confirmDownload(retryWindow, true);
                waitUntil([&] { return retryWindow.findChild<QMessageBox*>("KemuUpdateReady") != nullptr; });
                require(transfers.size()==4 && readJson(stage/"downloaded.json").at("from")=="v0.1.0", "A corrupt full installer must allow retry from a known version");
                retryWindow.findChild<QMessageBox*>("KemuUpdateReady")->reject();
                QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
                writeText(stage/name,"invalid cache"); pauseDownload = true;
                retryButton->click(); confirmDownload(retryWindow, true); waitUntil([&] { return downloading.load(); });
                retryDialog->findChild<QPushButton*>("KemuCancelUpdate")->click();
                waitUntil([&] { return retryButton->isEnabled(); });
                require(!fs::exists(stage/"downloaded.json") && !retryWindow.findChild<QMessageBox*>(), "Cancelled downloads must not be offered");
                require(retryDialog->findChild<QLabel*>("KemuUpdateStatus")->text() ==
                        "Update cancelled. Downloaded files were kept; valid cached files will be reused when you retry.",
                        "Cancellation must explain that valid cached files can be reused");
                pauseDownload = false; retryButton->click();
                confirmDownload(retryWindow, true);
                waitUntil([&] { return retryWindow.findChild<QMessageBox*>("KemuUpdateReady") != nullptr; });
                require(fs::exists(stage/"downloaded.json"), "Cancelled downloads must allow a clean retry");
                retry.stop();
            }
        }
    }
    for (const bool enabled : {false, true}) {
        QTemporaryDir temporary; const fs::path root(temporary.path().toStdWString());
        qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8()); qputenv("ANDROID_AVD_HOME",temporary.path().toUtf8());
        writeText(root/"VERSION.txt","v0.1.0\n"); writeText(root/".kemu-update/cached","old task");
        writeJson(root/".kemu-update/task.json", {{"full_installer",true},{"to","v0.1.0"}});
        std::atomic<int> checks{0};
        QSettings(QSettings::defaultFormat(),QSettings::UserScope,"Kemu","Kemu").setValue("updates/automatic",enabled);
        QWidget window;
        kemu::UpdateController controller(&window,[]{return QString();},[]{return QString();},[&] {
            ++checks;
            return Json{{"schema_version",1},{"releases",Json::array({{{"version","v0.1.0"},{"files",Json::array()}}})}};
        });
        QEventLoop wait; QTimer::singleShot(2300,&wait,&QEventLoop::quit); wait.exec();
        controller.stop();
        require(checks==(enabled ? 1 : 0) && !window.findChild<QDialog*>("KemuSettings")->isVisible(), "Disabled checks and up-to-date startup must stay quiet");
        require(!fs::exists(root/".kemu-update/cached"), "Startup must clean successful full updates even when automatic updates are disabled");
    }
}
static void checkUnknownVersionStartup() {
    using namespace kemu::update;
    for (const bool automatic : {false,true}) {
        for (const std::string state : {"missing","invalid","locked","lost-during-check"}) {
            QTemporaryDir temporary; const fs::path root(temporary.path().toStdWString()), stage=root/".kemu-update";
            qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8()); qputenv("ANDROID_AVD_HOME",temporary.path().toUtf8());
            if (state!="missing") writeText(root/"VERSION.txt",state=="invalid" ? "unknown" : "v0.1.0\n");
            writeJson(stage/"task.json",{{"from","v0.1.0"},{"to","v0.2.0"},{"full_installer",false}});
            writeText(stage/"history.json","cached history"); writeText(stage/"downloaded.json","cached ready marker");
            writeText(stage/"payload/a.dll","cached payload"); writeText(stage/"v0.2.0.tar.xz","cached archive");
            std::atomic<int> checks{0};
            QSettings(QSettings::defaultFormat(),QSettings::UserScope,"Kemu","Kemu").setValue("updates/automatic",automatic);
            QWidget window;
            kemu::UpdateController controller(&window,[]{return QString();},[]{return QString();},[&] {
                ++checks; fs::remove(root/"VERSION.txt");
                return Json{{"schema_version",1},{"releases",Json::array({{{"version","v0.3.0"},{"files",Json::array()}}})}};
            });
            auto* dialog=window.findChild<QDialog*>("KemuSettings"); dialog->setAttribute(Qt::WA_DontShowOnScreen);
            if (!automatic) dialog->show();
            auto* status=dialog->findChild<QLabel*>("KemuUpdateStatus"); const auto previousStatus=status->text();
            auto snapshot = [&] {
                std::map<std::string,std::pair<std::string,fs::file_time_type>> result;
                for (const auto& entry : fs::recursive_directory_iterator(root)) {
                    if (entry.path()==root/"VERSION.txt") continue;
                    result.emplace(entry.path().u8string(),std::make_pair(entry.is_regular_file() ? readText(entry.path()) : "<directory>",entry.last_write_time()));
                }
                return result;
            };
            const auto before=snapshot();
            HANDLE versionLock=INVALID_HANDLE_VALUE;
            if (state=="locked") {
                versionLock=CreateFileW((root/"VERSION.txt").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
                require(versionLock!=INVALID_HANDLE_VALUE,"Cannot lock the UI version fixture");
            }
            if (!automatic) dialog->findChild<QPushButton*>("KemuCheckUpdate")->click();
            QEventLoop wait; QTimer::singleShot(automatic ? 2300 : 150,&wait,&QEventLoop::quit); wait.exec();
            waitUntil([&] { return dialog->findChild<QPushButton*>("KemuCheckUpdate")->isEnabled(); });
            controller.stop();
            if (versionLock!=INVALID_HANDLE_VALUE) CloseHandle(versionLock);
            require(checks==(state=="lost-during-check" ? 1 : 0),"Unknown versions must skip both manual and automatic network checks");
            require(snapshot()==before,"Unknown versions must preserve all cached files and timestamps without creating temporary files or error logs");
            require(dialog->isVisible()==!automatic && status->text()==previousStatus && !window.findChild<QMessageBox*>(),
                    "Unknown versions must not open settings, replace status text or offer downloads");
        }
    }
}
static void checkUpdateNavigation() {
    using namespace kemu::update;
    for (const bool failure : {false,true}) {
        QTemporaryDir temporary; const fs::path root(temporary.path().toStdWString());
        qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8()); qputenv("ANDROID_AVD_HOME",temporary.path().toUtf8());
        writeText(root/"VERSION.txt","v0.1.0\n"); kemu::setUiLanguage("en");
        QSettings(QSettings::defaultFormat(),QSettings::UserScope,"Kemu","Kemu").setValue("updates/automatic",false);
        QWidget window;
        kemu::UpdateController controller(&window,[]{return QString();},[]{return QString();},[&]() -> Json {
            if (failure) throw std::runtime_error("Fixture update failure");
            return {{"schema_version",1},{"releases",Json::array({{{"version","v0.2.0"},{"files",Json::array()}}})}};
        });
        auto* dialog=window.findChild<QDialog*>("KemuSettings"); dialog->setAttribute(Qt::WA_DontShowOnScreen);
        auto* tabs=dialog->findChild<QListWidget*>("KemuSettingsTabs");
        auto* pages=dialog->findChild<QStackedWidget*>("KemuSettingsPages");
        auto* updates=dialog->findChild<QWidget*>("KemuUpdatesPage");
        auto* generalTab=tabs->item(0); auto* updatesTab=tabs->findItems("Updates",Qt::MatchExactly).at(0);
        tabs->takeItem(tabs->row(updatesTab)); tabs->addItem(updatesTab);
        pages->removeWidget(updates); pages->insertWidget(0,updates);
        tabs->setCurrentItem(updatesTab);
        require(pages->currentWidget()==updates,"Manual navigation must follow the page object after reordering");
        auto* language=dialog->findChild<QComboBox*>("KemuLanguage"); language->setCurrentIndex(language->findData("zh-CN"));
        require(updatesTab->text()==QCoreApplication::translate("Kemu","Updates") && generalTab->text()==QCoreApplication::translate("Kemu","General"),
                "Retranslation must follow navigation items after reordering");
        tabs->setCurrentItem(generalTab);
        dialog->findChild<QPushButton*>("KemuCheckUpdate")->click();
        waitUntil([&] { return dialog->isVisible() && dialog->findChild<QPushButton*>("KemuCheckUpdate")->isEnabled(); });
        require(tabs->currentItem()==updatesTab && pages->currentWidget()==updates,"Update results and errors must select the actual Updates page");
        controller.stop(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
}
static void checkUpdateFailureVisibility() {
    using namespace kemu::update;
    const auto failure = QStringLiteral("Update could not be completed. Please try again later.\n");
    for (const auto [automatic, network, visible] : {std::tuple{true, true, false}, std::tuple{true, false, false},
                                                    std::tuple{false, true, false}, std::tuple{false, true, true}}) {
        QTemporaryDir temporary;
        const fs::path root(temporary.path().toStdWString());
        qputenv("KEMU_INSTALL_ROOT",temporary.path().toUtf8());
        qputenv("ANDROID_AVD_HOME",temporary.path().toUtf8());
        writeText(root/"VERSION.txt","v0.1.0\n");
        if (!network) writeText(root/".kemu-update-pending","{}");
        kemu::setUiLanguage("en");
        QSettings(QSettings::defaultFormat(),QSettings::UserScope,"Kemu","Kemu").setValue("updates/automatic",automatic);
        QWidget window;
        kemu::UpdateController controller(&window,[]{return QString();},[]{return QString();},
            []() -> Json { throw NetworkError("HTTPS request failed; check your connection"); });
        auto* dialog = window.findChild<QDialog*>("KemuSettings");
        dialog->setAttribute(Qt::WA_DontShowOnScreen);
        if (visible) { dialog->findChild<QListWidget*>("KemuSettingsTabs")->setCurrentRow(2); dialog->show(); }
        auto* status = dialog->findChild<QLabel*>("KemuUpdateStatus");
        QEventLoop wait;
        QTimer poll, deadline;
        QObject::connect(&poll,&QTimer::timeout,[&] { if (status->text().startsWith(failure)) wait.quit(); });
        QObject::connect(&deadline,&QTimer::timeout,&wait,&QEventLoop::quit);
        poll.start(10); deadline.start(8000);
        if (!automatic) dialog->findChild<QPushButton*>("KemuCheckUpdate")->click();
        wait.exec(); controller.stop();
        const QString error = network ? "HTTPS request failed; check your connection" : "An unfinished update needs recovery; restart Kemu";
        require(status->text() == failure + error, "Update failure must keep the error in the update status");
        if (network && !visible) {
            require(!dialog->isVisible(), "Network failure must not open settings");
        } else {
            require(dialog->isVisible() && dialog->findChild<QListWidget*>("KemuSettingsTabs")->currentRow() == 2,
                    "Update failure that needs attention must open the Updates page");
        }
    }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    if (argc > 1 && QString::fromUtf8(argv[1]) == "move-progress") {
        try {
            using kemu::update::require;
            for (const auto* language : {"en","zh-CN"}) {
                kemu::setUiLanguage(language);
                kemu::StorageMoveDialog dialog("D:/Kemu data/userdata-qemu.img");
                dialog.setAttribute(Qt::WA_DontShowOnScreen); dialog.show();
                dialog.setProgress(72); app.processEvents();
                auto* bar=dialog.findChild<QProgressBar*>("KemuMoveProgress");
                require(bar && bar->value()==72,"Migration progress missing");
                require(dialog.palette().color(QPalette::Window)==QColor("#1b2028"),"Migration theme mismatch");
                require(dialog.findChild<QLabel*>("KemuMoveStatus")->font().family()=="Microsoft YaHei","Migration font mismatch");
                require(!dialog.close() && dialog.isVisible(),"Closing the progress window must not interrupt a disk move");
                if (argc>2) require(dialog.grab().save(QString::fromUtf8(argv[2])+"."+language+".png"),"Cannot render migration progress");
            }
            const auto avd=std::filesystem::path(temporary.path().toStdWString()) / "avd";
            const auto target=std::filesystem::path(temporary.path().toStdWString()) / "target";
            std::filesystem::create_directories(avd / "kemu.avd"); std::filesystem::create_directories(target);
            { std::ofstream file(kemu::storage::imagePath(avd),std::ios::binary); file << std::string(4096,'A'); }
            kemu::storage::requestGrowth(avd,128); kemu::storage::applyGrowth(avd);
            kemu::storage::completeGrowth(avd,kemu::storage::readRequest(avd));
            kemu::storage::requestMove(avd,target);
            kemu::StorageMoveDialog dialog(QString::fromStdWString(target.wstring()));
            dialog.setAttribute(Qt::WA_DontShowOnScreen);
            require(dialog.move(avd)==0 && std::filesystem::equivalent(kemu::storage::imagePath(avd),target / "userdata-qemu.img"),
                    "The progress window did not complete the disk migration");
            const auto blocked=target.parent_path() / "blocked";
            std::filesystem::create_directories(blocked);
            kemu::storage::requestMove(avd,blocked);
            { std::ofstream file(blocked / "userdata-qemu.img"); file << "existing disk"; }
            kemu::StorageMoveDialog failed(QString::fromStdWString(blocked.wstring()));
            failed.setAttribute(Qt::WA_DontShowOnScreen);
            QTimer dismiss;
            bool warned=false;
            QObject::connect(&dismiss,&QTimer::timeout,[&] {
                if (auto* prompt=failed.findChild<QMessageBox*>(); prompt && prompt->isVisible()) {
                    warned=true; prompt->accept();
                }
            });
            dismiss.start(20);
            require(failed.move(avd)==1 && warned &&
                    std::filesystem::equivalent(kemu::storage::imagePath(avd),target / "userdata-qemu.img"),
                    "Migration errors must keep the saved disk and show a dismissible warning");
            dismiss.stop();
            std::cout << "Migration progress UI and worker checks passed\n"; return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc > 1 && QString::fromUtf8(argv[1]) == "major") {
        try { kemu::update::require(argc == 3,"Pass update fixtures directory"); checkDownloadFlow(kemu::update::fs::u8path(argv[2])); std::cout << "Update download and startup UI checks passed\n"; return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc > 1 && QString::fromUtf8(argv[1]) == "failure") {
        try { checkUnknownVersionStartup(); checkUpdateNavigation(); checkUpdateFailureVisibility(); std::cout << "Update failure UI checks passed\n"; return 0; }
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
        const auto frameTime = [&](int frame, int rate) {
            return start + nanoseconds(int64_t(frame) * 1000000000 / rate);
        };
        const auto expectRate = [](double actual, double expected) {
            require(qAbs(actual - expected) < 0.0001, "Measured FPS differs from frame cadence");
        };
        for (int frame = 1; frame <= 30; ++frame) counter.recordFrame(frameTime(frame, 60));
        require(counter.sample(start) == 0.0, "Zero elapsed time must not consume frames");
        expectRate(counter.sample(start + milliseconds(500)), 60.0);
        require(counter.sample(start + seconds(1)) == 0.0, "Idle frame rate must return to zero");
        for (int frame = 1; frame <= 90; ++frame) counter.recordFrame(seconds(1) + frameTime(frame, 60));
        expectRate(counter.sample(start + milliseconds(2510)), 60.0);

        for (int rate : {15, 30, 60, 120, 144}) {
            for (int limit : {0, rate, rate * 2}) {
                kemu::FrameRateCounter steady(start, limit);
                int nextFrame = 1;
                for (int pollMs : {990, 2010, 2990, 4010, 7010}) {
                    const auto poll = start + milliseconds(pollMs);
                    while (frameTime(nextFrame, rate) <= poll) {
                        steady.recordFrame(frameTime(nextFrame++, rate));
                    }
                    expectRate(steady.sample(poll), rate);
                }
                require(steady.sample(start + seconds(9)) == 0.0, "An idle stream must report zero FPS");
            }
        }

        kemu::FrameRateCounter stalled(start, 60);
        for (int frame = 1; frame <= 60; ++frame) stalled.recordFrame(frameTime(frame, 60));
        const auto stalledRate = stalled.sample(start + milliseconds(1500));
        require(stalledRate > 39.0 && stalledRate < 41.0, "A stall within the sample must reduce FPS");
        require(stalled.sample(start + milliseconds(2500)) == 0.0, "A stopped stream must report zero FPS");
        for (int frame = 1; frame <= 60; ++frame) stalled.recordFrame(milliseconds(2500) + frameTime(frame, 60));
        expectRate(stalled.sample(start + milliseconds(3510)), 60.0);

        kemu::FrameRateCounter limited(start, 60);
        for (int frame = 1; frame <= 120; ++frame) limited.recordFrame(frameTime(frame, 120));
        expectRate(limited.sample(start + seconds(1)), 60.0);
        kemu::FrameRateCounter single(start);
        single.recordFrame(start + milliseconds(500));
        expectRate(single.sample(start + seconds(1)), 1.0);
        require(single.sample(start + seconds(2)) == 0.0, "A single frame must not keep FPS alive");
        kemu::FrameRateCounter burst(start, 60);
        for (int frame = 0; frame < 120; ++frame) burst.recordFrame(start);
        expectRate(burst.sample(start + seconds(1)), 60.0);
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
    const auto updateNotice = QCoreApplication::translate("Kemu", "Version %1 is available. Download the update now?").arg("v0.2.0");
    require(updateNotice.contains("v0.2.0") && updateNotice.contains(QStringLiteral("\u65b0\u7248\u672c")),
        "Update confirmation translation or version placeholder is missing");
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
            require(dialog->findChild<QListWidget*>("KemuSettingsTabs")->count()==4,"Settings navigation missing");
            require(dialog->findChild<QPushButton*>("KemuCheckUpdate")->isEnabled(),"Check update disabled");
            require(!dialog->findChild<QPushButton*>("KemuInstallUpdate") && !dialog->findChild<QPushButton*>("KemuDownloadUpdate"),"Obsolete buttons remain");
            if (ready) {
                promptCloser.stop(); require(!startupPrompt, "Startup must show the Updates page without prompting");
                require(dialog->findChild<QListWidget*>("KemuSettingsTabs")->currentRow()==2, "Cached update must open the Updates page");
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
            dialog->findChild<QListWidget*>("KemuSettingsTabs")->setCurrentRow(0);
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
            auto* aboutVersion = dialog->findChild<QLabel*>("KemuAboutVersion");
            auto* updateVersion = dialog->findChild<QLabel*>("KemuCurrentVersion");
            for (const auto* language : {"zh-CN", "en"}) {
                languages->setCurrentIndex(languages->findData(language));
                tabs->setCurrentRow(3); app.processEvents();
                const auto aboutTab = QLatin1String(language) == "zh-CN" ? QStringLiteral("\u5173\u4e8e") : QStringLiteral("About");
                require(tabs->item(3)->text() == aboutTab &&
                        dialog->findChild<QStackedWidget*>()->currentWidget()->objectName() == "KemuAbout",
                        "About must be the last settings page in both languages");
                const auto expectedVersion = missing
                    ? QCoreApplication::translate("Kemu", "Current version: Unknown")
                    : QCoreApplication::translate("Kemu", "Current version: ") + "v0.1.0";
                require(aboutVersion && updateVersion && aboutVersion->isVisible() &&
                        aboutVersion->text() == expectedVersion && aboutVersion->text() == updateVersion->text(),
                        "About and Updates must display the same detected version");
                auto* logo = dialog->findChild<QLabel*>("KemuAboutLogo");
                auto* github = dialog->findChild<QLabel*>("KemuAboutGitHub");
                require(logo && !logo->pixmap(Qt::ReturnByValue).isNull() && github && github->openExternalLinks() &&
                        github->text().contains("href=\"https://github.com/kuloPo/Kemu\""),
                        "About must display the bundled logo and clickable project link");
                if (argc > 2) require(dialog->grab().save(QString::fromUtf8(argv[2]) + ".about-" + language + ".png"),
                        "Cannot save About UI rendering");
            }
            languageChanged = false;
            languages->setCurrentIndex(languages->findData("zh-CN"));
            require(languageChanged && kemu::uiLanguage() == "zh-CN" && dialog->windowTitle() != "Settings", "Switch back failed");
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
            auto* move = dialog->findChild<QPushButton*>("KemuMoveDisk");
            auto* diskPath = dialog->findChild<QLabel*>("KemuStoragePath");
            app.processEvents();
            require(move && move->isEnabled() && diskPath &&
                    diskPath->toolTip()==QDir::toNativeSeparators(QString::fromStdWString(kemu::storage::imagePath(storageRoot).wstring())),
                    "Current disk path or Move button missing");
            for (const auto* language : {"en","zh-CN"}) {
                languages->setCurrentIndex(languages->findData(language)); app.processEvents();
                require(expand->size()==ram->size() && move->size()==ram->size(),"Disk buttons must match the RAM input size");
                require(diskPath->mapTo(dialog,QPoint()).y()>storage->mapTo(dialog,QPoint()).y(),"Disk path must be below capacity");
                require(move->mapTo(dialog,QPoint()).x()>diskPath->mapTo(dialog,QPoint()).x(),"Move must be to the right of the path");
                if (argc>2) require(dialog->grab().save(QString::fromUtf8(argv[2])+".disk-"+language+".png"),"Cannot render disk controls");
            }
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);
            bool selectedFolder=false;
            QTimer pickerCloser;
            QObject::connect(&pickerCloser,&QTimer::timeout,[&] {
                if (auto* picker=dialog->findChild<QFileDialog*>(); picker && picker->isVisible()) {
                    selectedFolder=true; picker->reject();
                }
            });
            pickerCloser.start(20);
            move->click();
            pickerCloser.stop();
            require(selectedFolder && !kemu::update::fs::exists(kemu::storage::moveRequestPath(storageRoot)),
                    "Cancelling the folder picker must not queue a disk move");
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
