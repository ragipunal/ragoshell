#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace RagoShell
{
    /// One UI language the app is translated into (see docs/Localization.md).
    struct LanguageInfo
    {
        /// Setting value and .ts / .qm suffix: "tr", "zh_CN", "pt_BR".
        QString code;
        /// Name in the language itself, as shown in the language picker.
        QString endonym;
        QString englishName;
        /// Locale for QLocale (carries the script where it matters).
        QString localeName;
        /// ISO 15924 script of the translation.
        QString script;
        bool rtl = false;
        /// Qt ships qtbase_<code>.qm / qtdeclarative_<code>.qm for it.
        bool qtTranslated = false;
    };

    /**
     * @brief The single table of UI languages, plus the logic that maps the
     *        OS language list to one of them. GUI-free so it is unit-tested.
     *
     *        Adding a language = a row in all() and a .ts file.
     */
    class LanguageCatalog
    {
    public:
        /// Setting value that follows the OS language.
        static QString systemSetting() { return QStringLiteral("system"); }
        /// Source language of every translatable string.
        static QString sourceLanguage() { return QStringLiteral("en"); }

        static const QList<LanguageInfo> &all();
        /// nullptr when the code is not a supported language.
        static const LanguageInfo *find(const QString &code);

        /// Language code to use for a stored setting. "system" and unknown
        /// values resolve through the OS language list (QLocale::uiLanguages()
        /// style tags such as "tr-TR", "zh-Hans-CN"); English when nothing
        /// matches.
        static QString resolve(const QString &setting, const QStringList &systemUiLanguages);

        /// First language of a BCP 47 list that is supported, or "" if none.
        /// A script subtag has to match the translation's script, so
        /// "az-Arab" (South Azerbaijani, right to left) and "zh-Hant" do not
        /// match "az" and "zh_CN".
        static QString matchSystemLanguages(const QStringList &uiLanguages);

        /// Wraps text in Unicode first-strong isolate marks (U+2068 .. U+2069)
        /// so a database name or path inside a right-to-left sentence keeps its
        /// own direction. Harmless in left-to-right text.
        static QString isolate(const QString &text);
    };
}
