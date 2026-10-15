#include "UpdateController.h"
#include "UiFont.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QPainter>
#include <QStyleOptionButton>
#include <QDesktopServices>
#include <QUrl>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <windows.h>

namespace kemu {
using namespace update;
class UpdateCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    void paintEvent(QPaintEvent* event) override {
        QCheckBox::paintEvent(event);
        if (!isChecked()) return;
        QStyleOptionButton option; initStyleOption(&option);
        const auto box = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(QPolygonF{QPointF(box.left()+4, box.top()+8), QPointF(box.left()+7, box.top()+11), QPointF(box.left()+13, box.top()+5)});
    }
};
static QString qpath(const fs::path& path) { return QString::fromStdWString(path.wstring()); }
UpdateController::UpdateController(QWidget* parent, std::function<QString()> adb, std::function<QString()> serial, std::function<update::Json()> history)
    : QObject(parent), mWindow(parent), mAdb(std::move(adb)), mSerial(std::move(serial)), mHistory(std::move(history)) {
    auto root = qEnvironmentVariable("KEMU_INSTALL_ROOT");
    if (root.isEmpty()) root = qEnvironmentVariable("ANDROID_EMULATOR_LAUNCHER_DIR");
    mRoot = fs::path(root.toStdWString());
    mDialog = new QDialog(parent); mDialog->setObjectName("KemuSettings");
    mDialog->setWindowTitle(QStringLiteral("设置")); mDialog->resize(680, 400);
    mDialog->setStyleSheet("QWidget {color:#edf1f6;} QDialog {background:#1b2028;} QListWidget {background:#151b24;border:1px solid #293343;border-radius:10px;padding:8px;outline:0;} QListWidget::item {height:42px;padding-left:16px;border-radius:7px;} QListWidget::item:hover {background:#252f3e;} QListWidget::item:selected {background:#29456b;color:#b8d8ff;} QPushButton {padding:9px 18px;border:1px solid #465468;border-radius:7px;background:#29323e;} QPushButton#KemuCheckUpdate {background:#3478cb;border-color:#448fe8;} QPushButton:hover {background:#3b506c;} QPushButton:disabled {color:#75808f;background:#20262f;} QCheckBox {spacing:9px;} QCheckBox::indicator {width:16px;height:16px;border-radius:4px;border:1px solid #75808f;background:#15191f;} QCheckBox::indicator:checked {background:#508ee0;border-color:#9bc5ff;} QProgressBar {border:0;border-radius:5px;background:#101720;text-align:center;min-height:20px;} QProgressBar::chunk {background:#438fe9;border-radius:5px;}");
    auto* layout = new QHBoxLayout(mDialog); auto* tabs = new QListWidget(mDialog);
    tabs->setObjectName("KemuSettingsTabs"); layout->setContentsMargins(16,16,16,16); layout->setSpacing(24); tabs->setFixedWidth(150); tabs->setFocusPolicy(Qt::NoFocus); tabs->addItem(QStringLiteral("更新")); layout->addWidget(tabs);
    auto* pages = new QStackedWidget(mDialog); layout->addWidget(pages, 1); auto* page = new QWidget(pages); pages->addWidget(page);
    connect(tabs, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex); tabs->setCurrentRow(0);
    auto* column = new QVBoxLayout(page); column->setSpacing(14);
    mVersion = new QLabel(QStringLiteral("当前版本：读取中…"), page); column->addWidget(mVersion);
    mStatus = new QLabel(QStringLiteral("点击检查更新获取新版本。"), page); mStatus->setObjectName("KemuUpdateStatus"); mStatus->setWordWrap(true); mStatus->setTextFormat(Qt::PlainText); column->addWidget(mStatus);
    mAutoUpdate = new UpdateCheckBox(QStringLiteral("自动更新"), page); mAutoUpdate->setObjectName("KemuAutoUpdate");
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
    bool enabled = settings.value("updates/automatic", settings.value("updates/autoUpdate", true).toBool() || settings.value("updates/autoDownload", false).toBool()).toBool();
    mAutoUpdate->setChecked(enabled); settings.setValue("updates/automatic", enabled);
    settings.remove("updates/autoDownload"); settings.remove("updates/autoUpdate");
    column->addWidget(mAutoUpdate);
    connect(mAutoUpdate, &QCheckBox::toggled, this, [](bool on) { QSettings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu").setValue("updates/automatic",on); });
    auto* buttons = new QHBoxLayout;
    mCheck = new QPushButton(QStringLiteral("检查更新"), page); mCheck->setObjectName("KemuCheckUpdate");
    buttons->addWidget(mCheck); buttons->addStretch(); column->addLayout(buttons);
    mProgress = new QProgressBar(page); mProgress->setObjectName("KemuDownloadProgress"); mProgress->hide(); column->addWidget(mProgress);
    mCancelButton = new QPushButton(QStringLiteral("取消"), page); mCancelButton->setObjectName("KemuCancelUpdate"); mCancelButton->hide(); column->addWidget(mCancelButton, 0, Qt::AlignLeft); column->addStretch();
    connect(mCheck, &QPushButton::clicked, this, [this] { check(); });
    connect(mCancelButton, &QPushButton::clicked, this, [this] { mCancel = true; mCancelButton->setEnabled(false); status(QStringLiteral("正在取消下载…")); });
    applyUiFont(mDialog);
    refresh();
    QTimer::singleShot(2000, this, [this] { startup(); });
}
UpdateController::~UpdateController() { stop(); }
void UpdateController::stop() { mClosing = true; mCancel = true; if (mWorker) { mWorker->wait(); delete mWorker; mWorker = nullptr; } }
void UpdateController::showSettings() { mDialog->show(); mDialog->raise(); mDialog->activateWindow(); }
void UpdateController::status(const QString& text) {
    if (QThread::currentThread() != thread()) { QMetaObject::invokeMethod(this, [this, text] { if (!mClosing) mStatus->setText(text); }, Qt::QueuedConnection); }
    else mStatus->setText(text);
}
void UpdateController::refresh() {
    mCheck->setEnabled(!mBusy);
    if (!mBusy) { mProgress->hide(); mCancelButton->hide(); }
}
void UpdateController::work(std::function<void()> task, std::function<void()> finished) {
    if (mBusy || mClosing) return;
    mBusy = true; mCancel = false; refresh(); auto error = std::make_shared<QString>();
    mWorker = QThread::create([task, error] { try { task(); } catch (const std::exception& e) { *error = QString::fromUtf8(e.what()); } });
    connect(mWorker, &QThread::finished, this, [this, error, finished] {
        if (mClosing) return; mWorker->deleteLater(); mWorker = nullptr; mBusy = false;
        if (!error->isEmpty() || mCancel) {
            mDownloaded = false; mPlan = {};
            try { if (!mStage.empty() && fs::is_directory(mStage)) writeText(mStage / "error.txt", error->toStdString()); } catch (...) {}
            status(mCancel ? QStringLiteral("下载已取消，可重新检查更新。") : QStringLiteral("更新未完成，请稍后重试。\n") + *error); showSettings();
        } else finished();
        refresh();
    });
    mWorker->start();
}
void UpdateController::startup() {
    if (mBusy || mDownloaded) return;
    try { mVersion->setText(QStringLiteral("当前版本：") + QString::fromStdString(readVersion(mRoot))); }
    catch (...) { mVersion->setText(QStringLiteral("当前版本：未知")); status(QString()); return; }
    if (mAutoUpdate->isChecked() || fs::exists(mRoot / ".kemu-update")) check(true);
}
void UpdateController::check(bool automatic) {
    if (mBusy) return;
    try { readVersion(mRoot); } catch (...) {
        mVersion->setText(QStringLiteral("当前版本：未知"));
        status(QString());
        if (!automatic) {
            UiMessageBox prompt(QMessageBox::Information, QStringLiteral("无法检测当前版本"),
                QStringLiteral("无法检测当前版本，点击确定后前往下载完整安装包。"), QMessageBox::NoButton, mDialog);
            auto* confirm = prompt.addButton(QStringLiteral("确定"), QMessageBox::AcceptRole);
            prompt.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
            prompt.exec();
            if (prompt.clickedButton() == confirm) QDesktopServices::openUrl(QUrl(QString::fromUtf8(ReleasesUrl)));
        }
        return;
    }
    work([this] {
        status(QStringLiteral("正在检查更新…")); mDownloaded = false; mPlan = {};
        readVersion(mRoot);
        require(!fs::exists(mRoot / ".kemu-update-pending"), "An unfinished update needs recovery; restart Kemu");
        auto latest = mRoot / ".kemu-update-history.tmp";
        if (mHistory) writeJson(latest, mHistory());
        else download(HistoryUrl, latest, 8*1024*1024, [this] { return mCancel.load(); });
        mPlan = prepareUpdateTask(mRoot, readJson(latest)); fs::remove(latest);
        mStage = mRoot / ".kemu-update";
        mDownloaded = loadDownloadedUpdate(mRoot, mStage, mPlan);
    }, [this, automatic] {
        mVersion->setText(QStringLiteral("当前版本：") + QString::fromStdString(mPlan.current));
        if (mDownloaded) {
            status(QStringLiteral("更新已就绪，可点击检查更新继续安装。"));
            if (!automatic || mAutoUpdate->isChecked()) offerInstall();
            return;
        }
        if (mPlan.current == mPlan.target) status(QStringLiteral("当前已是最新版本。"));
        else { status(QStringLiteral("发现新版本 %1，需要更新 %2 个文件。 ").arg(QString::fromStdString(mPlan.target)).arg(mPlan.files.size())); if (automatic && !mAutoUpdate->isChecked()) return; if (automatic || [this] {
            UiMessageBox prompt(QMessageBox::Question, QStringLiteral("发现新版本"),
                QStringLiteral("发现新版本 %1，是否下载更新？").arg(QString::fromStdString(mPlan.target)), QMessageBox::NoButton, mDialog);
            auto* yes = prompt.addButton(QStringLiteral("是"), QMessageBox::YesRole);
            prompt.addButton(QStringLiteral("否"), QMessageBox::NoRole);
            prompt.exec(); return prompt.clickedButton() == yes;
        }()) fetch(automatic); }
    });
}
void UpdateController::fetch(bool automatic) {
    mProgress->setRange(0, 0); mProgress->show(); mCancelButton->setEnabled(true); mCancelButton->show();
    work([this] {
        require(readVersion(mRoot) == mPlan.current, "Local version changed; check updates again");
        std::map<std::string, Release> packs;
        for (const auto& [path, release] : mPlan.files) {
            if (path[0] == '/') require(path.rfind("/system/",0) == 0 || path.rfind("/vendor/",0) == 0 ||
                path.rfind("/system_ext/",0) == 0 || path.rfind("/product/",0) == 0,
                "Image burner cannot update this Android file: " + path + ". Download the full release: " + ReleasesUrl);
            packs[release.version] = release;
        }
        for (const auto& [version, release] : packs) {
            status(QStringLiteral("正在下载并校验 %1 的更新包…").arg(QString::fromStdString(version)));
            auto archive = mStage / (version + ".zip"), extracted = mStage / version;
            safePath(mStage, version + ".zip");
            auto unpack = [&] {
                clearUpdateDirectory(mRoot, extracted);
                extractPack(archive, extracted);
                auto hashes = parseChecksums(readText(extracted / "sha256sum.txt"));
                for (const auto& path : release.files) {
                    auto packed = path[0] == '/' ? "Android" + path : "Windows/" + path;
                    require(hashes.count(packed), "History file is missing from the pack: " + path);
                }
            };
            bool cached = false;
            if (fs::is_regular_file(archive)) {
                try { unpack(); cached = true; }
                catch (const std::exception&) { clearUpdateDirectory(mRoot, extracted); fs::remove(archive); }
            }
            if (!cached) {
                auto partial = mStage / (version + ".zip.part");
                safePath(mStage, version + ".zip.part");
                download(std::string(ReleasesUrl) + "/download/" + version + "/update-pack.zip", partial, 8ull*1024*1024*1024, [this] { return mCancel.load(); },
                [this](uint64_t done, uint64_t total) {
                    QMetaObject::invokeMethod(this, [this, done, total] {
                        if (mClosing) return;
                        mProgress->setRange(0, total ? 100 : 0);
                        if (total) mProgress->setValue(int(std::min<uint64_t>(100, done * 100 / total)));
                    }, Qt::QueuedConnection);
                });
                require(!mCancel, "Update cancelled");
                fs::rename(partial, archive);
                unpack();
            }
            require(!mCancel, "Update cancelled");
        }
        clearUpdateDirectory(mRoot, mStage / "payload"); clearUpdateDirectory(mRoot, mStage / "android");
        fs::create_directories(mStage / "payload"); fs::create_directories(mStage / "android");
        for (const auto& [path, release] : mPlan.files) {
            auto packed = path[0] == '/' ? "Android" + path : "Windows/" + path;
            auto destination = path[0] == '/' ? mStage / "android" / fs::u8path(path.substr(1)) : mStage / "payload" / fs::u8path(path);
            fs::create_directories(destination.parent_path()); fs::copy_file(mStage / release.version / fs::u8path(packed), destination);
        }
        require(!mCancel, "Update cancelled");
        saveDownloadedUpdate(mStage, mPlan);
        mDownloaded = true;
    }, [this, automatic] {
        mProgress->hide(); mCancelButton->hide();
        status(QStringLiteral("更新已下载，下次启动时将询问是否更新，也可点击检查更新立即安装。"));
        if (!automatic) offerInstall();
    });
}
void UpdateController::offerInstall() {
    UiMessageBox prompt(QMessageBox::Information, QStringLiteral("更新已就绪"),
        QStringLiteral("更新已下载并通过校验。立即更新将关闭并重启模拟器。"), QMessageBox::NoButton, mDialog);
    auto* now = prompt.addButton(QStringLiteral("立即更新"), QMessageBox::AcceptRole);
    prompt.addButton(QStringLiteral("稍后更新"), QMessageBox::RejectRole);
    prompt.exec(); if (prompt.clickedButton() == now) install();
}
QByteArray UpdateController::adbCommand(const QString& adb, const QString& serial, const QStringList& args, int timeout) {
    require(!adb.isEmpty() && serial.startsWith("emulator-"), "ADB for this emulator is unavailable");
    QProcess process; process.setProcessChannelMode(QProcess::MergedChannels); process.start(adb, QStringList{"-s", serial} + args);
    require(process.waitForStarted(10000), "Cannot start ADB"); QElapsedTimer timer; timer.start(); QByteArray output;
    while (!process.waitForFinished(100)) {
        output += process.readAll(); if (output.size() > 4*1024*1024) output = output.right(4*1024*1024);
        if (mCancel || timer.elapsed() > timeout) { process.kill(); process.waitForFinished(5000); throw std::runtime_error("ADB operation cancelled or timed out"); }
    }
    output += process.readAll(); require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, "ADB failed: " + output.toStdString()); return output;
}
void UpdateController::prepare(const QString& adb, const QString& serial) {
    require(readVersion(mRoot) == mPlan.current, "Local version changed; check updates again");
    // Update the idle helper before it is launched, never while it is running.
    auto replacement = mStage / "payload/KemuUpdater.exe", helper = mRoot / "KemuUpdater.exe";
    if (fs::exists(replacement)) {
        auto record = mPlan.files.find("KemuUpdater.exe"); require(record != mPlan.files.end(), "Unexpected updater replacement");
        auto sums = parseChecksums(readText(mStage / record->second.version / "sha256sum.txt"));
        require(sha256(replacement) == sums.at("Windows/KemuUpdater.exe"), "Updater hash mismatch");
        status(QStringLiteral("正在更新独立更新器…")); safePath(mRoot, "KemuUpdater.exe");
        if (fs::exists(helper)) require(ReplaceFileW(helper.c_str(), replacement.c_str(), (mStage / "previous-updater.exe").c_str(), 0, nullptr, nullptr), "Cannot replace the updater");
        else require(MoveFileExW(replacement.c_str(), helper.c_str(), MOVEFILE_WRITE_THROUGH), "Cannot install the updater");
    }
    require(fs::is_regular_file(helper), "KemuUpdater.exe is missing. Download the full release: " + std::string(ReleasesUrl));
    Json android = Json::array();
    for (const auto& [path, release] : mPlan.files) if (path[0] == '/') {
        auto sums = parseChecksums(readText(mStage / release.version / "sha256sum.txt")); auto hash = sums.at("Android" + path);
        require(sha256(mStage / "android" / fs::u8path(path.substr(1))) == hash, "Android payload changed after download");
        android.push_back({{"path",path},{"source",path.substr(1)},{"sha256",hash}});
    }
    std::string baseImageHash;
    if (!android.empty()) {
        status(QStringLiteral("正在模拟器内制作新的系统镜像，请保持模拟器运行…"));
        QElapsedTimer bootWait; bootWait.start();
        for (;;) {
            require(!mCancel && bootWait.elapsed() < 300000, "Android did not finish booting before installation");
            try { if (adbCommand(adb, serial, {"shell","getprop","sys.boot_completed"}, 5000).trimmed() == "1") break; }
            catch (const std::exception&) { require(!mCancel, "Update cancelled"); }
            QThread::msleep(250);
        }
        adbCommand(adb, serial, {"shell","test","-x","/system/bin/kemu_image_burner"});
        auto base = sha256(mRoot / "system-img/system.img");
        baseImageHash = base;
        writeJson(mStage / "burn.json", {{"schema_version",1},{"base_image_sha256",base},{"files",android}});
        QString guest = "/data/local/tmp/kemu-update";
        adbCommand(adb, serial, {"shell","rm","-rf",guest});
        adbCommand(adb, serial, {"shell","mkdir","-p",guest + "/input"});
        adbCommand(adb, serial, {"push",qpath(mRoot / "system-img/system.img"),guest + "/input/system.img"}, 30*60*1000);
        adbCommand(adb, serial, {"push",qpath(mStage / "android"),guest + "/payload"}, 30*60*1000);
        adbCommand(adb, serial, {"push",qpath(mStage / "burn.json"),guest + "/job.json"});
        adbCommand(adb, serial, {"shell","/system/bin/kemu_image_burner","--input",guest + "/input/system.img","--manifest",guest + "/job.json","--payload-dir",guest + "/payload","--output-dir",guest + "/output"}, 60*60*1000);
        adbCommand(adb, serial, {"pull",guest + "/output/result.json",qpath(mStage / "burn-result.json")});
        auto result = readJson(mStage / "burn-result.json");
        require(result.at("status") == "ready" && result.at("base_image_sha256") == base && result.at("files") == android, "Burner did not produce the requested verified image");
        auto digest = result.at("image_sha256").get<std::string>(); validateHash(digest);
        auto paramsDigest = adbCommand(adb, serial, {"shell","sha256sum",guest + "/output/VerifiedBootParams.textproto"}).left(64).toStdString(); validateHash(paramsDigest);
        status(QStringLiteral("正在取回并重新校验系统镜像…"));
        fs::create_directories(mStage / "payload/system-img");
        adbCommand(adb, serial, {"pull",guest + "/output/system.img",qpath(mStage / "payload/system-img/system.img")}, 30*60*1000);
        adbCommand(adb, serial, {"pull",guest + "/output/VerifiedBootParams.textproto",qpath(mStage / "payload/system-img/VerifiedBootParams.textproto")});
        require(sha256(mStage / "payload/system-img/system.img") == digest && sha256(mStage / "payload/system-img/VerifiedBootParams.textproto") == paramsDigest, "Exported image/boot parameters SHA-256 mismatch");
        require(sha256(mRoot / "system-img/system.img") == base, "Input system image changed while preparing update");
        // The unique directory contains only this job's input, output and payload.
        adbCommand(adb, serial, {"shell","rm","-rf",guest});
    }
    Json files = Json::array();
    for (const auto& entry : fs::recursive_directory_iterator(mStage / "payload")) if (entry.is_regular_file()) {
        auto path = fs::relative(entry.path(), mStage / "payload").generic_u8string(); require(path != "VERSION.txt", "Unexpected packaged VERSION.txt");
        safePath(mRoot, path); auto hash = sha256(entry.path());
        auto selected = mPlan.files.find(path);
        require(selected != mPlan.files.end() || (!android.empty() && (path == "system-img/system.img" || path == "system-img/VerifiedBootParams.textproto")), "Unexpected prepared payload: " + path);
        if (selected != mPlan.files.end()) require(hash == parseChecksums(readText(mStage / selected->second.version / "sha256sum.txt")).at("Windows/" + path), "Windows payload changed after download");
        auto previous = path == "system-img/system.img" ? Json(baseImageHash) : fs::exists(mRoot / fs::u8path(path)) ? Json(sha256(mRoot / fs::u8path(path))) : Json(nullptr);
        files.push_back({{"path",path},{"sha256",hash},{"previous_sha256",previous}});
    }
    writeText(mStage / "payload/VERSION.txt", mPlan.target + "\n");
    files.push_back({{"path","VERSION.txt"},{"sha256",sha256(mStage / "payload/VERSION.txt")},{"previous_sha256",sha256(mRoot / "VERSION.txt")}});
    writeJson(mStage / "job.json", {{"schema_version",1},{"from",mPlan.current},{"to",mPlan.target},{"files",files}});
    validateJob(mRoot, mStage);
}
void UpdateController::install() {
    auto adb = mAdb(), serial = mSerial();
    work([this, adb, serial] { fs::remove(mStage / "downloaded.json"); prepare(adb, serial); }, [this] {
        try {
            require(!fs::exists(mRoot / ".kemu-update-pending"), "Another update is pending");
            fs::remove(mStage / "updater-ready");
            writeJson(mRoot / ".kemu-update-pending", {{"stage",mStage.filename().u8string()}});
            require(QProcess::startDetached(qpath(mRoot / "KemuUpdater.exe"), {"--apply",qpath(mStage),QString::number(GetCurrentProcessId())}, qpath(mRoot)), "Cannot start updater");
            mBusy = true; status(QStringLiteral("镜像与文件校验完成，正在交接给更新器…"));
            auto* timer = new QTimer(this); auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
            connect(timer, &QTimer::timeout, this, [this, timer, elapsed] {
                if (fs::exists(mStage / "updater-ready")) { timer->stop(); timer->deleteLater(); mWindow->close(); }
                else if (elapsed->elapsed() > 120000) { timer->stop(); timer->deleteLater(); status(QStringLiteral("更新器未就绪，未关闭模拟器。请重新启动 Kemu 以恢复。")); mBusy = false; refresh(); }
            }); timer->start(200);
        } catch (const std::exception& e) { status(QString::fromUtf8(e.what())); }
    });
}
}

