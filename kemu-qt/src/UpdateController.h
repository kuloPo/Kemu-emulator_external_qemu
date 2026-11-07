#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <functional>
#include <memory>
#include "../update/UpdateCore.h"
class QProgressBar; class QCheckBox; class QDialog; class QLabel; class QPushButton; class QThread; class QWidget;
class QListWidget; class QListWidgetItem; class QStackedWidget;
namespace kemu {
class PerformancePage;
class UpdateController final : public QObject {
public:
    UpdateController(QWidget* parent, std::function<QString()> adb, std::function<QString()> serial, std::function<update::Json()> history = {}, update::Download transfer = {});
    ~UpdateController() override;
    void showSettings();
    void stop();
    std::function<void()> languageChanged;
    std::function<void(bool)> showFrameRateChanged;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void showUpdatesPage();
    void check(bool automatic = false);
    void offerInstall();
    void offerDownload();
    void fetch();
    void startup();
    void install();
    void handoff(bool fullInstaller);
    void status(const QString& text);
    void setVersion(const QString& version);
    bool refreshVersion();
    void refresh();
    void work(std::function<void()> task, std::function<void()> finished,
              std::function<void(const QString& error, bool network)> failed);
    void updateFailed(const QString& error, bool quiet);
    void refreshStorage();
    void expandStorage();
    void moveStorage();
    void finishStorageGrowth();
    void storageFailure(const QString& error);
    void prepare(const QString& adb, const QString& serial);
    QByteArray adbCommand(const QString& adb, const QString& serial, const QStringList& args,
                          int timeout = 120000, bool cancellable = true);
    QWidget* mWindow;
    QDialog* mDialog;
    QListWidget* mSettingsTabs;
    QListWidgetItem* mUpdatesTab;
    QStackedWidget* mSettingsPages;
    QWidget* mUpdatesPage;
    QPointer<QDialog> mInstallPrompt;
    QLabel* mStatus;
    QLabel* mVersion;
    QLabel* mAboutVersion;
    QPushButton* mCheck;
    QPushButton* mCancelButton;
    QProgressBar* mProgress;
    QCheckBox* mAutoUpdate;
    PerformancePage* mPerformance;
    std::function<QString()> mAdb, mSerial;
    std::function<update::Json()> mHistory;
    update::Download mTransfer;
    update::fs::path mRoot, mStage, mAvd;
    update::Plan mPlan;
    QThread* mWorker = nullptr;
    update::Cancellation mCancel;
    bool mBusy = false, mDownloaded = false, mClosing = false;
    bool mGrowingStorage = false;
};
}
