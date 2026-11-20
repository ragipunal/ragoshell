#include "ragoshell-app/LanguageManager.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlEngine>
#include <QTranslator>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/LanguageCatalog.h"

namespace RagoShell
{
    namespace
    {
        /// Families tried, in order, for glyphs the UI font lacks. The order
        /// matters for Han characters: Japanese and Chinese share code points
        /// but draw them differently, so each language lists its own font first.
        QStringList fallbackFamilies(const QString &code)
        {
#if defined(Q_OS_WIN)
            if (code == QLatin1String("zh_CN"))
                return {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei")};
            if (code == QLatin1String("ja"))
                return {QStringLiteral("Yu Gothic UI"), QStringLiteral("Meiryo UI")};
            if (code == QLatin1String("hi"))
                return {QStringLiteral("Nirmala UI")};
            if (code == QLatin1String("ar"))
                return {QStringLiteral("Segoe UI")};
#elif defined(Q_OS_MACOS)
            if (code == QLatin1String("zh_CN"))
                return {QStringLiteral("PingFang SC")};
            if (code == QLatin1String("ja"))
                return {QStringLiteral("Hiragino Sans")};
            if (code == QLatin1String("hi"))
                return {QStringLiteral("Kohinoor Devanagari")};
            if (code == QLatin1String("ar"))
                return {QStringLiteral("Geeza Pro")};
#else
            if (code == QLatin1String("zh_CN"))
                return {QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans SC")};
            if (code == QLatin1String("ja"))
                return {QStringLiteral("Noto Sans CJK JP"), QStringLiteral("Noto Sans JP")};
            if (code == QLatin1String("hi"))
                return {QStringLiteral("Noto Sans Devanagari")};
            if (code == QLatin1String("ar"))
                return {QStringLiteral("Noto Sans Arabic"), QStringLiteral("Noto Naskh Arabic")};
#endif
            return {};
        }
    }

    LanguageManager::LanguageManager(SettingsManager *settings, QObject *parent)
        : QObject(parent), _settings(settings), _baseFont(QGuiApplication::font())
    {
    }

    LanguageManager::~LanguageManager()
    {
        removeTranslators();
    }

    void LanguageManager::setEngine(QQmlEngine *engine)
    {
        _engine = engine;
    }

    void LanguageManager::initialize(const QString &overrideCode)
    {
        apply(overrideCode.isEmpty() ? _settings->uiLanguage() : overrideCode);
    }

    bool LanguageManager::installExtraCatalog(const QString &qmFile)
    {
        const size_t before = _translators.size();
        installTranslator(qmFile);
        if (_translators.size() == before)
            return false;
        if (_engine)
            _engine->retranslate();
        return true;
    }

    QVariantList LanguageManager::languages() const
    {
        QVariantList list;
        for (const LanguageInfo &info : LanguageCatalog::all()) {
            list.append(QVariantMap{
                {QStringLiteral("code"), info.code},
                {QStringLiteral("endonym"), info.endonym},
                {QStringLiteral("englishName"), info.englishName},
                {QStringLiteral("rtl"), info.rtl},
            });
        }
        return list;
    }

    QString LanguageManager::current() const
    {
        const QString stored = _settings->uiLanguage();
        return LanguageCatalog::find(stored) ? stored : LanguageCatalog::systemSetting();
    }

    void LanguageManager::setCurrent(const QString &code)
    {
        const QString wanted = LanguageCatalog::find(code) ? code : LanguageCatalog::systemSetting();
        if (wanted == current())
            return;
        _settings->setUiLanguage(wanted);
        apply(wanted);
        emit currentChanged();
    }

    QString LanguageManager::effectiveEndonym() const
    {
        return endonym(_effective);
    }

    QString LanguageManager::systemEndonym() const
    {
        return endonym(LanguageCatalog::resolve(LanguageCatalog::systemSetting(),
                                                QLocale::system().uiLanguages()));
    }

    QString LanguageManager::endonym(const QString &code) const
    {
        const LanguageInfo *info = LanguageCatalog::find(code);
        return info ? info->endonym : code;
    }

    QString LanguageManager::isolate(const QString &text) const
    {
        return LanguageCatalog::isolate(text);
    }

    void LanguageManager::removeTranslators()
    {
        for (auto &translator : _translators)
            QCoreApplication::removeTranslator(translator.get());
        _translators.clear();
    }

    void LanguageManager::installTranslator(const QString &resourcePath)
    {
        auto translator = std::make_unique<QTranslator>();
        // Not every language has every catalog (no Qt catalog for Hindi, no
        // catalog at all for a language that is only half added): skip quietly.
        if (!translator->load(resourcePath))
            return;
        QCoreApplication::installTranslator(translator.get());
        _translators.push_back(std::move(translator));
    }

    void LanguageManager::apply(const QString &setting)
    {
        const QString code = LanguageCatalog::resolve(setting, QLocale::system().uiLanguages());
        const LanguageInfo *info = LanguageCatalog::find(code);
        if (!info)
            return;

        removeTranslators();
        // Qt's own catalogs first, ours last so ours can override them.
        if (info->qtTranslated) {
            installTranslator(QStringLiteral(":/i18n/qt/qtbase_%1.qm").arg(code));
            installTranslator(QStringLiteral(":/i18n/qt/qtdeclarative_%1.qm").arg(code));
        }
        installTranslator(QStringLiteral(":/i18n/%1_%2.qm").arg(QString::fromUtf8(BrandInfo::productId), code));

        QLocale::setDefault(QLocale(info->localeName));
        _effective = code;
        _rtl = info->rtl;
        QGuiApplication::setLayoutDirection(_rtl ? Qt::RightToLeft : Qt::LeftToRight);
        applyFonts(code);

        emit languageAboutToChange();
        if (_engine)
            _engine->retranslate();
        emit languageChanged();
    }

    void LanguageManager::applyFonts(const QString &code)
    {
        QFont font = _baseFont;
        const QStringList extra = fallbackFamilies(code);
        if (!extra.isEmpty()) {
            QStringList families{_baseFont.family()};
            families.append(extra);
            font.setFamilies(families);
        }
        QGuiApplication::setFont(font);
    }
}
