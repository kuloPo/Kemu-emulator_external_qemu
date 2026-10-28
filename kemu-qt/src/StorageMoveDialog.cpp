#include "StorageMoveDialog.h"
#include "UiFont.h"
#include "UiLanguage.h"
#include "../launcher/StorageLocation.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QDir>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QScreen>
#include <QWindow>
#include <thread>

static void initMoveResources() { Q_INIT_RESOURCE(kemu); }

namespace kemu {
StorageMoveDialog::StorageMoveDialog(const QString& destination) {
    initializeUiTranslations();
    initMoveResources();
    setObjectName("KemuStorageMoveProgress");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowTitle(QCoreApplication::translate("Kemu","Move disk"));
    setWindowIcon(QIcon(":/kemu/kemu.ico"));
    setStyleSheet(
        "QDialog {background:#1b2028;border:1px solid #293343;}"
        "QWidget {color:#edf1f6;}"
        "#KemuMoveTitle {background:#15191f;}"
        "#KemuMoveHint, #KemuMoveDestination {color:#9caabd;}"
        "QProgressBar {border:0;border-radius:5px;background:#101720;text-align:center;min-height:20px;}"
        "QProgressBar::chunk {background:#438fe9;border-radius:5px;}"
        "QPushButton {padding:9px 18px;border:1px solid #465468;border-radius:7px;background:#29323e;}"
        "QPushButton:hover {background:#3b506c;}");
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(1,1,1,1); column->setSpacing(0);
    mTitle = new QWidget(this); mTitle->setObjectName("KemuMoveTitle");
    mTitle->setFixedHeight(42); mTitle->installEventFilter(this);
    auto* title = new QHBoxLayout(mTitle); title->setContentsMargins(16,0,16,0); title->setSpacing(10);
    auto* logo = new QLabel(mTitle); logo->setPixmap(windowIcon().pixmap(QSize(20,20)));
    logo->setFixedSize(20,20); logo->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* brand = new QLabel("Kemu",mTitle); brand->setAttribute(Qt::WA_TransparentForMouseEvents);
    title->addWidget(logo); title->addWidget(brand); title->addStretch(); column->addWidget(mTitle);
    auto* content = new QWidget(this); auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24,22,24,24); body->setSpacing(14);
    auto* heading = new QLabel(QCoreApplication::translate("Kemu","Moving virtual disk"),content);
    body->addWidget(heading);
    auto* path = new QLabel(content); path->setObjectName("KemuMoveDestination");
    path->setTextFormat(Qt::PlainText);
    path->setToolTip(QDir::toNativeSeparators(destination));
    path->setAccessibleName(path->toolTip());
    body->addWidget(path);
    mStatus = new QLabel(content); mStatus->setObjectName("KemuMoveStatus"); body->addWidget(mStatus);
    mProgress = new QProgressBar(content); mProgress->setObjectName("KemuMoveProgress");
    mProgress->setRange(0,100); mProgress->setFixedHeight(22); body->addWidget(mProgress);
    auto* hint = new QLabel(QCoreApplication::translate("Kemu",
        "Keep both drives connected. Kemu will restart automatically when the move finishes."),content);
    hint->setObjectName("KemuMoveHint"); hint->setWordWrap(true); body->addWidget(hint);
    column->addWidget(content);
    applyUiFont(this); heading->setFont(uiFont(14,QFont::DemiBold));
    setFixedWidth(480);
    path->setText(path->fontMetrics().elidedText(path->toolTip(),Qt::ElideMiddle,430));
    setProgress(0); adjustSize();
    if (const auto* display = screen())
        QWidget::move(display->availableGeometry().center() - rect().center());
}
void StorageMoveDialog::setProgress(int percent) {
    // Reserve completion until the copied disk and its location are committed.
    mProgress->setValue((std::min)(percent,99));
    const char* text = percent < 50 ? QT_TRANSLATE_NOOP("Kemu","Copying disk...") :
        percent < 100 ? QT_TRANSLATE_NOOP("Kemu","Verifying disk...") : QT_TRANSLATE_NOOP("Kemu","Finishing the move...");
    mStatus->setText(QCoreApplication::translate("Kemu",text));
}
void StorageMoveDialog::reject() { if (!mRunning) QDialog::reject(); }
void StorageMoveDialog::closeEvent(QCloseEvent* event) {
    if (mRunning) event->ignore(); else QDialog::closeEvent(event);
}
bool StorageMoveDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == mTitle && event->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && windowHandle()) {
        windowHandle()->startSystemMove(); return true;
    }
    return QDialog::eventFilter(watched,event);
}
int StorageMoveDialog::move(const std::filesystem::path& avd) {
    QString error;
    std::thread worker([this,&avd,&error] {
        try {
            storage::applyMove(avd,[this](int percent) {
                QMetaObject::invokeMethod(this,[this,percent] { setProgress(percent); },Qt::QueuedConnection);
            });
        } catch (const std::exception& failure) { error = QString::fromUtf8(failure.what()); }
        QMetaObject::invokeMethod(this,[this] { mProgress->setValue(100); mRunning=false; accept(); },Qt::QueuedConnection);
    });
    exec(); worker.join();
    if (error.isEmpty()) return 0;
    UiMessageBox prompt(QMessageBox::Warning,QCoreApplication::translate("Kemu","Move disk"),
        QCoreApplication::translate("Kemu","Could not finish moving the disk. Kemu will reopen using the saved disk location.") +
        "\n\n" + error,QMessageBox::Ok,this);
    prompt.exec(); return 1;
}
int runStorageMove(int argc, char** argv) {
    const auto avd = std::filesystem::u8path(argv[2]);
    QApplication app(argc,argv);
    try {
        DWORD flags = 0;
        const auto lease = reinterpret_cast<HANDLE>(qEnvironmentVariable("KEMU_INSTANCE_HANDLE").toULongLong());
        storage::require(lease && GetHandleInformation(lease,&flags), "Disk moves must be started by Kemu.exe.");
        const auto destination = storage::readPath(storage::moveRequestPath(avd));
        StorageMoveDialog dialog(QString::fromStdWString(destination.wstring()));
        return dialog.move(avd);
    } catch (const std::exception&) { return 2; }
}
}
