#include "UiLanguage.h"
#include <QTranslator>

static void initTranslationResources() { Q_INIT_RESOURCE(kemu_translations); }

namespace kemu {
namespace {
class UiTranslations final : public QObject {
public:
    explicit UiTranslations(QObject* parent) : QObject(parent), chinese(this) {
        initTranslationResources();
        if (!chinese.load(QStringLiteral(":/kemu/translations/kemu_zh_CN.qm")))
            qFatal("Cannot load embedded Kemu translations");
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
        select(initializeUiLanguage(settings, systemUiLocale()));
    }

    void select(const QString& value) {
        if (language == value) return;
        if (language == "zh-CN") QCoreApplication::removeTranslator(&chinese);
        language = value;
        if (language == "zh-CN") QCoreApplication::installTranslator(&chinese);
    }

    QTranslator chinese;
    QString language;
};

UiTranslations& translations() {
    // Owned by QApplication so the translator is removed before Qt shuts down.
    static auto* instance = new UiTranslations(QCoreApplication::instance());
    return *instance;
}
} // namespace

void initializeUiTranslations() { translations(); }
QString uiLanguage() { return translations().language; }
void setUiLanguage(const QString& language) {
    if (language != "zh-CN" && language != "en") return;
    translations().select(language);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Kemu", "Kemu");
    settings.setValue("general/language", language);
    settings.sync();
}
QString nativeUiLanguageName(const QString& language) {
    if (language == "zh-CN") {
        // Keep language choices readable regardless of the selected UI language.
        const char* source = QT_TRANSLATE_NOOP("LanguageNames", "Simplified Chinese");
        return translations().chinese.translate("LanguageNames", source);
    }
    return QStringLiteral("English");
}
} // namespace kemu
