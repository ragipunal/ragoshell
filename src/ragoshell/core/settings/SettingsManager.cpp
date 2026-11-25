#include "ragoshell/core/settings/SettingsManager.h"

#include <algorithm>

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QVariantList>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDirIterator>

#include <openssl/crypto.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <combaseapi.h>
#include <shlobj.h>
#elif defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <parser.h>
#include <serializer.h>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/cloud/EntitlementToken.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/UpdatePulse.h"
#include "ragoshell/core/utils/WasmPersist.h"
#include "ragoshell/core/utils/AiQueryAssistant.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/utils/StdUtils.h"
#include "ragoshell/utils/KeyStore.h"
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

        void cleanseQString(QString &secret)
        {
            if (secret.isEmpty())
                return;
            OPENSSL_cleanse(secret.data(), static_cast<std::size_t>(secret.size()) * sizeof(QChar));
            secret.clear();
        }

        void cleanseStdString(std::string &secret)
        {
            if (secret.empty())
                return;
            OPENSSL_cleanse(secret.data(), secret.size());
            secret.clear();
        }

        QString aiKeyAccount(const QString &provider)
        {
            const QString cleanProvider = provider.trimmed();
            if (appDataDirOverridden()) {
                const QByteArray dirBytes = QDir::cleanPath(appDataDir()).toUtf8();
                const QByteArray digest = QCryptographicHash::hash(dirBytes, QCryptographicHash::Sha256).toHex().left(16);
                return QStringLiteral("ai:%1/%2").arg(QString::fromLatin1(digest), cleanProvider);
            }
            return QStringLiteral("ai/%1").arg(cleanProvider);
        }

        void assignStoredAiKey(const QString &provider,
                               const QString &value,
                               QHash<QString, QString> &keys,
                               QHash<QString, QString> &origins)
        {
            if (configurableBase(provider)) {
                // "origin\nkey", stored together in KeyStore: moving baseUrl never reuses the key (docs: T1).
                const qsizetype split = value.indexOf(QLatin1Char('\n'));
                if (split > 0) {
                    origins.insert(provider, value.left(split));
                    keys.insert(provider, value.mid(split + 1));
                }
            } else if (!value.isEmpty()) {
                keys.insert(provider, value);
            }
        }

        constexpr qint64 kMaxPolicyFileBytes = 65536;

        QString &policyFileOverrideStorage()
        {
            static QString s_path;
            return s_path;
        }

        bool &policyFileOverrideSetStorage()
        {
            static bool s_isSet = false;
            return s_isSet;
        }

        QString stripTrailingSlashesPolicy(const QString &url)
        {
            QString out = url.trimmed();
            while (out.size() > 1 && out.endsWith(QLatin1Char('/')))
                out.chop(1);
            return out;
        }

        enum class PolicyFileReadStatus
        {
            Missing,
            Ok,
            Invalid
        };

        PolicyFileReadStatus readBoundedRegularPolicyFile(const QString &path,
                                                          QByteArray *out,
                                                          QString *error)
        {
            out->clear();
#if defined(Q_OS_UNIX)
            // Open without following symlinks and verify regular file on fd to avoid POSIX TOCTOU.
            const QByteArray encodedPath = QFile::encodeName(path);
            int fd = -1;
            do {
                fd = ::open(encodedPath.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
            } while (fd < 0 && errno == EINTR);
            if (fd < 0) {
                if (errno == ENOENT)
                    return PolicyFileReadStatus::Missing;
                if (error)
                    *error = QStringLiteral("cannot open regular policy file (errno=%1)").arg(errno);
                return PolicyFileReadStatus::Invalid;
            }
            struct stat st {};
            if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                ::close(fd);
                if (error)
                    *error = QStringLiteral("policy path is not a regular file");
                return PolicyFileReadStatus::Invalid;
            }
            if (st.st_size > kMaxPolicyFileBytes) {
                ::close(fd);
                if (error)
                    *error = QStringLiteral("policy file exceeds %1 bytes").arg(kMaxPolicyFileBytes);
                return PolicyFileReadStatus::Invalid;
            }
            char buf[8192];
            while (out->size() <= kMaxPolicyFileBytes) {
                const qint64 want = std::min(static_cast<qint64>(sizeof(buf)),
                                             (kMaxPolicyFileBytes + 1) - out->size());
                ssize_t n = -1;
                do {
                    n = ::read(fd, buf, static_cast<size_t>(want));
                } while (n < 0 && errno == EINTR);
                if (n < 0) {
                    const int readErr = errno;
                    ::close(fd);
                    if (error)
                        *error = QStringLiteral("cannot read policy file (errno=%1)").arg(readErr);
                    return PolicyFileReadStatus::Invalid;
                }
                if (n == 0)
                    break;
                out->append(buf, static_cast<qsizetype>(n));
            }
            ::close(fd);
            if (out->size() > kMaxPolicyFileBytes) {
                if (error)
                    *error = QStringLiteral("policy file exceeds %1 bytes").arg(kMaxPolicyFileBytes);
                return PolicyFileReadStatus::Invalid;
            }
            return PolicyFileReadStatus::Ok;
#else
            const QFileInfo info(path);
            if (info.isSymLink() || info.isJunction()) {
                if (error)
                    *error = QStringLiteral("policy file must not be a symlink");
                return PolicyFileReadStatus::Invalid;
            }
            if (!info.exists())
                return PolicyFileReadStatus::Missing;
            if (!info.isFile()) {
                if (error)
                    *error = QStringLiteral("policy path is not a regular file");
                return PolicyFileReadStatus::Invalid;
            }
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                if (error)
                    *error = QStringLiteral("cannot open policy file: %1").arg(f.errorString());
                return PolicyFileReadStatus::Invalid;
            }
            constexpr qint64 kChunkBytes = 65536;
            while (out->size() <= kMaxPolicyFileBytes) {
                const qint64 want = std::min(kChunkBytes, (kMaxPolicyFileBytes + 1) - out->size());
                const QByteArray chunk = f.read(want);
                if (f.error() != QFileDevice::NoError) {
                    if (error)
                        *error = QStringLiteral("cannot read policy file: %1").arg(f.errorString());
                    return PolicyFileReadStatus::Invalid;
                }
                if (chunk.isEmpty())
                    break;
                out->append(chunk);
            }
            if (out->size() > kMaxPolicyFileBytes) {
                if (error)
                    *error = QStringLiteral("policy file exceeds %1 bytes").arg(kMaxPolicyFileBytes);
                return PolicyFileReadStatus::Invalid;
            }
            return PolicyFileReadStatus::Ok;
#endif
        }
    }

    QString SettingsManager::defaultSystemPolicyFilePath()
    {
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID) || defined(Q_OS_WASM)
        return QString();
#elif defined(Q_OS_WIN)
        PWSTR rawPath = nullptr;
        QString baseDir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &rawPath)) && rawPath)
            baseDir = QString::fromWCharArray(rawPath);
        if (rawPath)
            CoTaskMemFree(rawPath);
        if (baseDir.isEmpty())
            return QString();
        return QDir(baseDir).filePath(QString::fromUtf8(BrandInfo::productName) + QStringLiteral("/policy.json"));
#elif defined(Q_OS_MACOS)
        return QStringLiteral("/Library/Application Support/%1/policy.json")
            .arg(QString::fromUtf8(BrandInfo::productName));
#else
        return QStringLiteral("/etc/%1/policy.json")
            .arg(QString::fromUtf8(BrandInfo::productId));
#endif
    }

    void SettingsManager::setPolicyFilePathForTest(const QString &path)
    {
        policyFileOverrideSetStorage() = true;
        policyFileOverrideStorage() = path;
    }

    void SettingsManager::clearPolicyFilePathForTest()
    {
        policyFileOverrideSetStorage() = false;
        policyFileOverrideStorage().clear();
    }

    void SettingsManager::setCheckForUpdates(bool checkForUpdates)
    {
        if (isCheckForUpdatesLocked())
            return;
        _checkForUpdates = checkForUpdates;
        _userHasCheckForUpdates = true;
    }

    bool SettingsManager::checkForUpdates() const
    {
        if (_policyInvalid)
            return false;
        if (_hasEnterprisePolicy && _policyLocked && _policyHasCheckForUpdates)
            return _policyCheckForUpdates;
        if (_userHasCheckForUpdates)
            return _checkForUpdates;
        if (_hasEnterprisePolicy && !_policyLocked && _policyHasCheckForUpdates)
            return _policyCheckForUpdates;
        return _checkForUpdates;
    }

    bool SettingsManager::isCheckForUpdatesLocked() const
    {
        return _hasEnterprisePolicy && (_policyInvalid || (_policyLocked && _policyHasCheckForUpdates));
    }

    bool SettingsManager::disableHttpsFeatures() const
    {
        if (_hasEnterprisePolicy && !_policyInvalid && _policyLocked && _policyHasDisableHttpsFeatures)
            return _policyDisableHttpsFeatures;
        if (_disableHttpsFeatures || _userHasDisableHttpsFeatures)
            return _disableHttpsFeatures;
        if (_hasEnterprisePolicy && !_policyInvalid && !_policyLocked && _policyHasDisableHttpsFeatures)
            return _policyDisableHttpsFeatures;
        return _disableHttpsFeatures;
    }

    void SettingsManager::setUserCloudServerConfig(const CloudServerConfig &config)
    {
        if (isCloudServerPolicyLocked())
            return;
        CloudServerConfig clean;
        clean.mode = (config.mode.trimmed() == QLatin1String("custom"))
                         ? QStringLiteral("custom")
                         : QStringLiteral("default");
        clean.apiUrl = stripTrailingSlashesPolicy(config.apiUrl);
        clean.portalUrl = stripTrailingSlashesPolicy(config.portalUrl);
        clean.bridgeUrl = stripTrailingSlashesPolicy(config.bridgeUrl);
        clean.entitlementPubkeys = config.entitlementPubkeys.trimmed();
        _userCloudServer = clean;
        _userHasCloudServerConfig = true;
    }

    void SettingsManager::clearUserCloudServerConfig()
    {
        if (isCloudServerPolicyLocked())
            return;
        _userCloudServer = CloudServerConfig{};
        _userHasCloudServerConfig = false;
    }

    void SettingsManager::loadEnterprisePolicy()
    {
        _hasEnterprisePolicy = false;
        _policyLocked = false;
        _policyInvalid = false;
        _policyHasCloudServer = false;
        _policyCloudServer = CloudServerConfig{};
        _policyHasDisableHttpsFeatures = false;
        _policyDisableHttpsFeatures = false;
        _policyHasCheckForUpdates = false;
        _policyCheckForUpdates = true;
        _policyUpdateUrl.clear();

        const QString policyPath = policyFileOverrideSetStorage()
                                       ? policyFileOverrideStorage()
                                       : defaultSystemPolicyFilePath();
        if (policyPath.isEmpty())
            return;

        QByteArray raw;
        QString readErr;
        const PolicyFileReadStatus status = readBoundedRegularPolicyFile(policyPath, &raw, &readErr);
        if (status == PolicyFileReadStatus::Missing)
            return;
        _hasEnterprisePolicy = true;
        if (status == PolicyFileReadStatus::Invalid) {
            _policyLocked = true;
            _policyInvalid = true;
            LOG_MSG(QStringLiteral("Enterprise policy error (%1): %2; cloud features blocked.")
                        .arg(policyPath, readErr),
                    LogSeverity::Error, false);
            return;
        }

        QJsonParseError parseErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            _policyLocked = true;
            _policyInvalid = true;
            LOG_MSG(QStringLiteral("Enterprise policy error (%1): invalid JSON; cloud features blocked.")
                        .arg(policyPath),
                    LogSeverity::Error, false);
            return;
        }

        const QJsonObject obj = doc.object();
        auto markInvalid = [this, &policyPath](const QString &reason) {
            _policyLocked = true;
            _policyInvalid = true;
            LOG_MSG(QStringLiteral("Enterprise policy error (%1): %2; cloud features blocked.")
                        .arg(policyPath, reason),
                    LogSeverity::Error, false);
        };

        if (obj.contains(QStringLiteral("locked"))) {
            const QJsonValue v = obj.value(QStringLiteral("locked"));
            if (!v.isBool()) {
                markInvalid(QStringLiteral("'locked' must be a boolean"));
                return;
            }
            _policyLocked = v.toBool();
        } else {
            _policyLocked = true;
        }

        if (obj.contains(QStringLiteral("disableHttpsFeatures"))) {
            const QJsonValue v = obj.value(QStringLiteral("disableHttpsFeatures"));
            if (!v.isBool()) {
                markInvalid(QStringLiteral("'disableHttpsFeatures' must be a boolean"));
                return;
            }
            _policyHasDisableHttpsFeatures = true;
            _policyDisableHttpsFeatures = v.toBool();
        }

        if (obj.contains(QStringLiteral("checkForUpdates"))) {
            const QJsonValue v = obj.value(QStringLiteral("checkForUpdates"));
            if (!v.isBool()) {
                markInvalid(QStringLiteral("'checkForUpdates' must be a boolean"));
                return;
            }
            _policyHasCheckForUpdates = true;
            _policyCheckForUpdates = v.toBool();
        }

        if (obj.contains(QStringLiteral("updateUrl"))) {
            const QJsonValue v = obj.value(QStringLiteral("updateUrl"));
            if (!v.isString()) {
                markInvalid(QStringLiteral("'updateUrl' must be a string"));
                return;
            }
            const QString u = v.toString().trimmed();
            if (!u.isEmpty() && !UpdatePulse::isSafeEndpointUrl(u)) {
                markInvalid(QStringLiteral("'updateUrl' failed URL security validation"));
                return;
            }
            _policyUpdateUrl = u;
        }

        if (obj.contains(QStringLiteral("cloudServer"))) {
            const QJsonValue csVal = obj.value(QStringLiteral("cloudServer"));
            if (!csVal.isObject()) {
                markInvalid(QStringLiteral("'cloudServer' must be an object"));
                return;
            }
            _policyHasCloudServer = true;
            const QJsonObject cs = csVal.toObject();
            for (const char *key : {"mode", "apiUrl", "portalUrl", "bridgeUrl", "entitlementPubkeys"}) {
                const QString k = QLatin1String(key);
                if (cs.contains(k) && !cs.value(k).isString()) {
                    markInvalid(QStringLiteral("'cloudServer.%1' must be a string").arg(k));
                    return;
                }
            }

            const QString rawMode = cs.value(QStringLiteral("mode")).toString().trimmed();
            const QString mode = rawMode.isEmpty() ? QStringLiteral("default") : rawMode;
            if (mode != QLatin1String("default") && mode != QLatin1String("custom")) {
                markInvalid(QStringLiteral("'cloudServer.mode' must be 'default' or 'custom'"));
                return;
            }

            CloudServerConfig parsed;
            parsed.mode = mode;
            parsed.apiUrl = stripTrailingSlashesPolicy(cs.value(QStringLiteral("apiUrl")).toString());
            parsed.portalUrl = stripTrailingSlashesPolicy(cs.value(QStringLiteral("portalUrl")).toString());
            parsed.bridgeUrl = stripTrailingSlashesPolicy(cs.value(QStringLiteral("bridgeUrl")).toString());
            parsed.entitlementPubkeys = cs.value(QStringLiteral("entitlementPubkeys")).toString().trimmed();

            if (mode == QLatin1String("custom")) {
                if (!CloudClient::isAllowedApiBaseUrl(parsed.apiUrl)) {
                    markInvalid(QStringLiteral("'cloudServer.apiUrl' is invalid or insecure"));
                    return;
                }
                if (!CloudClient::isAllowedApiBaseUrl(parsed.portalUrl)) {
                    markInvalid(QStringLiteral("'cloudServer.portalUrl' is invalid or insecure"));
                    return;
                }
                if (!parsed.bridgeUrl.isEmpty() && !CloudClient::isAllowedBridgeUrl(parsed.bridgeUrl)) {
                    markInvalid(QStringLiteral("'cloudServer.bridgeUrl' is invalid or insecure"));
                    return;
                }
                QString ringErr;
                if (!EntitlementVerifier::isValidCustomKeyRing(parsed.entitlementPubkeys, &ringErr)) {
                    markInvalid(QStringLiteral("'cloudServer.entitlementPubkeys' is invalid (%1)").arg(ringErr));
                    return;
                }
            } else {
                if (!parsed.apiUrl.isEmpty() && !CloudClient::isAllowedApiBaseUrl(parsed.apiUrl)) {
                    markInvalid(QStringLiteral("'cloudServer.apiUrl' is invalid or insecure"));
                    return;
                }
                if (!parsed.portalUrl.isEmpty() && !CloudClient::isAllowedApiBaseUrl(parsed.portalUrl)) {
                    markInvalid(QStringLiteral("'cloudServer.portalUrl' is invalid or insecure"));
                    return;
                }
                if (!parsed.bridgeUrl.isEmpty() && !CloudClient::isAllowedBridgeUrl(parsed.bridgeUrl)) {
                    markInvalid(QStringLiteral("'cloudServer.bridgeUrl' is invalid or insecure"));
                    return;
                }
                if (!parsed.entitlementPubkeys.isEmpty()) {
                    QString ringErr;
                    if (!EntitlementVerifier::isValidCustomKeyRing(parsed.entitlementPubkeys, &ringErr)) {
                        markInvalid(QStringLiteral("'cloudServer.entitlementPubkeys' is invalid (%1)").arg(ringErr));
                        return;
                    }
                }
                const QString defaultApi = stripTrailingSlashesPolicy(QString::fromUtf8(BrandInfo::apiUrl));
                if (!parsed.apiUrl.isEmpty()
                    && parsed.apiUrl.compare(defaultApi, Qt::CaseInsensitive) != 0
                    && parsed.entitlementPubkeys.isEmpty()) {
                    markInvalid(QStringLiteral("'cloudServer.entitlementPubkeys' is required when overriding apiUrl"));
                    return;
                }
            }

            _policyCloudServer = parsed;
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
    SettingsManager::SettingsManager()
        : SettingsManager(std::shared_ptr<KeyStore>(createPlatformKeyStore()))
    {
    }

    SettingsManager::SettingsManager(std::shared_ptr<KeyStore> keyStore) :
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
        _shellTimeoutSec(15),
        _keyStore(std::move(keyStore))
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
        clearInMemoryAiKeys();
        std::for_each(_connections.begin(), _connections.end(), stdutils::default_delete<ConnectionSettings *>());
        _connections.clear();
    }

    void SettingsManager::clearInMemoryAiKeys()
    {
        for (auto it = _aiApiKeys.begin(); it != _aiApiKeys.end(); ++it)
            cleanseQString(it.value());
        _aiApiKeys.clear();
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
        auto eraseExisting = [this, &provider]() {
            auto it = _aiApiKeys.find(provider);
            if (it != _aiApiKeys.end()) {
                cleanseQString(it.value());
                _aiApiKeys.erase(it);
            }
        };
        if (!AiQueryAssistant::usesApiKey(AiQueryAssistant::providerFromId(provider))) {
            eraseExisting();   // Ollama: never stored
            _aiKeyOrigins.remove(provider);
            return;
        }
        eraseExisting();
        if (key.isEmpty()) {
            _aiKeyOrigins.remove(provider);
            return;
        }
        if (configurableBase(provider)) {
            const QString origin = AiQueryAssistant::urlOrigin(_aiBaseUrls.value(provider));
            if (origin.isEmpty()) {
                _aiKeyOrigins.remove(provider);
                return;
            }
            _aiKeyOrigins.insert(provider, origin);
        }
        _aiApiKeys.insert(provider, key);
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
            auto it = _aiApiKeys.find(provider);
            if (it != _aiApiKeys.end()) {
                cleanseQString(it.value());
                _aiApiKeys.erase(it);
            }
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
        loadEnterprisePolicy();

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

        const QVariantMap originalMap = map;
        QString previousSchema;
        const bool schemaMigrated = migrateMapToCurrentSchema(map, &previousSchema);

        loadFromMap(map);

        const bool hadLegacyAiKeys = _legacyAiKeysInFile;
        _legacyAiKeysInFile = false;
        if (schemaMigrated) {
            const bool synced = hadLegacyAiKeys ? syncAiKeysToKeyStore() : true;
            if (hadLegacyAiKeys && !synced) {
                LOG_MSG(QStringLiteral("AI assistant: keeping %1 untouched until the OS credential "
                                       "store is available to migrate existing API key(s).")
                            .arg(configFilePath()),
                        LogSeverity::Warning, false);
                return true;
            }

            QString safeTag = previousSchema.isEmpty()
                                  ? QStringLiteral("legacy")
                                  : previousSchema;
            safeTag.replace('/', '_');
            safeTag.replace('\\', '_');
            safeTag.replace(':', '_');
            const QString backupPath = configFilePath() + QStringLiteral(".bak-") + safeTag;
            QFile::remove(backupPath);
            if (hadLegacyAiKeys) {
                QVariantMap scrubbedBackup = originalMap;
                QVariantMap backupAi = scrubbedBackup.value(QStringLiteral("aiAssistant")).toMap();
                for (const QString &provider : AiProviders)
                    backupAi.remove(aiSettingName(provider, "apiKeyEncrypted"));
                scrubbedBackup.insert(QStringLiteral("aiAssistant"), backupAi);
                QFile bak(backupPath);
                if (bak.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    bak.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                    bool bakOk = false;
                    QJson::Serializer s;
                    s.setIndentMode(QJson::IndentFull);
                    s.serialize(scrubbedBackup, &bak, &bakOk);
                }
            } else {
                QFile::copy(configFilePath(), backupPath);
                QFile::setPermissions(backupPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            }

            if (!hadLegacyAiKeys)
                syncAiKeysToKeyStore();
            writeConfigMap();
            LOG_MSG(QString("Migrated user config schema from '%1' to '%2'")
                        .arg(previousSchema.isEmpty() ? QStringLiteral("unversioned") : previousSchema,
                             SchemaVersion),
                    RagoShell::LogSeverity::Info, false);
            if (hadLegacyAiKeys)
                LOG_MSG(QStringLiteral("AI assistant: migrated API key(s) out of %1.")
                            .arg(configFilePath()),
                        LogSeverity::Info, false);
        } else if (hadLegacyAiKeys) {
            if (syncAiKeysToKeyStore()) {
                writeConfigMap();
                LOG_MSG(QStringLiteral("AI assistant: migrated API key(s) out of %1.")
                            .arg(configFilePath()),
                        LogSeverity::Info, false);
            } else {
                LOG_MSG(QStringLiteral("AI assistant: keeping %1 untouched until the OS credential "
                                       "store is available to migrate existing API key(s).")
                            .arg(configFilePath()),
                        LogSeverity::Warning, false);
            }
        }

        return true;
    }

    bool SettingsManager::writeConfigMap() const
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

    bool SettingsManager::syncAiKeysToKeyStore() const
    {
        if (!_keyStore || !_keyStore->isAvailable()) {
            if (!_aiApiKeys.isEmpty()) {
                LOG_MSG(QStringLiteral("AI assistant: no OS credential store is available; API keys "
                                       "are kept in memory for this session only."),
                        LogSeverity::Warning, false);
            }
            return false;
        }

        bool allOk = true;
        for (const QString &provider : AiProviders) {
            if (!AiQueryAssistant::usesApiKey(AiQueryAssistant::providerFromId(provider)))
                continue;
            const QString key = _aiApiKeys.value(provider);
            const QString origin = _aiKeyOrigins.value(provider);
            const bool validEntry = !key.isEmpty()
                                    && (!configurableBase(provider) || !origin.isEmpty());
            if (!validEntry) {
                QString err;
                if (!_keyStore->remove(aiKeyAccount(provider), &err)) {
                    allOk = false;
                    LOG_MSG(QStringLiteral("AI assistant: could not remove %1 API key from %2 (%3).")
                                .arg(provider, _keyStore->name(), err),
                            LogSeverity::Warning, false);
                }
                continue;
            }
            QString payload = configurableBase(provider)
                                  ? (origin + QLatin1Char('\n') + key)
                                  : key;
            QString err;
            if (!_keyStore->write(aiKeyAccount(provider), payload, &err)) {
                allOk = false;
                LOG_MSG(QStringLiteral("AI assistant: could not store %1 API key in %2 (%3).")
                            .arg(provider, _keyStore->name(), err),
                        LogSeverity::Warning, false);
            }
            cleanseQString(payload);
        }
        return allOk;
    }

    /**
     * Saves all settings to config file.
     * @return true if success, false otherwise
     */
    bool SettingsManager::save()
    {
        syncAiKeysToKeyStore();
        return writeConfigMap();
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
        _userHasDisableHttpsFeatures = map.value("disableHttpsFeaturesUserSet").toBool()
                                       || (map.contains("disableHttpsFeatures") && _disableHttpsFeatures);

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

        _checkForUpdates = true;
        _userHasCheckForUpdates = false;
        if (map.contains("checkForUpdates")) {
            _checkForUpdates = map.value("checkForUpdates").toBool();
            if (map.value("checkForUpdatesUserSet").toBool() || !_checkForUpdates)
                _userHasCheckForUpdates = true;
        }

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
        clearInMemoryAiKeys();
        _aiKeyOrigins.clear();
        _legacyAiKeysInFile = false;
        _aiBaseUrls.clear();
        _aiTimeouts.clear();
        _aiAvailableModels.clear();
        for (const QString &provider : AiProviders) {
            auto name = [&provider](const char *key) { return aiSettingName(provider, key); };
            const QString model = ai.value(name("model")).toString();
            if (!model.isEmpty())
                _aiModels.insert(provider, model);
            const bool usesKey = AiQueryAssistant::usesApiKey(AiQueryAssistant::providerFromId(provider));
            if (configurableBase(provider)) {
                // A hand-edited address is checked again before any request.
                const QString base = ai.value(name("baseUrl")).toString();
                if (!base.isEmpty())
                    _aiBaseUrls.insert(provider, base);
                const QVariant timeout = ai.value(name("timeoutSec"));
                if (timeout.isValid() && timeout.toInt() > 0)
                    _aiTimeouts.insert(provider, AiQueryAssistant::clampTimeoutSec(timeout.toInt()));
            }
            QString stored;
            if (usesKey && _keyStore && _keyStore->isAvailable()) {
                if (_keyStore->read(aiKeyAccount(provider), &stored, nullptr) != KeyStore::Result::Ok)
                    cleanseQString(stored);
            }
            if (!stored.isEmpty()) {
                assignStoredAiKey(provider, stored, _aiApiKeys, _aiKeyOrigins);
                cleanseQString(stored);
            }
            const QString legacyEnc = ai.value(name("apiKeyEncrypted")).toString();
            if (!legacyEnc.isEmpty()) {
                _legacyAiKeysInFile = true;
                if (usesKey && _aiApiKeys.value(provider).isEmpty()) {
                    std::string dec = RagoCrypt::decrypt(QtUtils::toStdString(legacyEnc));
                    QString plain = QtUtils::toQString(dec);
                    cleanseStdString(dec);
                    if (!plain.isEmpty())
                        assignStoredAiKey(provider, plain, _aiApiKeys, _aiKeyOrigins);
                    cleanseQString(plain);
                }
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

        // Enterprise / self-hosted cloud server profile (docs/EnterpriseServerProfile.md).
        _userHasCloudServerConfig = map.contains("cloudServer");
        _userCloudServer = CloudServerConfig{};
        if (_userHasCloudServerConfig) {
            const QVariantMap cs = map.value("cloudServer").toMap();
            const QString mode = cs.value("mode").toString().trimmed();
            _userCloudServer.mode = (mode == QLatin1String("custom"))
                                        ? QStringLiteral("custom")
                                        : QStringLiteral("default");
            _userCloudServer.apiUrl = stripTrailingSlashesPolicy(cs.value("apiUrl").toString());
            _userCloudServer.portalUrl = stripTrailingSlashesPolicy(cs.value("portalUrl").toString());
            _userCloudServer.bridgeUrl = stripTrailingSlashesPolicy(cs.value("bridgeUrl").toString());
            _userCloudServer.entitlementPubkeys = cs.value("entitlementPubkeys").toString().trimmed();
        }

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
        if (_userHasCheckForUpdates)
            map.insert("checkForUpdatesUserSet", true);
        map.insert("mongoTimeoutSec", _mongoTimeoutSec);
        map.insert("shellTimeoutSec", _shellTimeoutSec);

        // 10. Save UI appearance & language
        map.insert("uiTheme", _uiTheme);
        map.insert("uiDensity", _uiDensity);
        map.insert("uiLanguage", _uiLanguage);

        // 11. Save font information
        map.insert("textFontFamily", _textFontFamily);
        map.insert("textFontPointSize", _textFontPointSize);

        // 11b. Save AI assistant settings (API keys live in KeyStore, never ragoshell.json)
        QVariantMap ai;
        ai.insert("enabled", _aiAssistantEnabled);
        ai.insert("provider", _aiProvider);
        for (const QString &provider : AiProviders) {
            auto name = [&provider](const char *key) { return aiSettingName(provider, key); };
            ai.insert(name("model"), _aiModels.value(provider));
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

        if (_userHasCloudServerConfig) {
            QVariantMap cs;
            cs.insert("mode", _userCloudServer.mode);
            cs.insert("apiUrl", _userCloudServer.apiUrl);
            cs.insert("portalUrl", _userCloudServer.portalUrl);
            cs.insert("bridgeUrl", _userCloudServer.bridgeUrl);
            cs.insert("entitlementPubkeys", _userCloudServer.entitlementPubkeys);
            map.insert("cloudServer", cs);
        }

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
        if (_userHasDisableHttpsFeatures)
            map.insert("disableHttpsFeaturesUserSet", true);
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
