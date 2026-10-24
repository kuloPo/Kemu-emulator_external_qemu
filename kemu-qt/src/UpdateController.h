#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <atomic>
#include <functional>
#include <memory>
#include "../update/UpdateCore.h"
class QProgressBar; class QCheckBox; class QDialog; class QLabel; class QPushButton; class QThread; class QWidget;
namespace kemu {
class PerformancePage;
class UpdateController final : public QObject {
public:
    UpdateController(QWidget* parent, std::function<QString()> adb, std::function<QString()> serial, std::function<update::Json()> history = {});
    ~UpdateController() override;
    void showSettings();
    void stop();
    std::function<void()> languageChanged;
    std::function<void(bool)> showFrameRateChanged;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void check(bool automatic = false);
    void offerInstall();
    void fetch(bool automatic);
    void startup();
    void install();
    void status(const QString& text);
    void refresh();
    void work(std::function<void()> task, std::function<void()> finished,
              std::function<void(const QString&)> failed = {});
    void refreshStorage();
    void expandStorage();
    void finishStorageGrowth();
    void storageFailure(const QString& error);
    void prepare(const QString& adb, const QString& serial);
    QByteArray adbCommand(const QString& adb, const QString& serial, const QStringList& args,
                          int timeout = 120000, bool cancellable = true);
    QWidget* mWindow;
    QDialog* mDialog;
    QLabel* mStatus;
    QLabel* mVersion;
    QPushButton* mCheck;
    QPushButton* mCancelButton;
    QProgressBar* mProgress;
    QCheckBox* mAutoUpdate;
    PerformancePage* mPerformance;
    std::function<QString()> mAdb, mSerial;
    std::function<update::Json()> mHistory;
    update::fs::path mRoot, mStage, mAvd;
    update::Plan mPlan;
    QThread* mWorker = nullptr;
    std::atomic<bool> mCancel{false};
    bool mBusy = false, mDownloaded = false, mClosing = false;
    bool mGrowingStorage = false;
};
}
