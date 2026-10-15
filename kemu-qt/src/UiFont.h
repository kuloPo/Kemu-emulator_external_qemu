#pragma once
#include <QFont>
#include <QFontDatabase>
#include <QMessageBox>
#include <QWidget>

namespace kemu {
inline QFont uiFont(int points = 10, QFont::Weight weight = QFont::Normal) {
#ifdef _WIN32
    static const int id = QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    Q_UNUSED(id);
#endif
    QFont font(QStringLiteral("Microsoft YaHei"), points, weight);
    font.setStyleStrategy(QFont::StyleStrategy(QFont::ForceOutline | QFont::PreferAntialias));
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}
// Stylesheet polish can reset font strategy; apply to each actual text widget.
inline void applyUiFont(QWidget* root, const QFont& font = uiFont()) {
    root->ensurePolished(); root->setFont(font);
    for (auto* child : root->findChildren<QWidget*>()) {
        child->ensurePolished(); child->setFont(font);
    }
}
// Message boxes create text/buttons lazily, after their parent was styled.
class UiMessageBox : public QMessageBox {
public:
    using QMessageBox::QMessageBox;
    int exec() override { applyUiFont(this); return QMessageBox::exec(); }
    void open() override { applyUiFont(this); QMessageBox::open(); }
protected:
    void showEvent(QShowEvent* event) override {
        QMessageBox::showEvent(event);
        applyUiFont(this);
    }
};
}
