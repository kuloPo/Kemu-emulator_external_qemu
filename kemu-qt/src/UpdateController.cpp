#include "UpdateController.h"
#include "UiFont.h"
#include "PerformancePage.h"
#include "UiLanguage.h"
#include "../launcher/StorageConfig.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QFileDialog>
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
static QString rootScript(QString script) {
    script.replace("'", "'\\''");
    return "su 0 sh -c '" + script + "'";
}
UpdateController::UpdateController(QWidget* parent, std::function<QString()> adb, std::function<QString()> serial, std::function<update::Json()> history)
    : QObject(parent), mWindow(parent), mAdb(std::move(adb)), mSerial(std::move(serial)), mHistory(std::move(history)) {
    initializeUiTranslations();
    mWindow->installEventFilter(this);
    auto root = qEnvironmentVariable("KEMU_INSTALL_ROOT");
    if (root.isEmpty()) root = qEnvironmentVariable("ANDROID_EMULATOR_LAUNCHER_DIR");
    mRoot = fs::path(root.toStdWString());
    auto avd = qEnvironmentVariable("ANDROID_AVD_HOME");
    if (avd.isEmpty()) avd = qEnvironmentVariable("LOCALAPPDATA") + "/Kemu/Kemu/avd";
    mAvd = fs::path(avd.toStdWString());
    mDialog = new QDialog(parent); mDialog->setObjectName("KemuSettings");
    mDialog->setWindowTitle(QCoreApplication::translate("Kemu", "Settings")); mDialog->resize(800, 590);
    mDialog->setStyleSheet("QWidget {color:#edf1f6;} QDialog {background:#1b2028;} QListWidget {background:#151b24;border:1px solid #293343;border-radius:10px;padding:8px;outline:0;} QListWidget::item {height:42px;padding-left:16px;border-radius:7px;} QListWidget::item:hover {background:#252f3e;} QListWidget::item:selected {background:#29456b;color:#b8d8ff;} QPushButton {padding:9px 18px;border:1px solid #465468;border-radius:7px;background:#29323e;} QPushButton#KemuCheckUpdate {background:#3478cb;border-color:#448fe8;} QPushButton:hover {background:#3b506c;} QPushButton:disabled {color:#75808f;background:#20262f;} QCheckBox {spacing:9px;} QCheckBox::indicator {width:16px;height:16px;border-radius:4px;border:1px solid #75808f;background:#15191f;} QCheckBox::indicator:checked {background:#508ee0;border-color:#9bc5ff;} QProgressBar {border:0;border-radius:5px;background:#101720;text-align:center;min-height:20px;} QProgressBar::chunk {background:#438fe9;border-radius:5px;}");
    auto* layout = new QHBoxLayout(mDialog); auto* tabs = new QListWidget(mDialog);
    tabs->setObjectName("KemuSettingsTabs"); layout->setContentsMargins(16,16,16,16); layout->setSpacing(24); tabs->setFixedWidth(150); tabs->setFocusPolicy(Qt::NoFocus); tabs->addItem(QCoreApplication::translate("Kemu", "General")); tabs->addItem(QCoreApplication::translate("Kemu", "Updates")); layout->addWidget(tabs);
    auto* pages = new QStackedWidget(mDialog); layout->addWidget(pages, 1); auto* general = new QWidget(pages); pages->addWidget(general);
    auto* generalColumn = new QVBoxLayout(general);
    generalColumn->setSpacing(14);
    auto* languageLabel = new QLabel(general);
    auto* language = new QComboBox(general); language->setObjectName("KemuLanguage");
    language->addItem(nativeUiLanguageName("zh-CN"), "zh-CN");
    language->addItem(QStringLiteral("English"), "en");
    language->setCurrentIndex(language->findData(uiLanguage()));
    language->setMinimumWidth(220);
    // Create the popup before applying the shared font to all child widgets.
    language->view();
    language->setStyleSheet("QComboBox {background:#29323e;border:1px solid #465468;border-radius:7px;padding:9px 12px;} QComboBox QAbstractItemView {background:#29323e;color:#edf1f6;selection-background-color:#29456b;}");
    generalColumn->addWidget(languageLabel); generalColumn->addWidget(language, 0, Qt::AlignLeft);
    auto* showFrameRate = new UpdateCheckBox(general);
    showFrameRate->setObjectName("KemuShowFrameRate");
    showFrameRate->setChecked(QSettings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu")
        .value("general/showFrameRate", false).toBool());
    generalColumn->addWidget(showFrameRate);
    generalColumn->addStretch();
    connect(showFrameRate, &QCheckBox::toggled, this, [this](bool on) {
        QSettings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu")
            .setValue("general/showFrameRate", on);
        if (showFrameRateChanged) showFrameRateChanged(on);
    });
    auto* page = new QWidget(pages); pages->addWidget(page);
    connect(tabs, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex); tabs->setCurrentRow(0);
    auto* column = new QVBoxLayout(page); column->setSpacing(14);
    mVersion = new QLabel(QCoreApplication::translate("Kemu", "Current version: Loading..."), page); column->addWidget(mVersion);
    mStatus = new QLabel(QCoreApplication::translate("Kemu", "Check for updates to get the latest version."), page); mStatus->setObjectName("KemuUpdateStatus"); mStatus->setWordWrap(true); mStatus->setTextFormat(Qt::PlainText); column->addWidget(mStatus);
    mAutoUpdate = new UpdateCheckBox(QCoreApplication::translate("Kemu", "Automatic updates"), page); mAutoUpdate->setObjectName("KemuAutoUpdate");
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
    bool enabled = settings.value("updates/automatic", settings.value("updates/autoUpdate", true).toBool() || settings.value("updates/autoDownload", false).toBool()).toBool();
    mAutoUpdate->setChecked(enabled); settings.setValue("updates/automatic", enabled);
    settings.remove("updates/autoDownload"); settings.remove("updates/autoUpdate");
    column->addWidget(mAutoUpdate);
    connect(mAutoUpdate, &QCheckBox::toggled, this, [](bool on) { QSettings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu").setValue("updates/automatic",on); });
    auto* buttons = new QHBoxLayout;
    mCheck = new QPushButton(QCoreApplication::translate("Kemu", "Check for updates"), page); mCheck->setObjectName("KemuCheckUpdate");
    buttons->addWidget(mCheck); buttons->addStretch(); column->addLayout(buttons);
    mProgress = new QProgressBar(page); mProgress->setObjectName("KemuDownloadProgress"); mProgress->hide(); column->addWidget(mProgress);
    mCancelButton = new QPushButton(QCoreApplication::translate("Kemu", "Cancel"), page); mCancelButton->setObjectName("KemuCancelUpdate"); mCancelButton->hide(); column->addWidget(mCancelButton, 0, Qt::AlignLeft); column->addStretch();
    connect(mCheck, &QPushButton::clicked, this, [this] { check(); });
    connect(mCancelButton, &QPushButton::clicked, this, [this] { mCancel = true; mCancelButton->setEnabled(false); status(QCoreApplication::translate("Kemu", "Cancelling download...")); });
    auto* performance = new PerformancePage(pages); pages->insertWidget(1,performance);
    mPerformance = performance;
    performance->expandStorage = [this] { expandStorage(); };
    performance->moveStorage = [this] { moveStorage(); };
    auto* storageTimer = new QTimer(this); storageTimer->setInterval(2000);
    connect(storageTimer,&QTimer::timeout,this,[this] { if (mDialog->isVisible()) refreshStorage(); });
    storageTimer->start();
    tabs->insertItem(1,QCoreApplication::translate("Kemu", "Performance"));
    connect(mDialog, &QDialog::finished, this, [this, performance] {
        if (!mClosing) performance->closed(mDialog, mWindow, qpath(mRoot), !mBusy);
    });
    auto retranslate = [this, tabs, languageLabel, showFrameRate, performance] {
        tabs->item(1)->setText(QCoreApplication::translate("Kemu", "Performance"));
        performance->retranslate(); refreshStorage();
        mDialog->setWindowTitle(QCoreApplication::translate("Kemu", "Settings"));
        tabs->item(0)->setText(QCoreApplication::translate("Kemu", "General"));
        tabs->item(2)->setText(QCoreApplication::translate("Kemu", "Updates"));
        languageLabel->setText(QCoreApplication::translate("Kemu", "Language"));
        showFrameRate->setText(QCoreApplication::translate("Kemu", "Show frame rate"));
        mAutoUpdate->setText(QCoreApplication::translate("Kemu", "Automatic updates"));
        mCheck->setText(QCoreApplication::translate("Kemu", "Check for updates"));
        mCancelButton->setText(QCoreApplication::translate("Kemu", "Cancel"));
        try { mVersion->setText(QCoreApplication::translate("Kemu", "Current version: ") + QString::fromStdString(readVersion(mRoot))); }
        catch (...) { mVersion->setText(QCoreApplication::translate("Kemu", "Current version: Unknown")); }
    };
    retranslate();
    connect(language, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, language, retranslate](int) {
        setUiLanguage(language->currentData().toString());
        retranslate();
        status(mDownloaded
            ? QCoreApplication::translate("Kemu", "Update ready. Click Check for updates to continue installation.")
            : QCoreApplication::translate("Kemu", "Check for updates to get the latest version."));
        if (languageChanged) languageChanged();
    });
    applyUiFont(mDialog);
    refresh();
    QTimer::singleShot(2000, this, [this] { startup(); });
}
UpdateController::~UpdateController() { stop(); }
bool UpdateController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == mWindow && event->type() == QEvent::Close && mGrowingStorage) {
        static_cast<QCloseEvent*>(event)->ignore();
        UiMessageBox prompt(QMessageBox::Information,QCoreApplication::translate("Kemu","Storage"),
            QCoreApplication::translate("Kemu","Disk expansion is in progress. Please wait before closing Kemu."),
            QMessageBox::Ok,mDialog);
        prompt.exec();
        return true;
    }
    return QObject::eventFilter(watched,event);
}
void UpdateController::stop() { mClosing = true; mCancel = true; if (mWorker) { mWorker->wait(); delete mWorker; mWorker = nullptr; } }
void UpdateController::showSettings() { mDialog->show(); mDialog->raise(); mDialog->activateWindow(); }
void UpdateController::status(const QString& text) {
    if (QThread::currentThread() != thread()) { QMetaObject::invokeMethod(this, [this, text] { if (!mClosing) mStatus->setText(text); }, Qt::QueuedConnection); }
    else mStatus->setText(text);
}
void UpdateController::refresh() {
    std::error_code error;
    const bool pending = fs::exists(storage::requestPath(mAvd),error);
    mCheck->setEnabled(!mBusy && !pending && !error);
    mDialog->findChild<QComboBox*>("KemuLanguage")->setEnabled(!mBusy);
    if (!mBusy) { mProgress->hide(); mCancelButton->hide(); }
    refreshStorage();
}
void UpdateController::refreshStorage() {
    QString path;
    try {
        path = qpath(storage::imagePath(mAvd));
        const auto used = storage::usage(mAvd);
        QString text = QString("%1G/%2G")
            .arg(double(used.allocated) / storage::GiB,0,'f',1)
            .arg(double(used.capacity) / storage::GiB,0,'f',used.capacity % storage::GiB ? 1 : 0);
        const auto pending = storage::readRequest(mAvd);
        if (pending.target)
            text += QCoreApplication::translate("Kemu"," (expanding to %1G)").arg(pending.target / storage::GiB);
        const bool available = !mBusy && !mClosing && !fs::exists(storage::moveRequestPath(mAvd));
        mPerformance->setStorage(text,available &&
            (pending.target || used.capacity / storage::GiB < storage::MaxGiB),path,available && !pending.target);
    } catch (...) {
        mPerformance->setStorage(QCoreApplication::translate("Kemu","Storage unavailable"),false,path,false);
    }
}
void UpdateController::moveStorage() {
    if (mBusy || mClosing) return;
    try {
        const auto current = storage::imagePath(mAvd);
        const auto directory = QFileDialog::getExistingDirectory(mDialog,
            QCoreApplication::translate("Kemu","Choose a folder for the virtual disk"),qpath(current.parent_path()));
        if (directory.isEmpty()) return;
        const auto destination = storage::moveDestination(mAvd,fs::path(directory.toStdWString()));
        UiMessageBox prompt(QMessageBox::Question,QCoreApplication::translate("Kemu","Move disk"),
            QCoreApplication::translate("Kemu","Kemu will close, move the virtual disk to the folder below, and restart. This may take several minutes. Keep both drives connected until it finishes.") +
            "\n\n" + QDir::toNativeSeparators(qpath(destination)),QMessageBox::NoButton,mDialog);
        prompt.setObjectName("KemuStorageMove");
        auto* confirm = prompt.addButton(QCoreApplication::translate("Kemu","Move and restart"),QMessageBox::AcceptRole);
        auto* cancel = prompt.addButton(QCoreApplication::translate("Kemu","Cancel"),QMessageBox::RejectRole);
        prompt.setDefaultButton(cancel); prompt.setEscapeButton(cancel); prompt.exec();
        if (prompt.clickedButton() != confirm || !mPerformance->save(mDialog)) return;
        storage::requestMove(mAvd,destination.parent_path());
        if (!startDetachedProcess(qpath(mRoot / "Kemu.exe"),
                {"--restart-pid",QString::number(GetCurrentProcessId())},qpath(mRoot))) {
            fs::remove(storage::moveRequestPath(mAvd));
            throw std::runtime_error("Cannot start the restart helper.");
        }
        mClosing = true; mWindow->close();
    } catch (const std::exception& error) {
        UiMessageBox prompt(QMessageBox::Warning,QCoreApplication::translate("Kemu","Move disk"),
            QCoreApplication::translate("Kemu","Could not move the disk.") + "\n\n" +
            QString::fromUtf8(error.what()),QMessageBox::Ok,mDialog);
        prompt.exec(); refreshStorage();
    }
}
void UpdateController::storageFailure(const QString& error) {
    mGrowingStorage = false;
    mDialog->findChild<QListWidget*>("KemuSettingsTabs")->setCurrentRow(1);
    showSettings();
    UiMessageBox prompt(QMessageBox::Warning,QCoreApplication::translate("Kemu","Storage"),
        QCoreApplication::translate("Kemu","Could not complete disk expansion. Your data has not been reset. Restart Kemu to retry.") +
        "\n\n" + error,QMessageBox::Ok,mDialog);
    prompt.exec();
}
void UpdateController::expandStorage() {
    if (mBusy || mClosing) return;
    try {
        const auto used = storage::usage(mAvd);
        const auto pending = storage::readRequest(mAvd);
        QDialog prompt(mDialog); prompt.setObjectName("KemuStorageExpand");
        prompt.setWindowTitle(QCoreApplication::translate("Kemu","Expand disk"));
        auto* layout = new QVBoxLayout(&prompt);
        auto* description = new QLabel(QCoreApplication::translate("Kemu",
            "Kemu will restart to expand the disk. Existing data will be kept. The disk cannot be made smaller."),&prompt);
        description->setWordWrap(true); layout->addWidget(description);
        auto* capacity = new QSpinBox(&prompt); capacity->setObjectName("KemuStorageCapacity");
        capacity->setSuffix(" G"); capacity->setKeyboardTracking(false);
        capacity->setStyleSheet("QSpinBox {background:#29323e;color:#edf1f6;border:1px solid #465468;border-radius:6px;padding:7px;}");
        const int minimum = int(used.capacity / storage::GiB) + 1;
        capacity->setRange(pending.target ? int(pending.target / storage::GiB) : minimum,int(storage::MaxGiB));
        capacity->setValue(pending.target ? int(pending.target / storage::GiB) :
            (std::min)(int(storage::MaxGiB),(std::max)(minimum,int(used.capacity / storage::GiB) * 2)));
        capacity->setEnabled(!pending.target); layout->addWidget(capacity);
        auto* buttons = new QHBoxLayout;
        auto* cancel = new QPushButton(QCoreApplication::translate("Kemu","Cancel"),&prompt);
        auto* confirm = new QPushButton(QCoreApplication::translate("Kemu","Expand and restart"),&prompt);
        buttons->addStretch(); buttons->addWidget(cancel); buttons->addWidget(confirm);
        confirm->setObjectName("KemuStorageConfirm"); cancel->setDefault(true); confirm->setAutoDefault(false);
        connect(confirm,&QPushButton::clicked,&prompt,&QDialog::accept);
        connect(cancel,&QPushButton::clicked,&prompt,&QDialog::reject);
        layout->addLayout(buttons); prompt.resize(440,prompt.sizeHint().height()); applyUiFont(&prompt);
        if (prompt.exec() != QDialog::Accepted || !mPerformance->save(mDialog)) return;
        const auto target = uint64_t(capacity->value());
        const auto adb = mAdb(), serial = mSerial();
        work([this,adb,serial] {
            adbCommand(adb,serial,{"shell",rootScript(
                "set -eu\n"
                "test -x /system/bin/resize2fs\n"
                "test \"$(id -u)\" = 0\n"
                "awk '$2 == \"/data\" && $3 == \"ext4\" { found=1 } END { exit !found }' /proc/mounts\n")},15000);
        },[this,target,pending] {
            bool created = false;
            try {
                if (!pending.target) { storage::requestGrowth(mAvd,target); created = true; }
                if (!startDetachedProcess(qpath(mRoot / "Kemu.exe"),
                        {"--restart-pid",QString::number(GetCurrentProcessId())},qpath(mRoot))) {
                    if (created) fs::remove(storage::requestPath(mAvd));
                    throw std::runtime_error("Cannot start the restart helper.");
                }
                mClosing = true; mWindow->close();
            } catch (const std::exception& error) { storageFailure(QString::fromUtf8(error.what())); }
        },[this](const QString& error) { storageFailure(error); });
    } catch (const std::exception& error) { storageFailure(QString::fromUtf8(error.what())); }
}
void UpdateController::finishStorageGrowth() {
    try {
        const auto request = storage::readRequest(mAvd);
        if (!request.target) return;
        require(storage::usage(mAvd).capacity == request.target,
                "Restart Kemu to apply the requested data image size.");
        const auto adb = mAdb(), serial = mSerial();
        mGrowingStorage = true;
        work([this,adb,serial,request] {
            QElapsedTimer boot; boot.start();
            bool ready = false;
            while (!mCancel && boot.elapsed() < 180000) {
                try {
                    ready = adbCommand(adb,serial,{"shell","getprop","sys.boot_completed"},5000).trimmed() == "1";
                    if (ready) break;
                } catch (const std::exception&) {}
                QThread::msleep(1000);
            }
            require(ready && !mCancel,"Android did not finish starting for disk expansion.");
            const auto script = QString(
                "set -eu\n"
                "dev=$(awk '$2 == \"/data\" && $3 == \"ext4\" { print $1; exit }' /proc/mounts)\n"
                "case \"$dev\" in /dev/block/*) ;; *) exit 1 ;; esac\n"
                "test \"$(blockdev --getsize64 \"$dev\")\" = %1\n"
                "/system/bin/resize2fs \"$dev\"\n"
                "set -- $(od -An -tu4 -j1028 -N4 \"$dev\")\n"
                "test \"$#\" = 1\n"
                "test \"$1\" = %2\n"
                "sync\n").arg(request.target).arg(request.target / 4096);
            // Once resizing starts, wait for it even if the window is closing.
            adbCommand(adb,serial,{"shell",rootScript(script)},10*60*1000,false);
            storage::completeGrowth(mAvd,request);
        },[this] {
            mGrowingStorage = false;
            refreshStorage();
            QTimer::singleShot(0,this,[this] { startup(); });
        },[this](const QString& error) { storageFailure(error); });
    } catch (const std::exception& error) { storageFailure(QString::fromUtf8(error.what())); }
}
void UpdateController::work(std::function<void()> task, std::function<void()> finished,
                            std::function<void(const QString&)> failed) {
    if (mBusy || mClosing) return;
    mBusy = true; mCancel = false; refresh(); auto error = std::make_shared<QString>();
    mWorker = QThread::create([task, error] { try { task(); } catch (const std::exception& e) { *error = QString::fromUtf8(e.what()); } });
    connect(mWorker, &QThread::finished, this, [this, error, finished, failed] {
        if (mClosing) return; mWorker->deleteLater(); mWorker = nullptr; mBusy = false;
        if (!error->isEmpty() || mCancel) {
            if (failed) { failed(*error); refresh(); return; }
            mDownloaded = false; mPlan = {};
            try { if (!mStage.empty() && fs::is_directory(mStage)) writeText(mStage / "error.txt", error->toStdString()); } catch (...) {}
            status(mCancel ? QCoreApplication::translate("Kemu", "Download cancelled. You can check for updates again.") : QCoreApplication::translate("Kemu", "Update could not be completed. Please try again later.\n") + *error); mDialog->findChild<QListWidget*>("KemuSettingsTabs")->setCurrentRow(2); showSettings();
        } else finished();
        refresh();
    });
    mWorker->start();
}
void UpdateController::startup() {
    if (mBusy || mDownloaded) return;
    if (fs::exists(storage::requestPath(mAvd))) { finishStorageGrowth(); return; }
    try { mVersion->setText(QCoreApplication::translate("Kemu", "Current version: ") + QString::fromStdString(readVersion(mRoot))); }
    catch (...) { mVersion->setText(QCoreApplication::translate("Kemu", "Current version: Unknown")); status(QString()); return; }
    if (mAutoUpdate->isChecked() || fs::exists(mRoot / ".kemu-update")) check(true);
}
void UpdateController::check(bool automatic) {
    if (mBusy || fs::exists(storage::requestPath(mAvd))) return;
    try { readVersion(mRoot); } catch (...) {
        mVersion->setText(QCoreApplication::translate("Kemu", "Current version: Unknown"));
        status(QString());
        if (!automatic) {
            UiMessageBox prompt(QMessageBox::Information, QCoreApplication::translate("Kemu", "Cannot detect current version"),
                QCoreApplication::translate("Kemu", "Cannot detect the current version. Click OK to download the full installer."), QMessageBox::NoButton, mDialog);
            auto* confirm = prompt.addButton(QCoreApplication::translate("Kemu", "OK"), QMessageBox::AcceptRole);
            prompt.addButton(QCoreApplication::translate("Kemu", "Cancel"), QMessageBox::RejectRole);
            prompt.exec();
            if (prompt.clickedButton() == confirm) QDesktopServices::openUrl(QUrl(QString::fromUtf8(ReleasesUrl)));
        }
        return;
    }
    work([this] {
        status(QCoreApplication::translate("Kemu", "Checking for updates...")); mDownloaded = false; mPlan = {};
        readVersion(mRoot);
        require(!fs::exists(mRoot / ".kemu-update-pending"), "An unfinished update needs recovery; restart Kemu");
        auto latest = mRoot / ".kemu-update-history.tmp";
        if (mHistory) writeJson(latest, mHistory());
        else download(HistoryUrl, latest, 8*1024*1024, [this] { return mCancel.load(); });
        auto history = readJson(latest); fs::remove(latest);
        mPlan = prepareUpdateTask(mRoot, history);
        if (mPlan.requiresFullInstaller) return;
        mStage = mRoot / ".kemu-update";
        mDownloaded = loadDownloadedUpdate(mRoot, mStage, mPlan);
    }, [this, automatic] {
        mVersion->setText(QCoreApplication::translate("Kemu", "Current version: ") + QString::fromStdString(mPlan.current));
        if (mPlan.requiresFullInstaller) {
            const auto message = QCoreApplication::translate("Kemu", "A major update is available. Please download the full installer.");
            status(message);
            if (!automatic) {
                UiMessageBox prompt(QMessageBox::Information, QCoreApplication::translate("Kemu", "Major update available"),
                    message, QMessageBox::NoButton, mDialog);
                prompt.setObjectName("KemuFullInstallerRequired");
                auto* download = prompt.addButton(QCoreApplication::translate("Kemu", "Download full installer"), QMessageBox::AcceptRole);
                prompt.addButton(QCoreApplication::translate("Kemu", "Cancel"), QMessageBox::RejectRole);
                prompt.exec();
                if (prompt.clickedButton() == download) QDesktopServices::openUrl(QUrl(QString::fromUtf8(ReleasesUrl)));
            }
            return;
        }
        if (mDownloaded) {
            status(QCoreApplication::translate("Kemu", "Update ready. Click Check for updates to continue installation."));
            if (!automatic || mAutoUpdate->isChecked()) offerInstall();
            return;
        }
        if (mPlan.current == mPlan.target) status(QCoreApplication::translate("Kemu", "You are up to date."));
        else { status(QCoreApplication::translate("Kemu", "Version %1 is available; %2 files need updating. ").arg(QString::fromStdString(mPlan.target)).arg(mPlan.files.size())); if (automatic && !mAutoUpdate->isChecked()) return; if (automatic || [this] {
            UiMessageBox prompt(QMessageBox::Question, QCoreApplication::translate("Kemu", "New version available"),
                QCoreApplication::translate("Kemu", "Version %1 is available. Download the update?").arg(QString::fromStdString(mPlan.target)), QMessageBox::NoButton, mDialog);
            auto* yes = prompt.addButton(QCoreApplication::translate("Kemu", "Yes"), QMessageBox::YesRole);
            prompt.addButton(QCoreApplication::translate("Kemu", "No"), QMessageBox::NoRole);
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
            status(QCoreApplication::translate("Kemu", "Downloading and verifying update %1...").arg(QString::fromStdString(version)));
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
        status(QCoreApplication::translate("Kemu", "Update downloaded. You will be prompted on next startup, or click Check for updates to install now."));
        if (!automatic) offerInstall();
    });
}
void UpdateController::offerInstall() {
    UiMessageBox prompt(QMessageBox::Information, QCoreApplication::translate("Kemu", "Update ready"),
        QCoreApplication::translate("Kemu", "The update has been downloaded and verified. Updating now will close and restart the emulator."), QMessageBox::NoButton, mDialog);
    auto* now = prompt.addButton(QCoreApplication::translate("Kemu", "Update now"), QMessageBox::AcceptRole);
    prompt.addButton(QCoreApplication::translate("Kemu", "Later"), QMessageBox::RejectRole);
    prompt.exec(); if (prompt.clickedButton() == now) install();
}
QByteArray UpdateController::adbCommand(const QString& adb, const QString& serial, const QStringList& args, int timeout, bool cancellable) {
    require(!adb.isEmpty() && serial.startsWith("emulator-"), "ADB for this emulator is unavailable");
    QProcess process; process.setProcessChannelMode(QProcess::MergedChannels); process.start(adb, QStringList{"-s", serial} + args);
    require(process.waitForStarted(10000), "Cannot start ADB"); QElapsedTimer timer; timer.start(); QByteArray output;
    while (!process.waitForFinished(100)) {
        output += process.readAll(); if (output.size() > 4*1024*1024) output = output.right(4*1024*1024);
        if ((cancellable && mCancel) || timer.elapsed() > timeout) { process.kill(); process.waitForFinished(5000); throw std::runtime_error("ADB operation cancelled or timed out"); }
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
        status(QCoreApplication::translate("Kemu", "Updating the standalone updater...")); safePath(mRoot, "KemuUpdater.exe");
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
        status(QCoreApplication::translate("Kemu", "Creating a new system image inside the emulator. Keep the emulator running..."));
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
        status(QCoreApplication::translate("Kemu", "Retrieving and verifying the system image..."));
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
            require(startDetachedProcess(qpath(mRoot / "KemuUpdater.exe"), {"--apply",qpath(mStage),QString::number(GetCurrentProcessId())}, qpath(mRoot)), "Cannot start updater");
            mBusy = true; status(QCoreApplication::translate("Kemu", "Images and files verified. Handing over to the updater..."));
            auto* timer = new QTimer(this); auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
            connect(timer, &QTimer::timeout, this, [this, timer, elapsed] {
                if (fs::exists(mStage / "updater-ready")) { timer->stop(); timer->deleteLater(); mWindow->close(); }
                else if (elapsed->elapsed() > 120000) { timer->stop(); timer->deleteLater(); status(QCoreApplication::translate("Kemu", "The updater is not ready. The emulator remains open. Restart Kemu to recover.")); mBusy = false; refresh(); }
            }); timer->start(200);
        } catch (const std::exception& e) { status(QString::fromUtf8(e.what())); }
    });
}
}

