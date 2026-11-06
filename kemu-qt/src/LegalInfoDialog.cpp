#include "LegalInfoDialog.h"
#include "UiFont.h"
#include <QBoxLayout>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>

namespace kemu {
namespace {
class LegalTextBrowser final : public QTextBrowser {
public:
    using QTextBrowser::QTextBrowser;
protected:
    QVariant loadResource(int type, const QUrl& name) override {
        const auto data = QTextBrowser::loadResource(type, name);
        const auto suffix = QFileInfo(name.path()).suffix();
        if (type != QTextDocument::HtmlResource || !name.isLocalFile() || !data.isValid() ||
            suffix.compare("html", Qt::CaseInsensitive) == 0 ||
            suffix.compare("htm", Qt::CaseInsensitive) == 0) return data;
        auto text = data.userType() == QMetaType::QByteArray
            ? QString::fromUtf8(data.toByteArray()) : data.toString();
        text.replace("\r\n", "\n").replace('\r', '\n');
        // Keep native navigation while protecting text from HTML parsing.
        // Qt strips one newline at each end of a preformatted block.
        return QStringLiteral("<pre>\n") + text.toHtmlEscaped() + QStringLiteral("\n</pre>");
    }
};
}
LegalInfoDialog::LegalInfoDialog(QWidget* parent, const QString& installRoot, int section)
    : QDialog(parent), mRoot(installRoot.isEmpty() ? QCoreApplication::applicationDirPath() : installRoot) {
    setObjectName("KemuLegalInformation");
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QCoreApplication::translate("Kemu", "Licenses and trademarks"));
    resize(900, 620);
    setStyleSheet("QDialog {background:#1b2028;} QWidget {color:#edf1f6;} "
        "QListWidget, QTextBrowser {background:#151b24;border:1px solid #293343;border-radius:7px;padding:10px;} "
        "QListWidget::item {padding:12px 4px;} QListWidget::item:selected {background:#29456b;} "
        "QPushButton {padding:8px 16px;background:#29323e;border:1px solid #465468;border-radius:6px;}");
    auto* column = new QVBoxLayout(this);
    auto* row = new QHBoxLayout;
    mSections = new QListWidget(this); mSections->setObjectName("KemuLegalSections");
    mSections->setMinimumWidth(185); mSections->setMaximumWidth(230);
    mSections->addItem(QCoreApplication::translate("Kemu", "Open-source licenses"));
    mSections->addItem(QCoreApplication::translate("Kemu", "Source code"));
    mSections->addItem(QCoreApplication::translate("Kemu", "Trademarks"));
    mText = new LegalTextBrowser(this); mText->setObjectName("KemuLegalText");
    mText->document()->setDefaultStyleSheet("a {color:#8ab8f5;} pre {white-space:pre-wrap;}");
    mText->setOpenLinks(false);
    connect(mText, &QTextBrowser::anchorClicked, this, [this](const QUrl& link) {
        const auto url = mText->source().resolved(link);
        if (url.scheme() == "https") {
            QDesktopServices::openUrl(url);
        } else if (url.isLocalFile()) {
            const auto file = QFileInfo(url.toLocalFile());
            const auto relative = QDir(mRoot).relativeFilePath(file.canonicalFilePath());
            if (file.isFile() && !relative.startsWith("../") && !QDir::isAbsolutePath(relative)) {
                mText->setSource(url);
            } else {
                mText->setPlainText(QCoreApplication::translate("Kemu", "This notice file is not included in this installation."));
            }
        }
    });
    row->addWidget(mSections); row->addWidget(mText, 1); column->addLayout(row, 1);
    auto* buttons = new QHBoxLayout;
    auto* back = new QPushButton(QCoreApplication::translate("Kemu", "Back"), this);
    back->setEnabled(false);
    connect(back, &QPushButton::clicked, mText, &QTextBrowser::backward);
    connect(mText, &QTextBrowser::backwardAvailable, back, &QPushButton::setEnabled);
    auto* close = new QPushButton(QCoreApplication::translate("Kemu", "Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(back); buttons->addStretch(); buttons->addWidget(close); column->addLayout(buttons);
    connect(mSections, &QListWidget::currentRowChanged, this, &LegalInfoDialog::loadSection);
    selectSection(section); applyUiFont(this);
}
void LegalInfoDialog::selectSection(int section) {
    section = qBound(0, section, 2);
    if (mSections->currentRow() == section) loadSection(section);
    else mSections->setCurrentRow(section);
}
void LegalInfoDialog::loadSection(int section) {
    static const char* paths[] = {"legal/licenses.html", "legal/sources.html", "legal/TRADEMARKS.txt"};
    const auto path = QDir(mRoot).filePath(QString::fromLatin1(paths[qBound(0, section, 2)]));
    mText->clear();
    if (QFileInfo(path).isFile()) mText->setSource(QUrl::fromLocalFile(path));
    else mText->setPlainText(QCoreApplication::translate("Kemu", "This notice file is not included in this installation."));
    mText->clearHistory();
}
void showLegalInformation(QWidget* parent, const QString& installRoot, int section) {
    auto* dialog = parent->findChild<QDialog*>("KemuLegalInformation");
    if (dialog) static_cast<LegalInfoDialog*>(dialog)->selectSection(section);
    else dialog = new LegalInfoDialog(parent, installRoot, section);
    dialog->show(); dialog->raise(); dialog->activateWindow();
}
}
