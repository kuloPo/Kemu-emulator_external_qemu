#pragma once
#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QToolTip>

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
// Call once after creating QApplication, before constructing any widgets.
inline void initializeUiFonts() {
    const auto font = uiFont();
    QApplication::setFont(font);
    QToolTip::setFont(font);
}
}
