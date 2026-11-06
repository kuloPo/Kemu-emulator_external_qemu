#pragma once
#include <QDialog>
#include <QString>

class QListWidget;
class QTextBrowser;

namespace kemu {
class LegalInfoDialog final : public QDialog {
public:
    LegalInfoDialog(QWidget* parent, const QString& installRoot, int section);
    void selectSection(int section);
private:
    void loadSection(int section);
    QString mRoot;
    QListWidget* mSections;
    QTextBrowser* mText;
};
void showLegalInformation(QWidget* parent, const QString& installRoot, int section);
}
