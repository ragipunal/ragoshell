#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QSet>
#include <QDir>

#include <vector>
#include <algorithm>
#include <cstdlib>
#include <memory>

#include "ragoshell/core/Enums.h"
#include "ragoshell/core/utils/AppPaths.h"

namespace RagoShell
{
    class ConnectionSettings;
    class KeyStore;
    struct ConfigFileAndImportFunction;
        
    // Settings directory with a trailing slash (see appDataDir()).
    inline QString configDirectory() { return appDataDir(); }

    // Current cache directory (version-independent across upgrades)
    inline QString cacheDirectory() { return configDirectory() + "cache/"; }
    // Current config file (version-independent across upgrades)
    inline QString configFilePath() { return configDirectory() + appFileName("json"); }

/* ----------------------------- SettingsManager ------------------------------ */

    /**
     * @brief SettingsManager gives you access to all settings, that is used
     *        by RagoShell. It can load() and save() them. Config file usually
     *        located here: ~/.ragoshell/ragoshell.json
     *
     *        You can access this manager via:
     *        AppRegistry::instance().settingsManager()
     *
     * @threadsafe no
     */
    class SettingsManager
    {
    public:
        typedef std::vector<ConnectionSettings *> ConnectionSettingsContainerType;
        typedef QMap<QString, QVariant> ToolbarSettingsContainerType;

        /**
         * @brief Creates SettingsManager for config file in default location
         *        (usually ~/.ragoshell/ragoshell.json)
         */
        SettingsManager();

        /// Testable constructor with explicit KeyStore injection (nullptr disables OS storage).
        explicit SettingsManager(std::shared_ptr<KeyStore> keyStore);

        /**
         * @brief Cleanup owned objects
         */
        ~SettingsManager();

        /**
         * @brief Current configuration schema model version (independent of app release version).
         */
        static QString schemaVersion();

        /**
         * @brief Migrates a parsed config map in-place if its schema 'version'
         *        differs from schemaVersion(). Leaves map untouched and returns
         *        false when the schema version already matches.
         */
        static bool migrateMapToCurrentSchema(QVariantMap &map, QString *previousVersion = nullptr);

        enum class AiProviderMigration
        {
            None,            ///< empty (default Anthropic) or a supported id: untouched
            HostedRemoved,   ///< the removed hosted "RagoShell AI" provider
            Unknown,         ///< any other id (hand edit, newer build)
        };

        /**
         * @brief A saved provider id the app no longer supports would read as
         *        Anthropic and could silently send schema data with a personal
         *        key from the environment. For the removed hosted provider and
         *        any other unsupported non-empty id, clears provider and turns
         *        the assistant off; leaves an empty or supported id untouched.
         */
        static AiProviderMigration migrateAiProvider(QString &provider, bool &assistantEnabled);

        /**
         * @brief Load settings from config file.
         * @return true if success, false otherwise
         */
        bool load();

        /**
         * @brief Saves all settings to config file.
         * @return true if success, false otherwise
         */
        bool save();

        /**
         * @brief Adds connection to the end of list.
         * Connection now will be owned by SettingsManager.
         */
        static void addConnection(ConnectionSettings *connection);

        /**
         * @brief Removes connection by index
         */
        void removeConnection(ConnectionSettings *connection);


        /**
        * @brief  Finds and returns original (non-clone) connection settings which is 
        *         loaded/saved from/into RagoShell config. file.
        * @return If uniqueID is valid returns original connection settings, 
        *         nullptr otherwise.
        */
        ConnectionSettings* getConnectionSettingsByUuid(QString const& uuid) const;
        ConnectionSettings* getConnectionSettingsByUuid(std::string const& uuid) const;

        void reorderConnections(const ConnectionSettingsContainerType &connections);

        void setToolbarSettings(QString toolbarName, bool visible);

        /**
         * @brief Returns list of connections
         */
        ConnectionSettingsContainerType connections() const { return _connections; }
        
        ToolbarSettingsContainerType toolbars() const { return _toolbars; }

        void setUuidEncoding(UUIDEncoding encoding) { _uuidEncoding = encoding; }
        UUIDEncoding uuidEncoding() const { return _uuidEncoding; }

        void setTimeZone(SupportedTimes timeZ) { _timeZone = timeZ; }
        SupportedTimes timeZone() const { return _timeZone; }

        void setViewMode(ViewMode viewMode) { _viewMode = viewMode; }
        ViewMode viewMode() const { return _viewMode; }

        void setAutocompletionMode(AutocompletionMode mode) { _autocompletionMode = mode; }
        AutocompletionMode autocompletionMode() const { return _autocompletionMode; }

        void setAutoExpand(bool isExpand) { _autoExpand = isExpand; }
        bool autoExpand() const { return _autoExpand; }

        void setAutoExec(bool isAutoExec) { _autoExec = isAutoExec; }
        bool autoExec() const { return _autoExec; }

        void setMinimizeToTray(bool isMinimizingToTray) { _minimizeToTray = isMinimizingToTray; }
        bool minimizeToTray() const { return _minimizeToTray; }

        void setLineNumbers(bool showLineNumbers) { _lineNumbers = showLineNumbers; }
        bool lineNumbers() const { return _lineNumbers; }

        /// JavaScript that runs before every query in a shell tab (helper functions, shortcuts).
        /// Replaces the old "load ~/.mongorc.js" switch; the file is imported once.
        void setStartupScript(const QString &script) { _startupScript = script; }
        QString startupScript() const { return _startupScript; }

        void setDisableConnectionShortcuts(bool isDisable) { _disableConnectionShortcuts = isDisable; }
        bool disableConnectionShortcuts() const { return _disableConnectionShortcuts; }

        void addAcceptedEulaVersion(QString const& version) { _acceptedEulaVersions.insert(version); }
        QSet<QString> const& acceptedEulaVersions() const { return _acceptedEulaVersions; }

        QSet<QString>::iterator addDbVersionConnected(QString const& version) { 
            return _dbVersionsConnected.insert(version); 
        }
        QSet<QString> const& dbVersionsConnected() const { return _dbVersionsConnected; }

        void setCheckForUpdates(bool checkForUpdates);
        bool checkForUpdates() const;
        bool isCheckForUpdatesLocked() const;

        /// Documents per page; never more than kMaxBatchSize, so one page cannot exhaust memory.
        static constexpr int kMaxBatchSize = 1000;
        void setBatchSize(int batchSize) { _batchSize = std::clamp(batchSize, 1, kMaxBatchSize); }
        int batchSize() const { return _batchSize; }

        /// "Automatic" (touch sizes on tablets), "Compact" or "Touch".
        QString uiDensity() const { return _uiDensity; }
        void setUiDensity(const QString &density) { _uiDensity = density; }
        QString uiTheme() const { return _uiTheme; }
        void setUiTheme(const QString& theme) { _uiTheme = theme; }
        /// UI language code ("system" or a code from LanguageManager); see docs/Localization.md.
        QString uiLanguage() const { return _uiLanguage; }
        void setUiLanguage(const QString& language) { _uiLanguage = language; }

        QString textFontFamily() const { return _textFontFamily; }
        void setTextFontFamily(const QString& fontFamily);

        int textFontPointSize() const { return _textFontPointSize; }
        void setTextFontPointSize(int pointSize);

        int mongoTimeoutSec() const { return _mongoTimeoutSec; }
        int shellTimeoutSec() const { return _shellTimeoutSec; }

        void setShellTimeoutSec(int newValue) { _shellTimeoutSec = std::abs(newValue); }

        // Optional AI query assistant: off by default. Provider ids are
        // AiQueryAssistant::providerIds() ("anthropic", "gemini", "openai_compat"); an empty
        // provider means Anthropic and an empty model the provider's default.
        bool aiAssistantEnabled() const { return _aiAssistantEnabled; }
        void setAiAssistantEnabled(bool enabled) { _aiAssistantEnabled = enabled; _aiHostedProviderRemoved = false; }
        QString aiProvider() const { return _aiProvider; }
        void setAiProvider(const QString &provider) { _aiProvider = provider; _aiHostedProviderRemoved = false; }
        // True after loading settings that chose the removed hosted provider
        // (the assistant was turned off); cleared once the user sets a
        // provider or turns the assistant on again. Not saved.
        bool aiHostedProviderRemoved() const { return _aiHostedProviderRemoved; }
        QString aiModel(const QString &provider) const { return _aiModels.value(provider); }
        void setAiModel(const QString &provider, const QString &model) { _aiModels.insert(provider, model); }
        // For a provider with a configurable base address the key belongs to
        // the origin (scheme, host, port) it was saved for: aiApiKey() is
        // empty once the base address points elsewhere, and setAiBaseUrl()
        // to another origin forgets the key (docs: T1).
        QString aiApiKey(const QString &provider) const;
        void setAiApiKey(const QString &provider, const QString &key);
        // Base address of a provider such as "openai_compat" (normalized by
        // AiQueryAssistant::checkBaseUrl). While none is saved, the
        // provider's default (Ollama: localhost), else empty.
        QString aiBaseUrl(const QString &provider) const;
        void setAiBaseUrl(const QString &provider, const QString &baseUrl);
        // Seconds to wait for an answer (providers with a base address only;
        // AiQueryAssistant::kMinTimeoutSec..kMaxTimeoutSec).
        int aiTimeoutSec(const QString &provider) const;
        void setAiTimeoutSec(const QString &provider, int seconds);
        // Model ids last fetched from the provider (empty: never fetched).
        QStringList aiAvailableModels(const QString &provider) const { return _aiAvailableModels.value(provider); }
        void setAiAvailableModels(const QString &provider, const QStringList &models) { _aiAvailableModels.insert(provider, models); }
        bool aiIncludeSampleValues() const { return _aiIncludeSampleValues; }
        void setAiIncludeSampleValues(bool include) { _aiIncludeSampleValues = include; }
        // Try another model of the same provider when one runs out of quota.
        bool aiAutoFallback() const { return _aiAutoFallback; }
        void setAiAutoFallback(bool enabled) { _aiAutoFallback = enabled; }

        // Background schema collection (docs/SchemaIntelligence.md): off,
        // for the databases in use (default), or every database of a connection.
        enum class SchemaCollectionMode { Off, OnUse, WholeDatabase };
        static constexpr int kDefaultSchemaBackgroundSampleSize = 200;
        static constexpr int kDefaultSchemaBackgroundMaxCollections = 50;
        static QString schemaCollectionModeId(SchemaCollectionMode mode);
        static SchemaCollectionMode schemaCollectionModeFromId(const QString &id);   ///< unknown → OnUse
        SchemaCollectionMode schemaCollectionMode() const { return _schemaCollectionMode; }
        void setSchemaCollectionMode(SchemaCollectionMode mode) { _schemaCollectionMode = mode; }
        int schemaBackgroundSampleSize() const { return _schemaBackgroundSampleSize; }
        void setSchemaBackgroundSampleSize(int size);           ///< clamped to 50..10000
        /// Background analysis waits while the network is metered (mobile data, a phone hotspot).
        bool schemaUnmeteredOnly() const { return _schemaUnmeteredOnly; }
        void setSchemaUnmeteredOnly(bool on) { _schemaUnmeteredOnly = on; }
        int schemaBackgroundMaxCollections() const { return _schemaBackgroundMaxCollections; }
        void setSchemaBackgroundMaxCollections(int count);      ///< per database and session; clamped to 1..1000

        QString anonymousID() const { return _anonymousID; }

        void addCacheData(QString const& key, QVariant const& value);
        QVariant cacheData(QString const& key) const;

        void setProgramExitedNormally(bool value) { _programExitedNormally = value; }
        bool programExitedNormally() const { return _programExitedNormally; }

        /// Cloud sync (docs/SyncClient.md): user configuration only — keys live in KeyStore,
        /// wire state (etags/fingerprints) in sync-state.json owned by SyncEngine.
        bool syncEnabled() const { return _syncEnabled; }
        void setSyncEnabled(bool enabled) { _syncEnabled = enabled; }
        QStringList syncCollections() const { return _syncCollections; }
        void setSyncCollections(const QStringList &collections) { _syncCollections = collections; }
        bool syncIncludePasswords() const { return _syncIncludePasswords; }
        void setSyncIncludePasswords(bool include) { _syncIncludePasswords = include; }
        /// "passphrase" | "device" | "" (unconfigured)
        QString syncKeyMode() const { return _syncKeyMode; }
        void setSyncKeyMode(const QString &mode) { _syncKeyMode = mode; }

        /// Enterprise / self-hosted cloud server configuration (docs/EnterpriseServerProfile.md).
        struct CloudServerConfig
        {
            QString mode = QStringLiteral("default"); ///< "default" | "custom"
            QString apiUrl;
            QString portalUrl;
            QString bridgeUrl;
            QString entitlementPubkeys;
        };

        static QString defaultSystemPolicyFilePath();
        static void setPolicyFilePathForTest(const QString &path);
        static void clearPolicyFilePathForTest();

        bool hasEnterprisePolicy() const { return _hasEnterprisePolicy; }
        bool isEnterprisePolicyLocked() const { return _hasEnterprisePolicy && _policyLocked; }
        bool isEnterprisePolicyInvalid() const { return _hasEnterprisePolicy && _policyInvalid; }
        bool isCloudServerPolicyLocked() const { return _hasEnterprisePolicy && (_policyInvalid || (_policyLocked && _policyHasCloudServer)); }
        bool hasPolicyCloudServer() const { return _hasEnterprisePolicy && !_policyInvalid && _policyHasCloudServer; }
        CloudServerConfig policyCloudServerConfig() const { return _policyCloudServer; }
        bool hasPolicyCheckForUpdates() const { return _hasEnterprisePolicy && !_policyInvalid && _policyHasCheckForUpdates; }
        bool policyCheckForUpdates() const { return _policyCheckForUpdates; }
        bool hasPolicyDisableHttpsFeatures() const { return _hasEnterprisePolicy && !_policyInvalid && _policyHasDisableHttpsFeatures; }
        bool isDisableHttpsFeaturesLocked() const { return _hasEnterprisePolicy && (_policyInvalid || (_policyLocked && _policyHasDisableHttpsFeatures)); }
        bool policyDisableHttpsFeatures() const { return _policyDisableHttpsFeatures; }
        QString policyUpdateUrl() const { return (_hasEnterprisePolicy && !_policyInvalid) ? _policyUpdateUrl : QString(); }

        bool hasUserCloudServerConfig() const { return _userHasCloudServerConfig; }
        CloudServerConfig userCloudServerConfig() const { return _userCloudServer; }
        void setUserCloudServerConfig(const CloudServerConfig &config);
        void clearUserCloudServerConfig();

        // Designed to be set only by human users (or locked enterprise policy.json)
        bool disableHttpsFeatures() const;
        bool debugMode() const { return _debugMode; }

    private:
        void loadEnterprisePolicy();

        /**
         * Load settings from the map. Existing settings will be overwritten.
         */
        void loadFromMap(QVariantMap &map);

        /**
         * Save all settings to map.
         */
        QVariantMap convertToMap() const;

        // The anonymousID stored in the map, or a new one when there is none.
        static QString getOrCreateAnonymousID(QVariantMap const& map);

        /**
         * @brief Version of settings schema currently loaded
         */
        QString _version;

        UUIDEncoding _uuidEncoding;
        SupportedTimes _timeZone;
        ViewMode _viewMode;
        AutocompletionMode _autocompletionMode;
        QString _startupScript;
        bool _autoExpand;
        bool _autoExec;
        bool _minimizeToTray;
        bool _lineNumbers;
        bool _disableConnectionShortcuts;
        bool _programExitedNormally = true;
        bool _disableHttpsFeatures = false;
        bool _userHasDisableHttpsFeatures = false;
        bool _debugMode = false;
        QSet<QString> _acceptedEulaVersions;
        QSet<QString> _dbVersionsConnected;
        int _batchSize;
        bool _checkForUpdates = true;
        bool _userHasCheckForUpdates = false;
        QString _uiTheme = "System";
        QString _uiDensity = "Automatic";
        QString _uiLanguage = "system";
        QString _textFontFamily;
        int _textFontPointSize;

        int _mongoTimeoutSec;
        int _shellTimeoutSec;

        bool writeConfigMap() const;
        bool syncAiKeysToKeyStore() const;
        void clearInMemoryAiKeys();

        bool _aiAssistantEnabled = false;
        bool _aiHostedProviderRemoved = false;
        QString _aiProvider;
        QHash<QString, QString> _aiModels;
        QHash<QString, QString> _aiApiKeys;     // stored in OS KeyStore, never written to ragoshell.json
        QHash<QString, QString> _aiKeyOrigins;  // origin each key was saved for (configurable providers)
        QHash<QString, QString> _aiBaseUrls;
        QHash<QString, int> _aiTimeouts;
        QHash<QString, QStringList> _aiAvailableModels;
        bool _aiIncludeSampleValues = false;
        bool _aiAutoFallback = true;

        SchemaCollectionMode _schemaCollectionMode = SchemaCollectionMode::OnUse;
        int _schemaBackgroundSampleSize = kDefaultSchemaBackgroundSampleSize;
        int _schemaBackgroundMaxCollections = kDefaultSchemaBackgroundMaxCollections;
        bool _schemaUnmeteredOnly = true;

        bool _syncEnabled = false;
        QStringList _syncCollections;
        bool _syncIncludePasswords = false;
        QString _syncKeyMode;

        bool _userHasCloudServerConfig = false;
        CloudServerConfig _userCloudServer;

        bool _hasEnterprisePolicy = false;
        bool _policyLocked = false;
        bool _policyInvalid = false;
        bool _policyHasCloudServer = false;
        CloudServerConfig _policyCloudServer;
        bool _policyHasDisableHttpsFeatures = false;
        bool _policyDisableHttpsFeatures = false;
        bool _policyHasCheckForUpdates = false;
        bool _policyCheckForUpdates = true;
        QString _policyUpdateUrl;

        /**
        * @brief This is an anonymous string taken from QUuid that is generated when RagoShell 
        *        is first installed on a user's machine and then launched for the first time  
        *        It stays the same throughout all upgrades.
        */
        QString _anonymousID;

        // Various cache data
        QMap<QString, QVariant> _cacheData;

        /**
         * @brief List of connections
         */
        static std::vector<ConnectionSettings*> _connections;

        ToolbarSettingsContainerType _toolbars;

        std::shared_ptr<KeyStore> _keyStore;
        bool _legacyAiKeysInFile = false;
    };
}
