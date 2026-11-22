#include "ragoshell/core/settings/SettingsManager.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariantList>
#include <QUuid>
#include <QJsonArray>
#include <QDirIterator>

#include <parser.h>
#include <serializer.h>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/WasmPersist.h"
#include "ragoshell/core/utils/AiQueryAssistant.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/utils/StdUtils.h"
#include "ragoshell/utils/common.h"
#include "ragoshell/utils/RagoCrypt.h"

namespace RagoShell
{
    /**
        * @brief Version of schema
    */
    const QString SchemaVersion = "2.0";

    namespace
    {
        const QStringList AiProviders = AiQueryAssistant::providerIds();

        bool configurableBase(const QString &provider)
        {
            return AiQueryAssistant::hasConfigurableBaseUrl(AiQueryAssistant::providerFromId(provider));
        }

        // Anthropic keeps the keys of the single-provider version ("model",
        // "apiKeyEncrypted"); other providers are prefixed ("geminiModel").
        QString aiSettingName(const QString &provider, const char *key)
        {
            const QString k = QString::fromLatin1(key);
            return provider == QLatin1String("anthropic") ? k : provider + k.left(1).toUpper() + k.mid(1);
        }
    }

    std::vector<ConnectionSettings*>  SettingsManager::_connections;

    QString SettingsManager::getOrCreateAnonymousID(QVariantMap const& map)
    {
        QUuid id = QUuid::fromString(map.value("anonymousID").toString());
        if (id.isNull())
            id = QUuid::createUuid();
        return id.toString(QUuid::WithoutBraces);
    }
    
    /**
     * Creates SettingsManager for config file in default location
     * ~/.ragoshell/ragoshell.json
     */
    SettingsManager::SettingsManager() :
        _version(SchemaVersion),
        _uuidEncoding(DefaultEncoding),
        _timeZone(Utc),
        _viewMode(RagoShell::Tree),
        _autocompletionMode(AutocompleteAll),
        _minimizeToTray(false),
        _lineNumbers(false),
        _disableConnectionShortcuts(false),
        _batchSize(50),
        _textFontFamily(""),
        _textFontPointSize(-1),
        _mongoTimeoutSec(10),
        _shellTimeoutSec(15)
    {
        if (!QDir().mkpath(configDirectory()))
            LOG_MSG("ERROR: Could not create settings path: " + configDirectory(), RagoShell::LogSeverity::Error);

        RagoCrypt::initKey();
        // What initKey() found or did (key store, key file); notify=false: log file only.
        for (const auto &entry : RagoCrypt::ragoCryptLogs())
            LOG_MSG(entry.first, entry.second, false);
        if (!load()) {  // if load fails (probably due to non-existing config. file or directory)
            save();     // create empty settings file
            load();     // read the defaults just written
        }

        LOG_MSG("SettingsManager initialized in " + configFilePath(), RagoShell::LogSeverity::Info, false);
    }

    SettingsManager::~SettingsManager()
    {
        std::for_each(_connections.begin(), _connections.end(), stdutils::default_delete<ConnectionSettings *>());
        _connections.clear();
    }

    QString SettingsManager::schemaVersion()
    {
        return SchemaVersion;
    }

    QString SettingsManager::schemaCollectionModeId(SchemaCollectionMode mode)
    {
        switch (mode) {
        case SchemaCollectionMode::Off:           return QStringLiteral("off");
        case SchemaCollectionMode::WholeDatabase: return QStringLiteral("wholeDatabase");
        case SchemaCollectionMode::OnUse:         break;
        }
        return QStringLiteral("onUse");
    }

    SettingsManager::SchemaCollectionMode SettingsManager::schemaCollectionModeFromId(const QString &id)
    {
        if (id == QLatin1String("off"))
            return SchemaCollectionMode::Off;
        if (id == QLatin1String("wholeDatabase"))
            return SchemaCollectionMode::WholeDatabase;
        return SchemaCollectionMode::OnUse;
    }

    void SettingsManager::setSchemaBackgroundSampleSize(int size)
    {
        _schemaBackgroundSampleSize = std::clamp(size, 50, 10000);
    }

    void SettingsManager::setSchemaBackgroundMaxCollections(int count)
    {
        _schemaBackgroundMaxCollections = std::clamp(count, 1, 1000);
    }

    QString SettingsManager::aiApiKey(const QString &provider) const
    {
        if (!AiQueryAssistant::usesApiKey(AiQueryAssistant::providerFromId(provider)))
            return {};
        const QString key = _aiApiKeys.value(provider);
        if (key.isEmpty() || !configurableBase(provider))
            return key;
        const QString origin = AiQueryAssistant::urlOrigin(_aiBaseUrls.value(provider));
        return !origin.isEmpty() && origin == _aiKeyOrigins.value(provider) ? key : QString();
    }

    void SettingsManager::setAiApiKey(const QString &provider, const QString &key)
    {
        if (!AiQueryAssistant::usesApiKey(AiQueryAssistant::providerFromId(provider))) {
            _aiApiKeys.remove(provider);   // Ollama: never stored
            _aiKeyOrigins.remove(provider);
            return;
        }
        _aiApiKeys.insert(provider, key);
        if (!configurableBase(provider))
            return;
        if (key.isEmpty())
            _aiKeyOrigins.remove(provider);
        else
            _aiKeyOrigins.insert(provider, AiQueryAssistant::urlOrigin(_aiBaseUrls.value(provider)));
    }

    QString SettingsManager::aiBaseUrl(const QString &provider) const
    {
        const QString saved = _aiBaseUrls.value(provider);
        return saved.isEmpty() ? AiQueryAssistant::defaultBaseUrl(AiQueryAssistant::providerFromId(provider)) : saved;
    }

    int SettingsManager::aiTimeoutSec(const QString &provider) const
    {
        const AiQueryAssistant::Provider p = AiQueryAssistant::providerFromId(provider);
        if (!configurableBase(provider) || !_aiTimeouts.contains(provider))
            return AiQueryAssistant::defaultTimeoutSec(p);
        return AiQueryAssistant::clampTimeoutSec(_aiTimeouts.value(provider));
    }

    void SettingsManager::setAiTimeoutSec(const QString &provider, int seconds)
    {
        if (configurableBase(provider))
            _aiTimeouts.insert(provider, AiQueryAssistant::clampTimeoutSec(seconds));
    }

    void SettingsManager::setAiBaseUrl(const QString &provider, const QString &baseUrl)
    {
        if (!configurableBase(provider))
            return;
        const QString origin = AiQueryAssistant::urlOrigin(baseUrl);
        if (origin.isEmpty() || origin != _aiKeyOrigins.value(provider)) {
            // The key was given for another server: never send it here.
            _aiApiKeys.remove(provider);
            _aiKeyOrigins.remove(provider);
        }
        // Listed by the previous server. Before the first save the list came
        // from Test connection on the address being saved now.
        const QString previous = _aiBaseUrls.value(provider);
        if (!previous.isEmpty() && previous != baseUrl)
            _aiAvailableModels.remove(provider);
        _aiBaseUrls.insert(provider, baseUrl);
    }

    SettingsManager::AiProviderMigration SettingsManager::migrateAiProvider(QString &provider,
                                                                           bool &assistantEnabled)
    {
        const QString id = provider.trimmed();
        if (id.isEmpty() || AiQueryAssistant::providerIds().contains(id))
            return AiProviderMigration::None;
        const AiProviderMigration result = id == QLatin1String(AiQueryAssistant::kRemovedHostedProviderId)
                                               ? AiProviderMigration::HostedRemoved
                                               : AiProviderMigration::Unknown;
        provider.clear();
        assistantEnabled = false;
        return result;
    }

    bool SettingsManager::migrateMapToCurrentSchema(QVariantMap &map, QString *previousVersion)
    {
        const QString fileSchema = map.value("version").toString().trimmed();
        if (previousVersion)
            *previousVersion = fileSchema;

        // Fast path: schema model has not changed — keep config untouched.
        if (fileSchema == SchemaVersion)
            return false;

        // ---- Step-by-step schema model normalization (< "2.0" -> "2.0") ----
        auto clampInt = [](const QVariant &v, int minV, int maxV, int defV) {
            bool ok = false;
            const int n = v.toInt(&ok);
            return (!ok || n < minV || n > maxV) ? defV : n;
        };

        map["uuidEncoding"] = clampInt(map.value("uuidEncoding"), 0, 3, 0);
        map["timeZone"] = clampInt(map.value("timeZone"), 0, 1, 0);
        map["viewMode"] = map.contains("viewMode")
                              ? clampInt(map.value("viewMode"), 0, 2, static_cast<int>(RagoShell::Custom))
                              : static_cast<int>(RagoShell::Tree);
        map["autocompletionMode"] = map.contains("autocompletionMode")
                                        ? clampInt(map.value("autocompletionMode"), 0, 2, static_cast<int>(AutocompleteAll))
                                        : static_cast<int>(AutocompleteAll);

        if (!map.contains("autoExpand")) map["autoExpand"] = true;
        if (!map.contains("autoExec")) map["autoExec"] = true;
        if (!map.contains("minimizeToTray")) map["minimizeToTray"] = false;
        if (!map.contains("lineNumbers")) map["lineNumbers"] = false;
        if (!map.contains("disableConnectionShortcuts")) map["disableConnectionShortcuts"] = false;
        if (!map.contains("checkForUpdates")) map["checkForUpdates"] = true;
        if (!map.contains("programExitedNormally")) map["programExitedNormally"] = true;
        if (!map.contains("disableHttpsFeatures")) map["disableHttpsFeatures"] = false;
        if (!map.contains("debugMode")) map["debugMode"] = false;

        if (map.value("batchSize").toInt() <= 0)
            map["batchSize"] = 50;
        if (map.value("mongoTimeoutSec").toInt() <= 0)
            map["mongoTimeoutSec"] = 10;
        if (map.value("shellTimeoutSec").toInt() <= 0)
            map["shellTimeoutSec"] = 15;

        // Migrate legacy 'style' theme hint into 'uiTheme' (2.0 QML theme)
        QString uiTheme = map.value("uiTheme").toString().trimmed();
        if (uiTheme.isEmpty()) {
            const QString legacyStyle = map.value("style").toString();
            if (legacyStyle.contains(QLatin1String("Dark"), Qt::CaseInsensitive))
                uiTheme = QStringLiteral("Dark");
            else if (legacyStyle.contains(QLatin1String("Light"), Qt::CaseInsensitive))
                uiTheme = QStringLiteral("Light");
            else
                uiTheme = QStringLiteral("System");
            map["uiTheme"] = uiTheme;
        }

        if (!map.contains("acceptedEulaVersions"))
            map["acceptedEulaVersions"] = QVariantList{};
        if (!map.contains("dbVersionsConnected"))
            map["dbVersionsConnected"] = QVariantList{};
        if (!map.contains("cacheData"))
            map["cacheData"] = QVariantMap{};

        // Migrate connection entries (UUIDs, legacy SSL keys, plaintext -> RagoCrypt encrypted secrets)
        QVariantList migratedConns;
        const QVariantList rawConns = map.value("connections").toList();
        for (const QVariant &item : rawConns) {
            QVariantMap conn = item.toMap();

            if (conn.value("uuid").toString().trimmed().isEmpty())
                conn["uuid"] = QUuid::createUuid().toString();
            if (!conn.contains("isReplicaSet"))
                conn["isReplicaSet"] = false;

            // Legacy 1.x top-level sslEnabled / sslPemKeyFile -> ssl sub-object
            QVariantMap ssl = conn.value("ssl").toMap();
            if (!conn.contains("ssl") &&
                (conn.contains("sslEnabled") || conn.contains("sslPemKeyFile"))) {
                ssl["sslEnabled"] = conn.value("sslEnabled").toBool();
                const QString legacyPem = conn.value("sslPemKeyFile").toString();
                ssl["pemKeyFile"] = legacyPem;
                ssl["usePemFile"] = !legacyPem.isEmpty();
                conn.remove("sslEnabled");
                conn.remove("sslPemKeyFile");
            }
            if (ssl.contains("pemPassPhrase")) {
                const QString plain = ssl.take("pemPassPhrase").toString();
                if (!ssl.contains("pemPassPhraseEncrypted")) {
                    ssl["pemPassPhraseEncrypted"] = plain.isEmpty()
                        ? QString()
                        : QtUtils::toQString(RagoCrypt::encrypt(QtUtils::toStdString(plain)));
                }
            }
            if (!ssl.isEmpty() || conn.contains("ssl"))
                conn["ssl"] = ssl;

            // SSH plaintext secrets -> encrypted keys
            if (conn.contains("ssh")) {
                QVariantMap ssh = conn.value("ssh").toMap();
                if (ssh.contains("userPassword")) {
                    const QString plain = ssh.take("userPassword").toString();
                    if (!ssh.contains("userPasswordEncrypted")) {
                        ssh["userPasswordEncrypted"] = plain.isEmpty()
                            ? QString()
                            : QtUtils::toQString(RagoCrypt::encrypt(QtUtils::toStdString(plain)));
                    }
                }
                if (ssh.contains("passphrase")) {
                    const QString plain = ssh.take("passphrase").toString();
                    if (!ssh.contains("passphraseEncrypted")) {
                        ssh["passphraseEncrypted"] = plain.isEmpty()
                            ? QString()
                            : QtUtils::toQString(RagoCrypt::encrypt(QtUtils::toStdString(plain)));
                    }
                }
                conn["ssh"] = ssh;
            }

            // Credentials plaintext userPassword -> userPasswordEncrypted
            if (conn.contains("credentials")) {
                QVariantList migratedCreds;
                for (const QVariant &cItem : conn.value("credentials").toList()) {
                    QVariantMap cred = cItem.toMap();
                    if (cred.contains("userPassword")) {
                        const QString plain = cred.take("userPassword").toString();
                        if (!cred.contains("userPasswordEncrypted")) {
                            cred["userPasswordEncrypted"] = plain.isEmpty()
                                ? QString()
                                : QtUtils::toQString(RagoCrypt::encrypt(QtUtils::toStdString(plain)));
                        }
                    }
                    migratedCreds.append(cred);
                }
                conn["credentials"] = migratedCreds;
            }

            migratedConns.append(conn);
        }
        map["connections"] = migratedConns;

        // Stamp migrated schema version
        map["version"] = SchemaVersion;
        return true;
    }

    /**
     * Load settings from config file.
     * @return true if success, false otherwise
     */
    bool SettingsManager::load()
    {
        if (!QFile::exists(configFilePath()))
            return false;

        QFile f(configFilePath());
        if (!f.open(QIODevice::ReadOnly))
            return false;

        bool ok;
        QJson::Parser parser;
        QVariantMap map = parser.parse(f.readAll(), &ok).toMap();
        f.close();
        if (!ok)
            return false;

        QString previousSchema;
        const bool schemaMigrated = migrateMapToCurrentSchema(map, &previousSchema);
        if (schemaMigrated) {
            QString safeTag = previousSchema.isEmpty()
                                  ? QStringLiteral("legacy")
                                  : previousSchema;
            safeTag.replace('/', '_');
            safeTag.replace('\\', '_');
            safeTag.replace(':', '_');
            const QString backupPath = configFilePath() + QStringLiteral(".bak-") + safeTag;
            QFile::remove(backupPath);
            QFile::copy(configFilePath(), backupPath);
        }

        loadFromMap(map);

        if (schemaMigrated) {
            save();
            LOG_MSG(QString("Migrated user config schema from '%1' to '%2'")
                        .arg(previousSchema.isEmpty() ? QStringLiteral("unversioned") : previousSchema,
                             SchemaVersion),
                    RagoShell::LogSeverity::Info, false);
        }

        return true;
    }

    /**
     * Saves all settings to config file.
     * @return true if success, false otherwise
     */
    bool SettingsManager::save()
    {
        QVariantMap const& map = convertToMap();

        QFile f(configFilePath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            LOG_MSG("ERROR: Could not write settings to: " + configFilePath(), RagoShell::LogSeverity::Error);
            return false;
        }

        bool ok;
        QJson::Serializer s;
        s.setIndentMode(QJson::IndentFull);
        s.serialize(map, &f, &ok);

        LOG_MSG("Settings saved to: " + configFilePath(), RagoShell::LogSeverity::Info);

        if (ok)
            requestPersistSync();

        return ok;
    }

    void SettingsManager::addCacheData(QString const& key, QVariant const& value)
    {
        _cacheData.insert(key, value);
    }

    QVariant SettingsManager::cacheData(QString const& key) const
    {
        return _cacheData.value(key);
    }

    /**
     * Load settings from the map. Existings settings will be overwritten.
     */
    void SettingsManager::loadFromMap(QVariantMap &map)
    {
        // 1. Load version
        _version = map.value("version").toString();

        // 2. Load UUID encoding
        int encoding = map.value("uuidEncoding").toInt();
        if (encoding > 3 || encoding < 0)
            encoding = 0;

        _uuidEncoding = (UUIDEncoding)encoding;


        // 3. Load view mode
        if (map.contains("viewMode")) {
            int viewMode = map.value("viewMode").toInt();
            if (viewMode > 2 || viewMode < 0)
                viewMode = Custom; // Default View Mode
            _viewMode = (ViewMode)viewMode;
        }
        else {
            _viewMode = Custom; // Default View Mode
        }

        _autoExpand = map.contains("autoExpand") ? map.value("autoExpand").toBool() : true;
        _autoExec = map.contains("autoExec") ? map.value("autoExec").toBool() : true;
        _minimizeToTray = map.contains("minimizeToTray") ? map.value("minimizeToTray").toBool() : false;
        _lineNumbers = map.contains("lineNumbers") ? map.value("lineNumbers").toBool() : false;
        _programExitedNormally = map.contains("programExitedNormally") ? 
                                 map.value("programExitedNormally").toBool() : true;

        _disableHttpsFeatures = map.contains("disableHttpsFeatures") ? 
                                map.value("disableHttpsFeatures").toBool() : false;

        _debugMode = map.contains("debugMode") ? map.value("debugMode").toBool() : false;

        // 4. Load TimeZone
        int timeZone = map.value("timeZone").toInt();
        if (timeZone > 1 || timeZone < 0)
            timeZone = 0;

        _timeZone = (SupportedTimes)timeZone;
        if (map.contains("startupScript")) {
            _startupScript = map.value("startupScript").toString();
        } else {
            // Older versions had a switch that loaded ~/.mongorc.js (or ~/.mongoshrc.js); keep what it
            // did, once: the file's text becomes the startup script and is saved with the settings.
            _startupScript.clear();
            if (map.value("loadMongoRcJs").toBool()) {
                for (const char *name : {".mongorc.js", ".mongoshrc.js"}) {
                    QFile rc(QDir::home().filePath(QString::fromLatin1(name)));
                    if (rc.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        _startupScript = QString::fromUtf8(rc.readAll());
                        break;
                    }
                }
            }
        }
        _disableConnectionShortcuts = map.value("disableConnectionShortcuts").toBool();
        
        if (map.contains("acceptedEulaVersions")) {
            const QStringList accepted = map.value("acceptedEulaVersions").toStringList();
            _acceptedEulaVersions = QSet<QString>(accepted.begin(), accepted.end());
        }

        if (map.contains("dbVersionsConnected")) {
            const QStringList dbVersions = map.value("dbVersionsConnected").toStringList();
            _dbVersionsConnected = QSet<QString>(dbVersions.begin(), dbVersions.end());
        }
        
        // Load anonymousID
        _anonymousID = getOrCreateAnonymousID(map);

        // Load AutocompletionMode
        if (map.contains("autocompletionMode")) {
            int autocompletionMode = map.value("autocompletionMode").toInt();
            if (autocompletionMode < 0 || autocompletionMode > 2)
                autocompletionMode = AutocompleteAll; // Default Mode
            _autocompletionMode = (AutocompletionMode)autocompletionMode;
        }
        else {
            _autocompletionMode = AutocompleteAll; // Default Mode
        }

        // Load Batch Size
        _batchSize = map.value("batchSize").toInt();
        if (_batchSize <= 0)
            _batchSize = 50;
        _batchSize = std::min(_batchSize, kMaxBatchSize);

        if (map.contains("checkForUpdates"))
            _checkForUpdates = map.value("checkForUpdates").toBool();

        _uiDensity = map.value("uiDensity", QStringLiteral("Automatic")).toString();
        if (_uiDensity != QLatin1String("Compact") && _uiDensity != QLatin1String("Touch"))
            _uiDensity = QStringLiteral("Automatic");
        _uiTheme = map.value("uiTheme").toString();
        if (_uiTheme.isEmpty()) {
            _uiTheme = "System";
        }

        _uiLanguage = map.value("uiLanguage").toString().trimmed();
        if (_uiLanguage.isEmpty())
            _uiLanguage = "system";

        // Load font information
        _textFontFamily = map.value("textFontFamily").toString();
        _textFontPointSize = map.value("textFontPointSize").toInt();

        if (map.contains("mongoTimeoutSec")) {
            _mongoTimeoutSec = map.value("mongoTimeoutSec").toInt();
        }

        if (map.contains("shellTimeoutSec")) {
            _shellTimeoutSec = map.value("shellTimeoutSec").toInt();
        }

        // Optional AI query assistant
        const QVariantMap ai = map.value("aiAssistant").toMap();
        _aiAssistantEnabled = ai.value("enabled", false).toBool();
        _aiProvider = ai.value("provider").toString();
        const QString savedAiProvider = _aiProvider;
        const AiProviderMigration aiMigration = migrateAiProvider(_aiProvider, _aiAssistantEnabled);
        _aiHostedProviderRemoved = aiMigration == AiProviderMigration::HostedRemoved;
        if (aiMigration == AiProviderMigration::HostedRemoved)
            LOG_MSG(QStringLiteral("AI assistant: the hosted AI provider was removed; "
                                   "the assistant is off until a provider and key are set."),
                    LogSeverity::Warning);
        else if (aiMigration == AiProviderMigration::Unknown)
            LOG_MSG(QStringLiteral("AI assistant: unsupported provider \"%1\" in the settings; "
                                   "the assistant is off until a provider and key are set.")
                        .arg(savedAiProvider.left(64)),
                    LogSeverity::Warning);
        _aiModels.clear();
        _aiApiKeys.clear();
        _aiKeyOrigins.clear();
        _aiBaseUrls.clear();
        _aiTimeouts.clear();
        _aiAvailableModels.clear();
        for (const QString &provider : AiProviders) {
            auto name = [&provider](const char *key) { return aiSettingName(provider, key); };
            const QString model = ai.value(name("model")).toString();
            if (!model.isEmpty())
                _aiModels.insert(provider, model);
            const QString aiKey = ai.value(name("apiKeyEncrypted")).toString();
            const QString plain = aiKey.isEmpty() ? QString()
                                  : QtUtils::toQString(RagoCrypt::decrypt(QtUtils::toStdString(aiKey)));
            if (configurableBase(provider)) {
                // A hand-edited address is checked again before any request.
                const QString base = ai.value(name("baseUrl")).toString();
                if (!base.isEmpty())
                    _aiBaseUrls.insert(provider, base);
                const QVariant timeout = ai.value(name("timeoutSec"));
                if (timeout.isValid() && timeout.toInt() > 0)
                    _aiTimeouts.insert(provider, AiQueryAssistant::clampTimeoutSec(timeout.toInt()));
                // "origin\nkey", encrypted together: editing the file cannot
                // move the key to another server (docs: T1).
                const qsizetype split = plain.indexOf(QLatin1Char('\n'));
                if (split > 0 && AiQueryAssistant::usesApiKey(AiQueryAssistant::providerFromId(provider))) {
                    _aiKeyOrigins.insert(provider, plain.left(split));
                    _aiApiKeys.insert(provider, plain.mid(split + 1));
                }
            } else if (!plain.isEmpty()) {
                _aiApiKeys.insert(provider, plain);
            }
            const QStringList models = ai.value(name("availableModels")).toStringList();
            if (!models.isEmpty())
                _aiAvailableModels.insert(provider, models);
        }
        _aiIncludeSampleValues = ai.value("includeSampleValues", false).toBool();
        _aiAutoFallback = ai.value("autoFallback", true).toBool();

        // Cloud sync (docs/SyncClient.md): flags only, no secrets.
        const QVariantMap sync = map.value("sync").toMap();
        _syncEnabled = sync.value("enabled", false).toBool();
        _syncCollections = sync.value("collections").toStringList();
        _syncIncludePasswords = sync.value("includePasswords", false).toBool();
        _syncKeyMode = sync.value("keyMode").toString();
        if (_syncKeyMode != QLatin1String("passphrase") && _syncKeyMode != QLatin1String("device"))
            _syncKeyMode.clear();

        // Background schema collection
        const QVariantMap schema = map.value("schema").toMap();
        _schemaCollectionMode = schemaCollectionModeFromId(schema.value("collectionMode").toString());
        setSchemaBackgroundSampleSize(schema.value("backgroundSampleSize", kDefaultSchemaBackgroundSampleSize).toInt());
        setSchemaBackgroundMaxCollections(
            schema.value("backgroundMaxCollections", kDefaultSchemaBackgroundMaxCollections).toInt());
        _schemaUnmeteredOnly = schema.value("unmeteredOnly", true).toBool();

        // 5. Load connections
        std::for_each(_connections.begin(), _connections.end(),
                      stdutils::default_delete<ConnectionSettings *>());
        _connections.clear();

        QVariantList const& list = map.value("connections").toList();
        for (auto const& conn : list) {
            auto connSettings = new ConnectionSettings(false);
            connSettings->fromVariant(conn.toMap());
            addConnection(connSettings);
        }

        _toolbars = map.value("toolbars").toMap();
        ToolbarSettingsContainerType::const_iterator it = _toolbars.find("connect");
        if (_toolbars.end() == it)
            _toolbars["connect"] = true;

        it = _toolbars.find("open_save");
        if (_toolbars.end() == it)
            _toolbars["open_save"] = true;

        it = _toolbars.find("exec");
        if (_toolbars.end() == it)
            _toolbars["exec"] = true;

        it = _toolbars.find("explorer");
        if (_toolbars.end() == it)
            _toolbars["explorer"] = true;

        it = _toolbars.find("logs");
        if (_toolbars.end() == it)
            _toolbars["logs"] = false;

        _cacheData = map.value("cacheData").toMap();
    }

    /**
     * Save all settings to map.
     */
    QVariantMap SettingsManager::convertToMap() const
    {
        QVariantMap map;

        // 1. Save schema version
        map.insert("version", SchemaVersion);

        // 2. Save UUID encoding
        map.insert("uuidEncoding", _uuidEncoding);

        // 3. Save TimeZone encoding
        map.insert("timeZone", _timeZone);

        // 4. Save view mode
        map.insert("viewMode", _viewMode);
        map.insert("autoExpand", _autoExpand);
        map.insert("lineNumbers", _lineNumbers);

        // 5. Save Autocompletion mode
        map.insert("autocompletionMode", _autocompletionMode);

        // 6. Save the startup script
        map.insert("startupScript", _startupScript);

        // 7. Save disableConnectionShortcuts
        map.insert("disableConnectionShortcuts", _disableConnectionShortcuts);
        
        // 8. Save acceptedEulaVersions array
        QJsonArray arr;
        for (auto const& str : _acceptedEulaVersions)
            arr.push_back(str);

        map.insert("acceptedEulaVersions", arr.toVariantList());

        // x. Save unique set of db versions connected
        QJsonArray dbVersionsArr;
        for (auto const& version : _dbVersionsConnected)
            dbVersionsArr.push_back(version);

        map.insert("dbVersionsConnected", dbVersionsArr.toVariantList());

        // 9. Save batchSize
        map.insert("batchSize", _batchSize);
        map.insert("checkForUpdates", _checkForUpdates);
        map.insert("mongoTimeoutSec", _mongoTimeoutSec);
        map.insert("shellTimeoutSec", _shellTimeoutSec);

        // 10. Save UI appearance & language
        map.insert("uiTheme", _uiTheme);
        map.insert("uiDensity", _uiDensity);
        map.insert("uiLanguage", _uiLanguage);

        // 11. Save font information
        map.insert("textFontFamily", _textFontFamily);
        map.insert("textFontPointSize", _textFontPointSize);

        // 11b. Save AI assistant settings (API key encrypted like connection secrets)
        QVariantMap ai;
        ai.insert("enabled", _aiAssistantEnabled);
        ai.insert("provider", _aiProvider);
        for (const QString &provider : AiProviders) {
            auto name = [&provider](const char *key) { return aiSettingName(provider, key); };
            const QString aiKey = _aiApiKeys.value(provider);
            const QString stored = configurableBase(provider) && !aiKey.isEmpty()
                                       ? _aiKeyOrigins.value(provider) + QLatin1Char('\n') + aiKey
                                       : aiKey;
            ai.insert(name("model"), _aiModels.value(provider));
            ai.insert(name("apiKeyEncrypted"), aiKey.isEmpty() ? QString()
                      : QtUtils::toQString(RagoCrypt::encrypt(QtUtils::toStdString(stored))));
            ai.insert(name("availableModels"), QVariant(_aiAvailableModels.value(provider)));
            if (configurableBase(provider)) {
                ai.insert(name("baseUrl"), _aiBaseUrls.value(provider));
                if (_aiTimeouts.contains(provider))
                    ai.insert(name("timeoutSec"), _aiTimeouts.value(provider));
            }
        }
        ai.insert("includeSampleValues", _aiIncludeSampleValues);
        ai.insert("autoFallback", _aiAutoFallback);
        map.insert("aiAssistant", ai);

        // 11c. Save background schema collection settings
        QVariantMap schema;
        schema.insert("collectionMode", schemaCollectionModeId(_schemaCollectionMode));
        schema.insert("backgroundSampleSize", _schemaBackgroundSampleSize);
        schema.insert("backgroundMaxCollections", _schemaBackgroundMaxCollections);
        schema.insert("unmeteredOnly", _schemaUnmeteredOnly);
        map.insert("schema", schema);

        // 11d. Save cloud sync configuration (flags only; secrets live in KeyStore)
        QVariantMap sync;
        sync.insert("enabled", _syncEnabled);
        sync.insert("collections", _syncCollections);
        sync.insert("includePasswords", _syncIncludePasswords);
        sync.insert("keyMode", _syncKeyMode);
        map.insert("sync", sync);

        // 12. Save connections
        QVariantList list;
        for (auto const conn : _connections) 
            list.append(conn->toVariant().toMap());

        map.insert("connections", list);
        map.insert("autoExec", _autoExec);
        map.insert("minimizeToTray", _minimizeToTray);
        map.insert("toolbars", _toolbars);
        map.insert("anonymousID", _anonymousID);
        map.insert("cacheData", _cacheData);
        map.insert("programExitedNormally", _programExitedNormally);
        map.insert("disableHttpsFeatures", _disableHttpsFeatures);
        map.insert("debugMode", _debugMode);
        
        return map;
    }


    /**
     * Adds connection to the end of list and set it's uniqueID
     */
    void SettingsManager::addConnection(ConnectionSettings *connection)
    {
        _connections.push_back(connection);
    }

    /**
     * Removes connection by index
     */
    void SettingsManager::removeConnection(ConnectionSettings *connection)
    {
        ConnectionSettingsContainerType::iterator it = std::find(_connections.begin(), _connections.end(), connection);
        if (it != _connections.end()) {
            _connections.erase(it);
            delete connection;
        }
    }

    ConnectionSettings* SettingsManager::getConnectionSettingsByUuid(QString const& uuid) const
    {
        for (auto const connSettings : _connections){
            if (connSettings->uuid() == uuid)
                return connSettings;
        }

        LOG_MSG("Failed to find connection settings object by UUID.", RagoShell::LogSeverity::Warning);
        return nullptr;
    }

    ConnectionSettings* SettingsManager::getConnectionSettingsByUuid(std::string const& uuid) const
    {
        return getConnectionSettingsByUuid(QString::fromStdString(uuid));
    }

    void SettingsManager::setTextFontFamily(const QString& fontFamily)
    {
        _textFontFamily = fontFamily;
    }

    void SettingsManager::setTextFontPointSize(int pointSize) {
        _textFontPointSize = pointSize > 0 ? pointSize : -1;
    }

    void SettingsManager::reorderConnections(const ConnectionSettingsContainerType &connections)
    {
        _connections = connections;
    }

    void SettingsManager::setToolbarSettings(const QString toolbarName, const bool visible)
    {
        _toolbars[toolbarName] = visible;
    }

}
