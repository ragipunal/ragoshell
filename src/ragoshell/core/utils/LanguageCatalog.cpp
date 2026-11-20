#include "ragoshell/core/utils/LanguageCatalog.h"

namespace RagoShell
{
    namespace
    {
        LanguageInfo lang(const char *code, const QString &endonym, const char *english,
                          const char *locale, const char *script, bool rtl, bool qtTranslated)
        {
            LanguageInfo info;
            info.code = QString::fromLatin1(code);
            info.endonym = endonym;
            info.englishName = QString::fromLatin1(english);
            info.localeName = QString::fromLatin1(locale);
            info.script = QString::fromLatin1(script);
            info.rtl = rtl;
            info.qtTranslated = qtTranslated;
            return info;
        }

        // Which of the languages Qt itself ships translations for was checked
        // against the translations folder of Qt 6.12. Arabic uses the ar_AE
        // locale because its CLDR default digits are Latin (ar and ar_SA use
        // Arabic-Indic digits, which are wrong for a database tool).
        QList<LanguageInfo> buildTable()
        {
            return {
                lang("en", QStringLiteral("English"), "English", "en", "Latn", false, true),
                lang("hi", QString::fromUtf8("\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80"),
                     "Hindi", "hi_IN", "Deva", false, false),
                lang("zh_CN", QString::fromUtf8("\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87"),
                     "Chinese (Simplified)", "zh_CN", "Hans", false, true),
                lang("es", QString::fromUtf8("Espa\xC3\xB1ol"), "Spanish", "es", "Latn", false, true),
                lang("pt_BR", QString::fromUtf8("Portugu\xC3\xAA" "s (Brasil)"),
                     "Portuguese (Brazil)", "pt_BR", "Latn", false, true),
                lang("ru", QString::fromUtf8("\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"),
                     "Russian", "ru", "Cyrl", false, true),
                lang("de", QStringLiteral("Deutsch"), "German", "de", "Latn", false, true),
                lang("fr", QString::fromUtf8("Fran\xC3\xA7" "ais"), "French", "fr", "Latn", false, true),
                lang("ja", QString::fromUtf8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"),
                     "Japanese", "ja", "Jpan", false, true),
                lang("ar", QString::fromUtf8("\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9"),
                     "Arabic", "ar_AE", "Arab", true, true),
                lang("tr", QString::fromUtf8("T\xC3\xBCrk\xC3\xA7" "e"), "Turkish", "tr", "Latn", false, true),
                lang("az", QString::fromUtf8("Az\xC9\x99rbaycan dili"),
                     "Azerbaijani", "az_Latn_AZ", "Latn", false, false),
                lang("kk", QString::fromUtf8("\xD2\x9A\xD0\xB0\xD0\xB7\xD0\xB0\xD2\x9B \xD1\x82\xD1\x96\xD0\xBB\xD1\x96"),
                     "Kazakh", "kk", "Cyrl", false, true),
                lang("ky", QString::fromUtf8("\xD0\x9A\xD1\x8B\xD1\x80\xD0\xB3\xD1\x8B\xD0\xB7\xD1\x87\xD0\xB0"),
                     "Kyrgyz", "ky", "Cyrl", false, false),
                lang("uz", QString::fromUtf8("O\xCA\xBBzbekcha"), "Uzbek", "uz_Latn_UZ", "Latn", false, false),
            };
        }

        /// Script a bare language/region tag ("zh-CN", "zh-TW") implies, for
        /// the languages whose region decides the script.
        QString impliedScript(const QString &language, const QString &region)
        {
            if (language == QLatin1String("zh")) {
                const QString r = region.toUpper();
                if (r == QLatin1String("TW") || r == QLatin1String("HK") || r == QLatin1String("MO"))
                    return QStringLiteral("Hant");
                return QStringLiteral("Hans");
            }
            return {};
        }
    }

    const QList<LanguageInfo> &LanguageCatalog::all()
    {
        static const QList<LanguageInfo> table = buildTable();
        return table;
    }

    const LanguageInfo *LanguageCatalog::find(const QString &code)
    {
        for (const LanguageInfo &info : all())
            if (info.code == code)
                return &info;
        return nullptr;
    }

    QString LanguageCatalog::matchSystemLanguages(const QStringList &uiLanguages)
    {
        for (const QString &tag : uiLanguages) {
            const QStringList parts = tag.trimmed().replace(QLatin1Char('_'), QLatin1Char('-'))
                                          .split(QLatin1Char('-'), Qt::SkipEmptyParts);
            if (parts.isEmpty())
                continue;
            const QString language = parts.at(0).toLower();
            QString script, region;
            for (int i = 1; i < parts.size(); ++i) {
                const QString &p = parts.at(i);
                if (p.size() == 4 && script.isEmpty())
                    script = p.at(0).toUpper() + p.mid(1).toLower();
                else if (p.size() == 2 || (p.size() == 3 && p.at(0).isDigit()))
                    region = p;
            }
            if (script.isEmpty())
                script = impliedScript(language, region);

            for (const LanguageInfo &info : all()) {
                if (info.code.section(QLatin1Char('_'), 0, 0) != language)
                    continue;
                // Japanese is one "Jpan" entry; its tags also use Hira/Kana/Hani.
                const bool scriptOk = script.isEmpty()
                                      || script == info.script
                                      || (info.script == QLatin1String("Jpan") && !script.isEmpty());
                if (scriptOk)
                    return info.code;
            }
        }
        return {};
    }

    QString LanguageCatalog::resolve(const QString &setting, const QStringList &systemUiLanguages)
    {
        const QString wanted = setting.trimmed();
        if (find(wanted))
            return wanted;
        const QString matched = matchSystemLanguages(systemUiLanguages);
        return matched.isEmpty() ? sourceLanguage() : matched;
    }

    QString LanguageCatalog::isolate(const QString &text)
    {
        if (text.isEmpty())
            return text;
        return QChar(0x2068) + text + QChar(0x2069);
    }
}
