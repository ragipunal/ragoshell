#include "ragoshell-app/SettingsBridge.h"

#include <QFontDatabase>
#include <QSet>
#include <algorithm>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AiQueryAssistant.h"
#include "ragoshell-app/AiAssistant.h"
#include "ragoshell/utils/RagoCrypt.h"
#include "ragoshell/core/Enums.h"

namespace RagoShell
{
    namespace
    {
        QStringList preferredCodingFontCandidates()
        {
            return {
                QStringLiteral("Cascadia Mono"),
                QStringLiteral("Cascadia Code"),
                QStringLiteral("JetBrains Mono"),
                QStringLiteral("Fira Code"),
                QStringLiteral("SF Mono"),
                QStringLiteral("Menlo"),
                QStringLiteral("Consolas"),
                QStringLiteral("Source Code Pro"),
                QStringLiteral("IBM Plex Mono"),
                QStringLiteral("Ubuntu Mono"),
                QStringLiteral("DejaVu Sans Mono"),
                QStringLiteral("Liberation Mono"),
                QStringLiteral("Monaco"),
                QStringLiteral("Courier New")
            };
        }
    }

    SettingsBridge::SettingsBridge(SettingsManager *settings, QObject *parent)
        : QObject(parent), _settings(settings)
    {
        const QStringList allFamilies = QFontDatabase::families();
        QSet<QString> installedSet(allFamilies.begin(), allFamilies.end());

        for (const QString &cand : preferredCodingFontCandidates()) {
            if (installedSet.contains(cand)) {
                if (_defaultCodeFont.isEmpty())
                    _defaultCodeFont = cand;
                _availableCodeFonts.append(cand);
            }
        }

        if (_defaultCodeFont.isEmpty()) {
            const QFont sysFixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            _defaultCodeFont = sysFixed.family().isEmpty()
                ? QStringLiteral("monospace")
                : sysFixed.family();
            if (!_availableCodeFonts.contains(_defaultCodeFont))
                _availableCodeFonts.prepend(_defaultCodeFont);
        }

        for (const QString &fam : allFamilies) {
            if (fam.startsWith(QLatin1Char('@')))
                continue;
            if (QFontDatabase::isFixedPitch(fam) && !_availableCodeFonts.contains(fam))
                _availableCodeFonts.append(fam);
        }
    }

    int SettingsBridge::batchSize() const { return _settings->batchSize(); }
    void SettingsBridge::setBatchSize(int v)
    {
        if (v <= 0)
            return;
        _settings->setBatchSize(v);
        emit batchSizeChanged();
    }

    int SettingsBridge::uuidEncoding() const
    {
        return static_cast<int>(_settings->uuidEncoding());
    }
    void SettingsBridge::setUuidEncoding(int v)
    {
        _settings->setUuidEncoding(static_cast<UUIDEncoding>(v));
        emit uuidEncodingChanged();
    }

    int SettingsBridge::timeZone() const
    {
        return static_cast<int>(_settings->timeZone());
    }
    void SettingsBridge::setTimeZone(int v)
    {
        _settings->setTimeZone(static_cast<SupportedTimes>(v));
        emit timeZoneChanged();
    }

    int SettingsBridge::defaultViewMode() const
    {
        switch (_settings->viewMode()) {
        case Table: return 1;
        case Text:  return 2;
        case Tree:
        case Custom:
        default:    return 0;
        }
    }
    void SettingsBridge::setDefaultViewMode(int v)
    {
        const ViewMode mode = v == 1 ? Table : v == 2 ? Text : Tree;
        if (_settings->viewMode() == mode)
            return;
        _settings->setViewMode(mode);
        emit defaultViewModeChanged();
    }

    int SettingsBridge::autocompletionMode() const
    {
        return static_cast<int>(_settings->autocompletionMode());
    }
    void SettingsBridge::setAutocompletionMode(int v)
    {
        _settings->setAutocompletionMode(static_cast<AutocompletionMode>(v));
        emit autocompletionChanged();
    }

    bool SettingsBridge::lineNumbers() const
    {
        return _settings->lineNumbers();
    }
    void SettingsBridge::setLineNumbers(bool v)
    {
        _settings->setLineNumbers(v);
        emit lineNumbersChanged();
    }

    bool SettingsBridge::autoExpand() const
    {
        return _settings->autoExpand();
    }
    void SettingsBridge::setAutoExpand(bool v)
    {
        _settings->setAutoExpand(v);
        emit autoExpandChanged();
    }

    bool SettingsBridge::minimizeToTray() const
    {
        return _settings->minimizeToTray();
    }
    void SettingsBridge::setMinimizeToTray(bool v)
    {
        _settings->setMinimizeToTray(v);
        emit minimizeToTrayChanged();
    }

    QString SettingsBridge::startupScript() const
    {
        return _settings->startupScript();
    }
    void SettingsBridge::setStartupScript(const QString &v)
    {
        _settings->setStartupScript(v);
        emit startupScriptChanged();
    }

    bool SettingsBridge::confirmDestructiveQueries() const
    {
        const QVariant v = _settings->cacheData(QStringLiteral("confirmDestructiveQueries"));
        return v.isValid() ? v.toBool() : true;
    }

    void SettingsBridge::setConfirmDestructiveQueries(bool v)
    {
        const QVariant existing = _settings->cacheData(QStringLiteral("confirmDestructiveQueries"));
        if (existing.isValid() && existing.toBool() == v)
            return;
        _settings->addCacheData(QStringLiteral("confirmDestructiveQueries"), v);
        emit confirmDestructiveQueriesChanged();
    }

    bool SettingsBridge::aiAssistantEnabled() const
    {
        return _settings->aiAssistantEnabled();
    }
    void SettingsBridge::setAiAssistantEnabled(bool v)
    {
        _settings->setAiAssistantEnabled(v);
        emit aiSettingsChanged();
    }

    QString SettingsBridge::aiProvider() const
    {
        return AiQueryAssistant::providerId(AiQueryAssistant::providerFromId(_settings->aiProvider()));
    }
    void SettingsBridge::setAiProvider(const QString &provider)
    {
        _settings->setAiProvider(AiQueryAssistant::providerId(AiQueryAssistant::providerFromId(provider)));
        emit aiSettingsChanged();
    }

    QStringList SettingsBridge::aiProviders() const
    {
        return AiQueryAssistant::providerIds();
    }

    bool SettingsBridge::aiHostedProviderRemoved() const
    {
        return _settings->aiHostedProviderRemoved();
    }

    QStringList SettingsBridge::aiProviderNames() const
    {
        QStringList names;
        for (const QString &id : AiQueryAssistant::providerIds())
            names << AiQueryAssistant::providerName(AiQueryAssistant::providerFromId(id));
        return names;
    }

    bool SettingsBridge::aiIncludeSampleValues() const
    {
        return _settings->aiIncludeSampleValues();
    }
    void SettingsBridge::setAiIncludeSampleValues(bool v)
    {
        _settings->setAiIncludeSampleValues(v);
        emit aiSettingsChanged();
    }

    bool SettingsBridge::aiAutoFallback() const
    {
        return _settings->aiAutoFallback();
    }
    void SettingsBridge::setAiAutoFallback(bool v)
    {
        _settings->setAiAutoFallback(v);
        emit aiSettingsChanged();
    }

    int SettingsBridge::schemaCollectionMode() const
    {
        return static_cast<int>(_settings->schemaCollectionMode());
    }
    void SettingsBridge::setSchemaCollectionMode(int mode)
    {
        using Mode = SettingsManager::SchemaCollectionMode;
        _settings->setSchemaCollectionMode(static_cast<Mode>(
            std::clamp(mode, static_cast<int>(Mode::Off), static_cast<int>(Mode::WholeDatabase))));
        emit schemaSettingsChanged();
    }

    int SettingsBridge::schemaBackgroundSampleSize() const
    {
        return _settings->schemaBackgroundSampleSize();
    }
    void SettingsBridge::setSchemaBackgroundSampleSize(int size)
    {
        _settings->setSchemaBackgroundSampleSize(size);
        emit schemaSettingsChanged();
    }

    bool SettingsBridge::schemaUnmeteredOnly() const
    {
        return _settings->schemaUnmeteredOnly();
    }
    void SettingsBridge::setSchemaUnmeteredOnly(bool on)
    {
        _settings->setSchemaUnmeteredOnly(on);
        emit schemaSettingsChanged();
    }
    int SettingsBridge::maxBatchSize() const
    {
        return SettingsManager::kMaxBatchSize;
    }

    int SettingsBridge::schemaBackgroundMaxCollections() const
    {
        return _settings->schemaBackgroundMaxCollections();
    }
    void SettingsBridge::setSchemaBackgroundMaxCollections(int count)
    {
        _settings->setSchemaBackgroundMaxCollections(count);
        emit schemaSettingsChanged();
    }

    bool SettingsBridge::checkForUpdates() const
    {
        return _settings->checkForUpdates();
    }
    void SettingsBridge::setCheckForUpdates(bool on)
    {
        if (_settings->checkForUpdates() == on)
            return;
        _settings->setCheckForUpdates(on);
        emit updateSettingsChanged();
    }

    bool SettingsBridge::shareAnonymousUsage() const
    {
        const QVariant v = _settings->cacheData(QStringLiteral("shareAnonymousUsage"));
        return v.isValid() ? v.toBool() : true;
    }
    void SettingsBridge::setShareAnonymousUsage(bool on)
    {
        const QVariant existing = _settings->cacheData(QStringLiteral("shareAnonymousUsage"));
        if (existing.isValid() && existing.toBool() == on)
            return;
        _settings->addCacheData(QStringLiteral("shareAnonymousUsage"), on);
        emit updateSettingsChanged();
    }

    QString SettingsBridge::aiModelFor(const QString &provider) const
    {
        const QString model = _settings->aiModel(provider);
        return model.isEmpty() ? AiQueryAssistant::defaultModel(AiQueryAssistant::providerFromId(provider))
                               : model;
    }
    void SettingsBridge::setAiModelFor(const QString &provider, const QString &model)
    {
        const QString m = model.trimmed();
        _settings->setAiModel(provider, m == AiQueryAssistant::defaultModel(AiQueryAssistant::providerFromId(provider))
                                            ? QString() : m);
        emit aiSettingsChanged();
    }

    QString SettingsBridge::aiApiKeyFor(const QString &provider) const
    {
        return _settings->aiApiKey(provider);
    }
    void SettingsBridge::setAiApiKeyFor(const QString &provider, const QString &key)
    {
        _settings->setAiApiKey(provider, key.trimmed());
        emit aiSettingsChanged();
    }

    QStringList SettingsBridge::aiModelsFor(const QString &provider) const
    {
        const QStringList fetched = _settings->aiAvailableModels(provider);
        if (fetched.isEmpty())
            return AiQueryAssistant::modelIds(AiQueryAssistant::providerFromId(provider));
        // Aliases such as gemini-flash-latest are not listed by the API.
        QStringList models;
        for (const QString &m : AiQueryAssistant::modelIds(AiQueryAssistant::providerFromId(provider)))
            if (m.endsWith(QLatin1String("-latest")))
                models << m;
        return models + fetched;
    }

    bool SettingsBridge::aiModelsFetched(const QString &provider) const
    {
        return !_settings->aiAvailableModels(provider).isEmpty();
    }

    QString SettingsBridge::aiKeyEnvironmentVariable(const QString &provider, const QString &baseUrl) const
    {
        const AiQueryAssistant::Provider p = AiQueryAssistant::providerFromId(provider);
        const QString base = AiQueryAssistant::checkBaseUrl(baseUrl, false, true).normalized;
        for (const QString &name : AiQueryAssistant::apiKeyEnvironmentVariables(p, base))
            if (!qEnvironmentVariable(name.toUtf8().constData()).trimmed().isEmpty())
                return name;
        return {};
    }

    QString SettingsBridge::aiKeyPageUrl(const QString &provider, const QString &baseUrl) const
    {
        const QString base = AiQueryAssistant::checkBaseUrl(baseUrl, false, true).normalized;
        return AiQueryAssistant::apiKeyPageUrl(AiQueryAssistant::providerFromId(provider), base);
    }

    bool SettingsBridge::aiHasBaseUrl(const QString &provider) const
    {
        return AiQueryAssistant::hasConfigurableBaseUrl(AiQueryAssistant::providerFromId(provider));
    }

    bool SettingsBridge::aiUsesKey(const QString &provider) const
    {
        return AiQueryAssistant::usesApiKey(AiQueryAssistant::providerFromId(provider));
    }

    int SettingsBridge::aiTimeoutFor(const QString &provider) const
    {
        return _settings->aiTimeoutSec(provider);
    }

    void SettingsBridge::setAiTimeoutFor(const QString &provider, int seconds)
    {
        _settings->setAiTimeoutSec(provider, seconds);
        emit aiSettingsChanged();
    }

    int SettingsBridge::aiMinTimeout() const
    {
        return AiQueryAssistant::kMinTimeoutSec;
    }

    int SettingsBridge::aiMaxTimeout() const
    {
        return AiQueryAssistant::kMaxTimeoutSec;
    }

    QString SettingsBridge::aiDefaultBaseUrl(const QString &provider) const
    {
        return AiQueryAssistant::defaultBaseUrl(AiQueryAssistant::providerFromId(provider));
    }

    QString SettingsBridge::aiBaseUrlFor(const QString &provider) const
    {
        return _settings->aiBaseUrl(provider);
    }

    void SettingsBridge::setAiBaseUrlFor(const QString &provider, const QString &baseUrl)
    {
        _settings->setAiBaseUrl(provider, baseUrl.trimmed());
        emit aiSettingsChanged();
    }

    QVariantMap SettingsBridge::checkAiBaseUrl(const QString &baseUrl, bool hasKey) const
    {
        const AiQueryAssistant::BaseUrlCheck check = AiQueryAssistant::checkBaseUrl(baseUrl, hasKey, aiAllowsPrivateHttp());
        QString scope;
        if (check.ok()) {
            switch (AiQueryAssistant::hostScope(check.normalized)) {
            case AiQueryAssistant::HostScope::ThisDevice:
                scope = QStringLiteral("device");
                break;
            case AiQueryAssistant::HostScope::LocalNetwork:
                scope = QStringLiteral("network");
                break;
            case AiQueryAssistant::HostScope::Internet:
                scope = QStringLiteral("internet");
                break;
            }
        }
        return {{QStringLiteral("ok"), check.ok()},
                {QStringLiteral("scope"), scope},
                {QStringLiteral("normalized"), check.normalized},
                {QStringLiteral("error"), aiBaseUrlErrorText(check.error)},
                {QStringLiteral("host"), check.ok() ? AiQueryAssistant::urlHost(check.normalized) : QString()},
                {QStringLiteral("origin"), check.ok() ? AiQueryAssistant::urlOrigin(check.normalized) : QString()}};
    }

    QVariantList SettingsBridge::aiPresets() const
    {
        QVariantList list;
        for (const AiQueryAssistant::Preset &p : AiQueryAssistant::openAiPresets())
            list << QVariantMap{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name},
                                {QStringLiteral("baseUrl"), p.baseUrl}};
        return list;
    }

    QString SettingsBridge::aiPresetFor(const QString &baseUrl) const
    {
        const AiQueryAssistant::Preset *p =
            AiQueryAssistant::presetForBaseUrl(AiQueryAssistant::checkBaseUrl(baseUrl, false, true).normalized);
        return p ? p->id : QString();
    }

    QString SettingsBridge::secretStorageName() const
    {
        return RagoCrypt::keyStorage().storeName;
    }

    bool SettingsBridge::secretStorageInKeyStore() const
    {
        return RagoCrypt::keyStorage().inKeyStore;
    }

    int SettingsBridge::shellTimeoutSec() const
    {
        return _settings->shellTimeoutSec();
    }
    void SettingsBridge::setShellTimeoutSec(int v)
    {
        if (_settings->shellTimeoutSec() == std::abs(v))
            return;
        _settings->setShellTimeoutSec(v);
        // Apply to already-open connections and shell tabs too, not only to
        // connections opened later.
        if (App *app = AppRegistry::instance().app()) {
            for (auto const &server : app->getServers())
                server->changeWorkerShellTimeout(_settings->shellTimeoutSec());
        }
        emit shellTimeoutChanged();
    }

    QString SettingsBridge::codeFontFamily() const
    {
        const QString saved = _settings->textFontFamily().trimmed();
        if (!saved.isEmpty() && QFontDatabase::families().contains(saved))
            return saved;
        return _defaultCodeFont;
    }

    void SettingsBridge::setCodeFontFamily(const QString &family)
    {
        const QString trimmed = family.trimmed();
        if (trimmed == codeFontFamily())
            return;
        _settings->setTextFontFamily(trimmed);
        emit codeFontChanged();
    }

    int SettingsBridge::codeFontSize() const
    {
        const int saved = _settings->textFontPointSize();
        if (saved >= 10 && saved <= 28)
            return saved;
        return 13;
    }

    void SettingsBridge::setCodeFontSize(int px)
    {
        const int clamped = std::clamp(px, 10, 28);
        if (clamped == codeFontSize() && _settings->textFontPointSize() == clamped)
            return;
        _settings->setTextFontPointSize(clamped);
        emit codeFontChanged();
    }

    QStringList SettingsBridge::availableCodeFonts() const
    {
        return _availableCodeFonts;
    }

    bool SettingsBridge::save() { return _settings->save(); }
}
