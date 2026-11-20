#include "gtest/gtest.h"

#include "ragoshell/core/utils/LanguageCatalog.h"

#include <QLocale>
#include <QString>

using RagoShell::LanguageCatalog;

TEST(LanguageCatalog_Tests, Table_HasFifteenLanguagesAndArabicIsTheOnlyRightToLeftOne)
{
    const auto &all = LanguageCatalog::all();
    EXPECT_EQ(15, all.size());

    QStringList rtl;
    for (const auto &info : all) {
        EXPECT_FALSE(info.endonym.isEmpty()) << qPrintable(info.code);
        EXPECT_FALSE(info.localeName.isEmpty()) << qPrintable(info.code);
        EXPECT_NE(QLocale::AnyLanguage, QLocale(info.localeName).language()) << qPrintable(info.code);
        if (info.rtl)
            rtl << info.code;
    }
    EXPECT_EQ(QStringList{QStringLiteral("ar")}, rtl);
}

TEST(LanguageCatalog_Tests, Table_RightToLeftFlagAgreesWithQtLocale)
{
    for (const auto &info : LanguageCatalog::all())
        EXPECT_EQ(info.rtl, QLocale(info.localeName).textDirection() == Qt::RightToLeft)
            << qPrintable(info.code);
}

TEST(LanguageCatalog_Tests, Find_KnowsCodesFromTheTable)
{
    ASSERT_NE(nullptr, LanguageCatalog::find(QStringLiteral("tr")));
    EXPECT_EQ(QStringLiteral("Turkish"), LanguageCatalog::find(QStringLiteral("tr"))->englishName);
    EXPECT_EQ(nullptr, LanguageCatalog::find(QStringLiteral("xx")));
    EXPECT_EQ(nullptr, LanguageCatalog::find(QStringLiteral("system")));
}

TEST(LanguageCatalog_Tests, Resolve_ExplicitLanguageWinsOverTheSystem)
{
    EXPECT_EQ(QStringLiteral("ja"),
              LanguageCatalog::resolve(QStringLiteral("ja"), {QStringLiteral("de-DE")}));
}

TEST(LanguageCatalog_Tests, Resolve_SystemFollowsTheFirstSupportedOsLanguage)
{
    EXPECT_EQ(QStringLiteral("de"),
              LanguageCatalog::resolve(QStringLiteral("system"),
                                       {QStringLiteral("it-IT"), QStringLiteral("de-AT"), QStringLiteral("fr-FR")}));
}

TEST(LanguageCatalog_Tests, Resolve_FallsBackToEnglish)
{
    EXPECT_EQ(QStringLiteral("en"),
              LanguageCatalog::resolve(QStringLiteral("system"), {QStringLiteral("it-IT"), QStringLiteral("ko-KR")}));
    EXPECT_EQ(QStringLiteral("en"), LanguageCatalog::resolve(QStringLiteral("system"), {}));
    EXPECT_EQ(QStringLiteral("en"),
              LanguageCatalog::resolve(QStringLiteral("klingon"), {QStringLiteral("it-IT")}));
}

TEST(LanguageCatalog_Tests, Match_ChineseNeedsTheSimplifiedScript)
{
    EXPECT_EQ(QStringLiteral("zh_CN"), LanguageCatalog::matchSystemLanguages({QStringLiteral("zh-Hans-CN")}));
    EXPECT_EQ(QStringLiteral("zh_CN"), LanguageCatalog::matchSystemLanguages({QStringLiteral("zh-CN")}));
    EXPECT_EQ(QStringLiteral("zh_CN"), LanguageCatalog::matchSystemLanguages({QStringLiteral("zh-SG")}));
    EXPECT_EQ(QString(), LanguageCatalog::matchSystemLanguages({QStringLiteral("zh-Hant-TW")}));
    EXPECT_EQ(QString(), LanguageCatalog::matchSystemLanguages({QStringLiteral("zh-TW")}));
    EXPECT_EQ(QString(), LanguageCatalog::matchSystemLanguages({QStringLiteral("zh-HK")}));
}

TEST(LanguageCatalog_Tests, Match_ArabicScriptTurkicVariantsAreNotTheLatinOrCyrillicTranslations)
{
    EXPECT_EQ(QStringLiteral("az"), LanguageCatalog::matchSystemLanguages({QStringLiteral("az-Latn-AZ")}));
    EXPECT_EQ(QStringLiteral("az"), LanguageCatalog::matchSystemLanguages({QStringLiteral("az-AZ")}));
    EXPECT_EQ(QString(), LanguageCatalog::matchSystemLanguages({QStringLiteral("az-Arab-IR")}));
    EXPECT_EQ(QStringLiteral("kk"), LanguageCatalog::matchSystemLanguages({QStringLiteral("kk-KZ")}));
    EXPECT_EQ(QString(), LanguageCatalog::matchSystemLanguages({QStringLiteral("kk-Arab-CN")}));
    EXPECT_EQ(QStringLiteral("uz"), LanguageCatalog::matchSystemLanguages({QStringLiteral("uz-Latn-UZ")}));
    EXPECT_EQ(QString(), LanguageCatalog::matchSystemLanguages({QStringLiteral("uz-Arab-AF")}));
    EXPECT_EQ(QStringLiteral("ky"), LanguageCatalog::matchSystemLanguages({QStringLiteral("ky-KG")}));
}

TEST(LanguageCatalog_Tests, Match_AcceptsUnderscoresAndAnyCase)
{
    EXPECT_EQ(QStringLiteral("pt_BR"), LanguageCatalog::matchSystemLanguages({QStringLiteral("pt_PT")}));
    EXPECT_EQ(QStringLiteral("tr"), LanguageCatalog::matchSystemLanguages({QStringLiteral("TR-tr")}));
    EXPECT_EQ(QStringLiteral("ja"), LanguageCatalog::matchSystemLanguages({QStringLiteral("ja-JP")}));
    EXPECT_EQ(QStringLiteral("ar"), LanguageCatalog::matchSystemLanguages({QStringLiteral("ar-SA")}));
}

TEST(LanguageCatalog_Tests, Isolate_WrapsInFirstStrongIsolateMarks)
{
    const QString wrapped = LanguageCatalog::isolate(QStringLiteral("db.users"));
    EXPECT_EQ(QChar(0x2068), wrapped.front());
    EXPECT_EQ(QChar(0x2069), wrapped.back());
    EXPECT_EQ(QStringLiteral("db.users"), wrapped.mid(1, wrapped.size() - 2));
    EXPECT_TRUE(LanguageCatalog::isolate(QString()).isEmpty());
}

// UI language must never change how identifiers are case-converted: with a
// Turkish or Azerbaijani default locale "index" has to stay "INDEX", not "İNDEX".
TEST(LanguageCatalog_Tests, CaseConversionOfIdentifiersIgnoresTheTurkicDottedI)
{
    const QLocale previous = QLocale();
    for (const char *name : {"tr_TR", "az_Latn_AZ"}) {
        QLocale::setDefault(QLocale(QString::fromLatin1(name)));
        EXPECT_EQ(QStringLiteral("INDEX"), QStringLiteral("index").toUpper()) << name;
        EXPECT_EQ(QStringLiteral("id"), QStringLiteral("ID").toLower()) << name;
        EXPECT_EQ(QStringLiteral("findandmodify"), QStringLiteral("findAndModify").toLower()) << name;
    }
    QLocale::setDefault(previous);
}

// A database tool shows numbers with Latin digits in every language
// (Arabic-Indic and Devanagari digits are not what developers expect).
TEST(LanguageCatalog_Tests, Table_LocalesFormatNumbersWithLatinDigits)
{
    for (const auto &info : RagoShell::LanguageCatalog::all()) {
        const QString text = QLocale(info.localeName).toString(1234567);
        for (const QChar c : text)
            if (c.isDigit())
                EXPECT_TRUE(c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                    << qPrintable(info.code) << ": " << qPrintable(text);
    }
}
