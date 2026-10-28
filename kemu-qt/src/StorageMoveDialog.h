#pragma once
#include <QDialog>
#include <filesystem>

class QLabel;
class QProgressBar;

namespace kemu {
class StorageMoveDialog final : public QDialog {
public:
    explicit StorageMoveDialog(const QString& destination);
    void setProgress(int percent);
    int move(const std::filesystem::path& avd);
protected:
    void reject() override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    QWidget* mTitle;
    QLabel* mStatus;
    QProgressBar* mProgress;
    bool mRunning = true;
};
int runStorageMove(int argc, char** argv);
}
