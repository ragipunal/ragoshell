#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    class SettingsManager;

    /**
     * @brief QML-facing read/write bridge over SettingsManager.
     *        Writes apply immediately to the live SettingsManager and are
     *        persisted via SettingsManager::save().
     */
    class SettingsBridge : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(AppSettings)
        QML_SINGLETON

        Q_PROPERTY(int batchSize READ batchSize WRITE setBatchSize
                   NOTIFY batchSizeChanged FINAL)
        Q_PROPERTY(int uuidEncoding READ uuidEncoding WRITE setUuidEncoding
                   NOTIFY uuidEncodingChanged FINAL)
        Q_PROPERTY(int timeZone READ timeZone WRITE setTimeZone
                   NOTIFY timeZoneChanged FINAL)
        /// View new shell tabs start in: 0 = Tree, 1 = Table, 2 = Text
        /// (ShellController::viewMode numbering, not core ViewMode).
        Q_PROPERTY(int defaultViewMode READ defaultViewMode
                   WRITE setDefaultViewMode NOTIFY defaultViewModeChanged FINAL)
        Q_PROPERTY(int autocompletionMode READ autocompletionMode
                   WRITE setAutocompletionMode NOTIFY autocompletionChanged FINAL)
        Q_PROPERTY(bool lineNumbers READ lineNumbers WRITE setLineNumbers
                   NOTIFY lineNumbersChanged FINAL)
        Q_PROPERTY(bool autoExpand READ autoExpand WRITE setAutoExpand
                   NOTIFY autoExpandChanged FINAL)
        Q_PROPERTY(bool minimizeToTray READ minimizeToTray
                   WRITE setMinimizeToTray NOTIFY minimizeToTrayChanged FINAL)
        Q_PROPERTY(QString startupScript READ startupScript WRITE setStartupScript
                   NOTIFY startupScriptChanged FINAL)
        Q_PROPERTY(bool confirmDestructiveQueries READ confirmDestructiveQueries
                   WRITE setConfirmDestructiveQueries NOTIFY confirmDestructiveQueriesChanged FINAL)
        Q_PROPERTY(int shellTimeoutSec READ shellTimeoutSec
                   WRITE setShellTimeoutSec NOTIFY shellTimeoutChanged FINAL)
        Q_PROPERTY(QString codeFontFamily READ codeFontFamily
                   WRITE setCodeFontFamily NOTIFY codeFontChanged FINAL)
        Q_PROPERTY(int codeFontSize READ codeFontSize
                   WRITE setCodeFontSize NOTIFY codeFontChanged FINAL)
        Q_PROPERTY(QStringList availableCodeFonts READ availableCodeFonts CONSTANT FINAL)
        /// Optional AI query assistant (Claude or Gemini API); off by default.
        Q_PROPERTY(bool aiAssistantEnabled READ aiAssistantEnabled
                   WRITE setAiAssistantEnabled NOTIFY aiSettingsChanged FINAL)
        /// "anthropic" or "gemini" (aiProviders); per-provider model and key
        /// go through aiModelFor() / aiApiKeyFor().
        Q_PROPERTY(QString aiProvider READ aiProvider WRITE setAiProvider NOTIFY aiSettingsChanged FINAL)
        Q_PROPERTY(QStringList aiProviders READ aiProviders CONSTANT FINAL)
        /// The saved settings chose the removed hosted "RagoShell AI"
        /// provider; Preferences explains why the assistant is off.
        Q_PROPERTY(bool aiHostedProviderRemoved READ aiHostedProviderRemoved NOTIFY aiSettingsChanged FINAL)
        Q_PROPERTY(QString secretStorageName READ secretStorageName CONSTANT FINAL)
        Q_PROPERTY(bool secretStorageInKeyStore READ secretStorageInKeyStore CONSTANT FINAL)
        Q_PROPERTY(QStringList aiProviderNames READ aiProviderNames CONSTANT FINAL)
        Q_PROPERTY(bool aiIncludeSampleValues READ aiIncludeSampleValues
                   WRITE setAiIncludeSampleValues NOTIFY aiSettingsChanged FINAL)
        Q_PROPERTY(bool aiAutoFallback READ aiAutoFallback
                   WRITE setAiAutoFallback NOTIFY aiSettingsChanged FINAL)
        /// Background schema collection: 0 off, 1 databases in use, 2 all
        /// databases (SettingsManager::SchemaCollectionMode).
        Q_PROPERTY(int schemaCollectionMode READ schemaCollectionMode
                   WRITE setSchemaCollectionMode NOTIFY schemaSettingsChanged FINAL)
        Q_PROPERTY(int schemaBackgroundSampleSize READ schemaBackgroundSampleSize
                   WRITE setSchemaBackgroundSampleSize NOTIFY schemaSettingsChanged FINAL)
        Q_PROPERTY(bool schemaUnmeteredOnly READ schemaUnmeteredOnly
                   WRITE setSchemaUnmeteredOnly NOTIFY schemaSettingsChanged FINAL)
        Q_PROPERTY(int maxBatchSize READ maxBatchSize CONSTANT FINAL)
        Q_PROPERTY(int schemaBackgroundMaxCollections READ schemaBackgroundMaxCollections
                   WRITE setSchemaBackgroundMaxCollections NOTIFY schemaSettingsChanged FINAL)
        Q_PROPERTY(bool checkForUpdates READ checkForUpdates
                   WRITE setCheckForUpdates NOTIFY updateSettingsChanged FINAL)
        Q_PROPERTY(bool checkForUpdatesLocked READ checkForUpdatesLocked CONSTANT FINAL)
        Q_PROPERTY(bool disableHttpsFeatures READ disableHttpsFeatures NOTIFY updateSettingsChanged FINAL)
        Q_PROPERTY(bool shareAnonymousUsage READ shareAnonymousUsage
                   WRITE setShareAnonymousUsage NOTIFY updateSettingsChanged FINAL)

    public:
        explicit SettingsBridge(SettingsManager *settings,
                                QObject *parent = nullptr);
        static SettingsBridge *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<SettingsBridge>::get();
        }

        int batchSize() const;
        void setBatchSize(int v);
        int uuidEncoding() const;
        void setUuidEncoding(int v);
        int timeZone() const;
        void setTimeZone(int v);
        int defaultViewMode() const;
        void setDefaultViewMode(int v);
        int autocompletionMode() const;
        void setAutocompletionMode(int v);
        bool lineNumbers() const;
        void setLineNumbers(bool v);
        bool autoExpand() const;
        void setAutoExpand(bool v);
        bool minimizeToTray() const;
        void setMinimizeToTray(bool v);
        QString startupScript() const;
        void setStartupScript(const QString &v);
        bool confirmDestructiveQueries() const;
        void setConfirmDestructiveQueries(bool v);
        int shellTimeoutSec() const;
        void setShellTimeoutSec(int v);

        QString codeFontFamily() const;
        void setCodeFontFamily(const QString &family);
        int codeFontSize() const;
        void setCodeFontSize(int px);
        QStringList availableCodeFonts() const;

        bool aiAssistantEnabled() const;
        void setAiAssistantEnabled(bool v);
        QString aiProvider() const;
        void setAiProvider(const QString &provider);
        QStringList aiProviders() const;
        bool aiHostedProviderRemoved() const;
        QStringList aiProviderNames() const;
        bool aiIncludeSampleValues() const;
        void setAiIncludeSampleValues(bool v);
        bool aiAutoFallback() const;
        int schemaCollectionMode() const;
        void setSchemaCollectionMode(int mode);
        int schemaBackgroundSampleSize() const;
        void setSchemaBackgroundSampleSize(int size);
        bool schemaUnmeteredOnly() const;
        void setSchemaUnmeteredOnly(bool on);
        int maxBatchSize() const;
        int schemaBackgroundMaxCollections() const;
        void setSchemaBackgroundMaxCollections(int count);
        bool checkForUpdates() const;
        void setCheckForUpdates(bool on);
        bool checkForUpdatesLocked() const;
        bool disableHttpsFeatures() const;
        bool shareAnonymousUsage() const;
        void setShareAnonymousUsage(bool on);
        void setAiAutoFallback(bool v);

        /// Selected model of a provider (its default when none is saved).
        Q_INVOKABLE QString aiModelFor(const QString &provider) const;
        Q_INVOKABLE void setAiModelFor(const QString &provider, const QString &model);
        /// Saved key of a provider (empty when none; see aiKeyEnvironmentVariable).
        Q_INVOKABLE QString aiApiKeyFor(const QString &provider) const;
        Q_INVOKABLE void setAiApiKeyFor(const QString &provider, const QString &key);
        /// Models to offer: the list last fetched from the provider, or the
        /// built-in one.
        Q_INVOKABLE QStringList aiModelsFor(const QString &provider) const;
        /// Whether aiModelsFor() comes from the provider.
        Q_INVOKABLE bool aiModelsFetched(const QString &provider) const;
        /// The environment variable that provides a key, or "".
        /// baseUrl matters for the OpenAI-compatible provider only (its
        /// environment key is used at a preset's own address, docs: T12).
        Q_INVOKABLE QString aiKeyEnvironmentVariable(const QString &provider, const QString &baseUrl = {}) const;
        /// Provider page where an API key is created (Preferences › How to get a key).
        Q_INVOKABLE QString aiKeyPageUrl(const QString &provider, const QString &baseUrl = {}) const;
        /// Whether the provider has a base address field (OpenAI-compatible).
        Q_INVOKABLE bool aiHasBaseUrl(const QString &provider) const;
        /// False for Ollama: Preferences hides the key field.
        Q_INVOKABLE bool aiUsesKey(const QString &provider) const;
        /// Answer time limit in seconds (providers with a base address).
        Q_INVOKABLE int aiTimeoutFor(const QString &provider) const;
        Q_INVOKABLE void setAiTimeoutFor(const QString &provider, int seconds);
        Q_INVOKABLE int aiMinTimeout() const;
        Q_INVOKABLE int aiMaxTimeout() const;
        Q_INVOKABLE QString aiBaseUrlFor(const QString &provider) const;
        /// Address a provider starts from when none is saved (Ollama: localhost).
        Q_INVOKABLE QString aiDefaultBaseUrl(const QString &provider) const;
        /// Saves the address; another origin forgets the saved key (docs: T1).
        Q_INVOKABLE void setAiBaseUrlFor(const QString &provider, const QString &baseUrl);
        /// {ok, normalized, error, host, origin} for the address field.
        Q_INVOKABLE QVariantMap checkAiBaseUrl(const QString &baseUrl, bool hasKey) const;
        /// [{id, name, baseUrl}] of the OpenAI-compatible presets.
        Q_INVOKABLE QVariantList aiPresets() const;
        /// Preset id at exactly this address, "" for a custom address.
        Q_INVOKABLE QString aiPresetFor(const QString &baseUrl) const;
        /// Where the key that encrypts saved passwords is kept ("Windows Credential
        /// Manager", "Keychain", "Secret Service"); empty without a key store.
        QString secretStorageName() const;
        /// False when the key is in a file next to the settings (no key store).
        bool secretStorageInKeyStore() const;

        /// Persist all settings to ~/.ragoshell config.
        Q_INVOKABLE bool save();

    Q_SIGNALS:
        void batchSizeChanged();
        void uuidEncodingChanged();
        void timeZoneChanged();
        void defaultViewModeChanged();
        void autocompletionChanged();
        void lineNumbersChanged();
        void autoExpandChanged();
        void minimizeToTrayChanged();
        void startupScriptChanged();
        void confirmDestructiveQueriesChanged();
        void shellTimeoutChanged();
        void codeFontChanged();
        void aiSettingsChanged();
        void schemaSettingsChanged();
        void updateSettingsChanged();

    private:
        SettingsManager *_settings;
        QString _defaultCodeFont;
        QStringList _availableCodeFonts;
    };
}
