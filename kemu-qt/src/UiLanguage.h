#pragma once
#include <QLocale>
#include <QSettings>
#include <QString>
#include <QCoreApplication>
#ifdef _WIN32
#include <windows.h>
#endif

namespace kemu {
inline QString defaultUiLanguage(const QLocale& locale) {
    return locale.language() == QLocale::Chinese && locale.script() == QLocale::SimplifiedHanScript
        ? QStringLiteral("zh-CN") : QStringLiteral("en");
}
inline QLocale systemUiLocale() {
#ifdef _WIN32
    // The Windows display language can differ from regional formatting.
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name,
                         LOCALE_NAME_MAX_LENGTH, 0)) return QLocale(QString::fromWCharArray(name));
#endif
    return QLocale::system();
}
inline QString initializeUiLanguage(QSettings& settings, const QLocale& locale) {
    auto language = settings.value("general/language").toString();
    if (language != "zh-CN" && language != "en") {
        language = defaultUiLanguage(locale);
        settings.setValue("general/language", language);
        settings.sync();
    }
    return language;
}
// Call on the GUI thread before constructing translated widgets.
void initializeUiTranslations();
QString uiLanguage();
void setUiLanguage(const QString& language);
QString nativeUiLanguageName(const QString& language);
} // namespace kemu
