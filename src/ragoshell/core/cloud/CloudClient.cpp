#include "ragoshell/core/cloud/CloudClient.h"

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/utils/KeyStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUrl>

#include <openssl/crypto.h>

#include <algorithm>
#include <cstdint>

namespace RagoShell
{
namespace
{
    constexpr int kDefaultTimeoutMs = 15000;
    constexpr int kLogoutTimeoutMs = 5000;
    constexpr qint64 kMaxApiResponseBytes = 65536;
    constexpr qint64 kAccessTokenRefreshSkewSec = 30;

    QString stripTrailingSlashes(const QString &url)
    {
        QString out = url.trimmed();
        while (out.size() > 1 && out.endsWith(QLatin1Char('/')))
            out.chop(1);
        return out;
    }

    bool isLoopbackHost(const QString &hostLower)
    {
        return hostLower == QLatin1String("127.0.0.1")
               || hostLower == QLatin1String("localhost")
               || hostLower == QLatin1String("::1");
    }

    bool hasControlOrBackslash(const QString &s)
    {
        for (const QChar ch : s)
        {
            const ushort u = ch.unicode();
            if (u <= 0x20 || u == 0x7f || u == '\\')
                return true;
        }
        return false;
    }

    bool isSafeUserCode(const QString &code)
    {
        if (code.isEmpty() || code.size() > 32)
            return false;
        static const QRegularExpression kUserCodeRe(QStringLiteral("^[A-Za-z0-9 -]{4,32}$"));
        return kUserCodeRe.match(code).hasMatch();
    }

    QString currentClientVersionString(const QString &overrideVer)
    {
        if (!overrideVer.trimmed().isEmpty())
            return overrideVer.trimmed().left(32);
        const QString envVer = productEnv("APP_VERSION_OVERRIDE").trimmed();
        if (!envVer.isEmpty())
            return envVer.left(32);
#ifdef PROJECT_VERSION
        return QStringLiteral(PROJECT_VERSION);
#else
        return QStringLiteral("2.1.0");
#endif
    }
} // namespace

// ---------------------------------------------------------------------------
// KeyStoreCloudCredentialStore
// ---------------------------------------------------------------------------

KeyStoreCloudCredentialStore::KeyStoreCloudCredentialStore()
    : m_keyStore(createPlatformKeyStore())
{
}

KeyStoreCloudCredentialStore::KeyStoreCloudCredentialStore(std::unique_ptr<KeyStore> keyStore)
    : m_keyStore(keyStore ? std::move(keyStore) : createPlatformKeyStore())
{
}

KeyStoreCloudCredentialStore::~KeyStoreCloudCredentialStore() = default;

bool KeyStoreCloudCredentialStore::isAvailable() const
{
    return m_keyStore && m_keyStore->isAvailable();
}

QString KeyStoreCloudCredentialStore::accountNameForSlot(const QString &slot)
{
    const QString cleanSlot = slot.trimmed();
    if (appDataDirOverridden())
    {
        const QByteArray dirBytes = QDir::cleanPath(appDataDir()).toUtf8();
        const QByteArray digest = QCryptographicHash::hash(dirBytes, QCryptographicHash::Sha256).toHex().left(16);
        return QStringLiteral("cloud:%1/%2").arg(QString::fromLatin1(digest), cleanSlot);
    }
    return QStringLiteral("cloud/%1").arg(cleanSlot);
}

bool KeyStoreCloudCredentialStore::readSecret(const QString &slot, QString *valueOut, QString *errorOut)
{
    if (!m_keyStore || !m_keyStore->isAvailable())
    {
        if (errorOut)
            *errorOut = QStringLiteral("OS credential store is unavailable.");
        return false;
    }
    QString raw;
    const KeyStore::Result res = m_keyStore->read(accountNameForSlot(slot), &raw, errorOut);
    if (res == KeyStore::Result::Error)
    {
        if (!raw.isEmpty())
            OPENSSL_cleanse(raw.data(), static_cast<std::size_t>(raw.size()) * sizeof(QChar));
        return false;
    }
    if (valueOut)
        *valueOut = (res == KeyStore::Result::Ok) ? std::move(raw) : QString();
    if (!raw.isEmpty())
        OPENSSL_cleanse(raw.data(), static_cast<std::size_t>(raw.size()) * sizeof(QChar));
    return true;
}

bool KeyStoreCloudCredentialStore::writeSecret(const QString &slot, const QString &value, QString *errorOut)
{
    if (!m_keyStore || !m_keyStore->isAvailable())
    {
        if (errorOut)
            *errorOut = QStringLiteral("OS credential store is unavailable.");
        return false;
    }
    return m_keyStore->write(accountNameForSlot(slot), value, errorOut);
}

bool KeyStoreCloudCredentialStore::removeSecret(const QString &slot, QString *errorOut)
{
    if (!m_keyStore || !m_keyStore->isAvailable())
        return true;
    return m_keyStore->remove(accountNameForSlot(slot), errorOut);
}

// ---------------------------------------------------------------------------
// InMemoryCloudCredentialStore
// ---------------------------------------------------------------------------

bool InMemoryCloudCredentialStore::readSecret(const QString &slot, QString *valueOut, QString *errorOut)
{
    if (!m_available)
    {
        if (errorOut)
            *errorOut = QStringLiteral("In-memory credential store disabled.");
        return false;
    }
    if (valueOut)
        *valueOut = m_secrets.value(slot);
    return true;
}

bool InMemoryCloudCredentialStore::writeSecret(const QString &slot, const QString &value, QString *errorOut)
{
    if (!m_available)
    {
        if (errorOut)
            *errorOut = QStringLiteral("In-memory credential store disabled.");
        return false;
    }
    m_secrets.insert(slot, value);
    return true;
}

bool InMemoryCloudCredentialStore::removeSecret(const QString &slot, QString *errorOut)
{
    Q_UNUSED(errorOut);
    if (m_secrets.contains(slot))
    {
        QString val = m_secrets.take(slot);
        if (!val.isEmpty())
            OPENSSL_cleanse(val.data(), static_cast<std::size_t>(val.size()) * sizeof(QChar));
    }
    return true;
}

// ---------------------------------------------------------------------------
// CloudClient
// ---------------------------------------------------------------------------

CloudClient::CloudClient(std::shared_ptr<ICloudCredentialStore> credentialStore,
                         SettingsManager *settings,
                         QObject *parent)
    : QObject(parent),
      m_credentialStore(credentialStore ? std::move(credentialStore)
                                        : std::make_shared<KeyStoreCloudCredentialStore>()),
      m_settings(settings)
{
    m_metadata.deviceName = defaultDeviceName();

    m_pollTimer.setSingleShot(true);
    connect(&m_pollTimer, &QTimer::timeout, this, &CloudClient::pollDeviceToken);

    m_countdownTimer.setInterval(1000);
    connect(&m_countdownTimer, &QTimer::timeout, this, &CloudClient::updateCountdownTick);

    m_autoRefreshTimer.setInterval(kAutoRefreshCheckMs);
    connect(&m_autoRefreshTimer, &QTimer::timeout, this, &CloudClient::checkEntitlementRefresh);
    // A fresh token from any path (sign-in, manual or automatic refresh) clears the failure backoff.
    connect(this, &CloudClient::entitlementsUpdated, this, [this]() {
        m_autoRefreshBackoffSec = 0;
        m_autoRefreshNextAttemptAt = 0;
    });

    reloadVerifierFromConfig();
}

void CloudClient::setAutoRefreshEnabled(bool enabled)
{
    if (m_autoRefreshEnabled == enabled)
        return;
    m_autoRefreshEnabled = enabled;
    if (!enabled)
    {
        m_autoRefreshTimer.stop();
        return;
    }
    m_autoRefreshTimer.start();
    QTimer::singleShot(kAutoRefreshFirstCheckMs, this, &CloudClient::checkEntitlementRefresh);
}

void CloudClient::checkEntitlementRefresh()
{
    if (!m_autoRefreshEnabled || !m_metadata.signedIn || m_authState != CloudAuthState::SignedIn)
        return;
    const qint64 now = nowUnixSec();

    // A token verified as ValidOnline stays so in memory until re-verified: once its exp has passed it must
    // not be offered to the hosted gateway any more (the gateway would close the session at once).
    if (m_entitlementState == EntitlementState::ValidOnline && m_claims.expiresAt > 0 && now >= m_claims.expiresAt)
    {
        verifyAndApplyCachedEntitlementOffline();
        emit stateChanged();
    }

    if (m_settings && m_settings->disableHttpsFeatures())
        return;

    if (m_busy || m_refreshInFlight || m_meReply || m_entitlementsReply)
        return;
    if (m_autoRefreshNextAttemptAt > 0 && now < m_autoRefreshNextAttemptAt)
        return;

    const bool usable = m_entitlementState == EntitlementState::ValidOnline
                        && m_claims.issuedAt > 0 && m_claims.expiresAt > m_claims.issuedAt;
    if (usable && now < m_claims.issuedAt + (m_claims.expiresAt - m_claims.issuedAt) / 2)
        return;

    // Until entitlementsUpdated resets it, the next attempt waits 5 min, then 10, 20, 40, 60 (cap).
    m_autoRefreshBackoffSec = m_autoRefreshBackoffSec <= 0
                                  ? kAutoRefreshMinBackoffSec
                                  : std::min(m_autoRefreshBackoffSec * 2, kAutoRefreshMaxBackoffSec);
    m_autoRefreshNextAttemptAt = now + m_autoRefreshBackoffSec;
    refreshAccountAndEntitlements();
}

CloudClient::~CloudClient()
{
    m_pollTimer.stop();
    m_countdownTimer.stop();
    m_autoRefreshTimer.stop();
    cleanseString(&m_accessToken);
    cleanseString(&m_volatileRefreshToken);
    cleanseString(&m_deviceCode);
}

void CloudClient::cleanseString(QString *secret)
{
    if (!secret || secret->isEmpty())
        return;
    OPENSSL_cleanse(secret->data(), static_cast<std::size_t>(secret->size()) * sizeof(QChar));
    secret->clear();
}

void CloudClient::cleanseByteArray(QByteArray *secret)
{
    if (!secret || secret->isEmpty())
        return;
    OPENSSL_cleanse(secret->data(), static_cast<std::size_t>(secret->size()));
    secret->clear();
}

qint64 CloudClient::nowUnixSec() const
{
    if (m_clockSecFn)
        return m_clockSecFn();
    return QDateTime::currentSecsSinceEpoch();
}

bool CloudClient::keyStoreAvailable() const
{
    return m_credentialStore && m_credentialStore->isAvailable();
}

QString CloudClient::defaultDeviceName()
{
    const QString host = sanitizeDeviceName(QSysInfo::machineHostName());
    return host.isEmpty() ? QStringLiteral("Workstation") : host;
}

QString CloudClient::sanitizeDeviceName(const QString &rawName)
{
    const QList<uint> codePoints = rawName.trimmed().toUcs4();
    QString out;
    out.reserve(std::min<qsizetype>(rawName.size(), 128));
    int count = 0;
    int utf8Bytes = 0;

    for (const uint cp : codePoints)
    {
        if (cp < 0x20U || (cp >= 0x7fU && cp <= 0x9fU) || cp > 0x10ffffU)
            continue;
        const QChar::Category cat = QChar::category(cp);
        if (cat == QChar::Other_Control
            || cat == QChar::Other_Format
            || cat == QChar::Other_Surrogate
            || cat == QChar::Other_NotAssigned
            || cat == QChar::Separator_Line
            || cat == QChar::Separator_Paragraph)
        {
            continue;
        }

        const QString piece = QString::fromUcs4(&cp, 1);
        const int pieceBytes = piece.toUtf8().size();
        if (count >= 128 || (utf8Bytes + pieceBytes) > 512)
            break;

        out.append(piece);
        ++count;
        utf8Bytes += pieceBytes;
    }
    return out.trimmed();
}

QString CloudClient::normalizedPlatformId(const QString &rawOs)
{
    const QString lower = rawOs.trimmed().toLower();
    if (lower == QLatin1String("osx") || lower == QLatin1String("macos") || lower == QLatin1String("darwin"))
        return QStringLiteral("macos");
    if (lower == QLatin1String("windows") || lower == QLatin1String("winnt"))
        return QStringLiteral("windows");
    if (lower == QLatin1String("ios") || lower == QLatin1String("tvos"))
        return QStringLiteral("ios");
    if (lower == QLatin1String("android"))
        return QStringLiteral("android");
    if (lower == QLatin1String("wasm") || lower == QLatin1String("emscripten"))
        return QStringLiteral("wasm");
    return QStringLiteral("linux");
}

bool CloudClient::isAllowedApiBaseUrl(const QString &urlText)
{
    const QString trimmed = stripTrailingSlashes(urlText);
    if (trimmed.isEmpty() || trimmed.size() > 512 || hasControlOrBackslash(trimmed))
        return false;

    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return false;

    const QString host = url.host().toLower();
    if (host.isEmpty())
        return false;

    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("https"))
        return true;
    if (scheme == QLatin1String("http") && isLoopbackHost(host))
        return true;
    return false;
}

bool CloudClient::isAllowedVerificationUrl(const QString &candidateUrl,
                                           const QString &effectiveApiBaseUrl,
                                           const QString &effectivePortalBaseUrl)
{
    const QString trimmed = candidateUrl.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 512 || hasControlOrBackslash(trimmed))
        return false;

    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment())
        return false;

    const QString path = url.path();
    if (path.contains(QStringLiteral("..")) || path.contains(QStringLiteral("//")))
        return false;

    const QString candScheme = url.scheme().toLower();
    const QString candHost = url.host().toLower();
    if (candHost.isEmpty())
        return false;

    const QUrl portalUrl(stripTrailingSlashes(effectivePortalBaseUrl), QUrl::StrictMode);
    const QString portalHost = portalUrl.host().toLower();
    const int portalPort = portalUrl.port(-1);

    const QUrl apiUrl(stripTrailingSlashes(effectiveApiBaseUrl), QUrl::StrictMode);
    const QString apiScheme = apiUrl.scheme().toLower();
    const QString apiHost = apiUrl.host().toLower();

    if (candScheme == QLatin1String("https") && !portalHost.isEmpty() && candHost == portalHost)
    {
        const int candPort = url.port(-1);
        return portalPort == -1 ? (candPort == -1 || candPort == 443) : (candPort == portalPort);
    }

    if (apiScheme == QLatin1String("http") && isLoopbackHost(apiHost))
    {
        const int candPort = url.port(80);
        if (candScheme == QLatin1String("http") && candHost == apiHost && candPort == apiUrl.port(80))
            return true;
        if (candScheme == QLatin1String("http")
            && portalUrl.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0
            && !portalHost.isEmpty()
            && isLoopbackHost(portalHost)
            && candHost == portalHost
            && candPort == portalUrl.port(80))
            return true;
    }

    return false;
}

QString CloudClient::effectiveApiUrl() const
{
    if (!m_apiUrlOverride.isEmpty() && isAllowedApiBaseUrl(m_apiUrlOverride))
        return stripTrailingSlashes(m_apiUrlOverride);

    const QString envUrl = productEnv("API_URL").trimmed();
    if (!envUrl.isEmpty() && isAllowedApiBaseUrl(envUrl))
        return stripTrailingSlashes(envUrl);

    return stripTrailingSlashes(QString::fromUtf8(BrandInfo::apiUrl));
}

QString CloudClient::effectivePortalUrl() const
{
    if (!m_portalUrlOverride.isEmpty() && isAllowedApiBaseUrl(m_portalUrlOverride))
        return stripTrailingSlashes(m_portalUrlOverride);

    const QString envUrl = productEnv("PORTAL_URL").trimmed();
    if (!envUrl.isEmpty() && isAllowedApiBaseUrl(envUrl))
        return stripTrailingSlashes(envUrl);

    return stripTrailingSlashes(QString::fromUtf8(BrandInfo::portalUrl));
}

void CloudClient::setApiUrlOverride(const QString &apiUrl)
{
    m_apiUrlOverride = stripTrailingSlashes(apiUrl);
}

void CloudClient::setPortalUrlOverride(const QString &portalUrl)
{
    m_portalUrlOverride = stripTrailingSlashes(portalUrl);
}

void CloudClient::setPlatformOverride(const QString &platformId, const QString &arch, const QString &appVersion)
{
    m_platformOverride = platformId.trimmed();
    m_archOverride = arch.trimmed();
    m_appVersionOverride = appVersion.trimmed();
}

void CloudClient::setEntitlementVerifier(const EntitlementVerifier &verifier)
{
    m_verifier = verifier;
    m_customVerifierSet = true;
}

void CloudClient::setClockForTest(ClockFn clockSecondsFn)
{
    m_clockSecFn = std::move(clockSecondsFn);
}

void CloudClient::setMinPollIntervalMsForTest(int minIntervalMs, int slowDownStepMs)
{
    m_minPollIntervalMs = std::max(10, minIntervalMs);
    m_slowDownStepMs = std::max(20, slowDownStepMs);
}

void CloudClient::expireAccessTokenInMemoryForTest()
{
    m_accessTokenExpiresAtSec = 0;
}

void CloudClient::reloadVerifierFromConfig()
{
    if (m_customVerifierSet)
        return;

    const QString envRing = productEnv("ENTITLEMENT_PUBKEYS").trimmed();
    if (!envRing.isEmpty())
    {
        bool ok = false;
        QString err;
        EntitlementVerifier parsed = EntitlementVerifier::fromKeyRingString(envRing, &ok, &err);
        if (ok)
        {
            m_verifier = std::move(parsed);
            return;
        }
    }

    m_verifier = EntitlementVerifier::fromEmbeddedDefaults();
}

void CloudClient::setDeviceName(const QString &name)
{
    const QString clean = sanitizeDeviceName(name);
    const QString next = clean.isEmpty() ? defaultDeviceName() : clean;
    if (m_metadata.deviceName == next)
        return;
    m_metadata.deviceName = next;
    saveMetadataToSettings();
    emit stateChanged();
}

int CloudClient::codeRemainingSeconds() const
{
    if (m_authState != CloudAuthState::AwaitingBrowserApproval || m_codeExpiresAtSec <= 0)
        return 0;
    const qint64 rem = m_codeExpiresAtSec - nowUnixSec();
    return static_cast<int>(std::max<qint64>(0, rem));
}

void CloudClient::loadMetadataFromSettings()
{
    if (!m_settings)
        return;
    m_metadata.signedIn = m_settings->cacheData(QStringLiteral("cloudSignedIn")).toBool();
    m_metadata.deviceId = m_settings->cacheData(QStringLiteral("cloudDeviceId")).toString().trimmed();
    const QString storedDevName = sanitizeDeviceName(
        m_settings->cacheData(QStringLiteral("cloudDeviceName")).toString());
    m_metadata.deviceName = storedDevName.isEmpty() ? defaultDeviceName() : storedDevName;
    m_metadata.userId = m_settings->cacheData(QStringLiteral("cloudUserId")).toString().trimmed();
    m_metadata.userEmail = m_settings->cacheData(QStringLiteral("cloudUserEmail")).toString().trimmed();
}

void CloudClient::saveMetadataToSettings()
{
    if (!m_settings)
        return;
    const bool persistSession = m_metadata.signedIn && keyStoreAvailable();
    m_settings->addCacheData(QStringLiteral("cloudSignedIn"), persistSession);
    m_settings->addCacheData(QStringLiteral("cloudDeviceId"), persistSession ? m_metadata.deviceId : QString());
    m_settings->addCacheData(QStringLiteral("cloudDeviceName"), m_metadata.deviceName);
    m_settings->addCacheData(QStringLiteral("cloudUserId"), persistSession ? m_metadata.userId : QString());
    m_settings->addCacheData(QStringLiteral("cloudUserEmail"), persistSession ? m_metadata.userEmail : QString());
    m_settings->save();
}

void CloudClient::verifyAndApplyCachedEntitlementOffline()
{
    m_entitlementState = EntitlementState::Invalid;
    m_claims = EntitlementClaims{};

    if (!m_metadata.signedIn || !m_credentialStore || !m_credentialStore->isAvailable())
        return;

    QString cachedJwt;
    if (!m_credentialStore->readSecret(QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot), &cachedJwt)
        || cachedJwt.trimmed().isEmpty())
    {
        return;
    }

    reloadVerifierFromConfig();
    if (m_verifier.keyCount() == 0)
        return;

    EntitlementVerifyOptions opts;
    opts.expectedIssuer = effectiveApiUrl();
    opts.expectedDeviceId = m_metadata.deviceId;

    const EntitlementVerifyResult res = m_verifier.verify(cachedJwt.trimmed(), nowUnixSec(), opts);
    if (!res.isUsable())
        return;

    if (!m_metadata.userId.isEmpty()
        && res.claims.subject.compare(m_metadata.userId, Qt::CaseInsensitive) != 0)
    {
        return;
    }

    m_entitlementState = res.state;
    m_claims = res.claims;
}

bool CloudClient::hasActiveOnlineEntitlement() const
{
    if (m_settings && m_settings->disableHttpsFeatures())
        return false;
    return m_entitlementState == EntitlementState::ValidOnline
           && m_claims.expiresAt > 0
           && nowUnixSec() < m_claims.expiresAt;
}

QString CloudClient::entitlementJwt() const
{
    // Hosted gateway hello token (docs/HostedGateway.md §5): the cached
    // entitlement+jwt, returned only while ValidOnline and unexpired — the
    // gateway denies grace/expired states at hello, so a stale token must
    // never leave the KeyStore.
    if (!m_credentialStore || !m_credentialStore->isAvailable()
        || !hasActiveOnlineEntitlement())
        return QString();
    QString jwt;
    if (!m_credentialStore->readSecret(
            QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot), &jwt))
        return QString();
    return jwt.trimmed();
}

void CloudClient::restoreOfflineSession()
{
    loadMetadataFromSettings();
    if (!m_metadata.signedIn)
    {
        m_authState = CloudAuthState::SignedOut;
        m_entitlementState = EntitlementState::Invalid;
        m_claims = EntitlementClaims{};
        emit stateChanged();
        return;
    }

    // Without a usable OS KeyStore, sessions are strictly in-memory for the running process only.
    if (!keyStoreAvailable())
    {
        m_metadata.signedIn = false;
        m_metadata.deviceId.clear();
        m_metadata.userId.clear();
        m_metadata.userEmail.clear();
        saveMetadataToSettings();
        m_authState = CloudAuthState::SignedOut;
        m_entitlementState = EntitlementState::Invalid;
        m_claims = EntitlementClaims{};
        emit stateChanged();
        return;
    }

    // Verify that a refresh token still exists in KeyStore; if explicitly deleted (NotFound),
    // sign out cleanly, whereas transient KeyStore read errors preserve metadata until unlocked.
    QString existingRefresh;
    const bool readOk = m_credentialStore->readSecret(
        QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), &existingRefresh);
    if (readOk && existingRefresh.isEmpty())
    {
        m_metadata.signedIn = false;
        m_metadata.deviceId.clear();
        m_metadata.userId.clear();
        m_metadata.userEmail.clear();
        saveMetadataToSettings();
        m_authState = CloudAuthState::SignedOut;
        emit stateChanged();
        return;
    }
    cleanseString(&existingRefresh);

    m_authState = CloudAuthState::SignedIn;
    verifyAndApplyCachedEntitlementOffline();
    emit stateChanged();
}

QNetworkRequest CloudClient::buildApiRequest(const QString &path, bool jsonBody, int timeoutMs) const
{
    const QString fullUrl = effectiveApiUrl() + path;
    QNetworkRequest req{QUrl(fullUrl)};
    if (jsonBody)
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("%1/%2")
                      .arg(QString::fromUtf8(BrandInfo::productName),
                           currentClientVersionString(m_appVersionOverride)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(0);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setTransferTimeout(timeoutMs);
    return req;
}

CloudClient::ApiErrorInfo CloudClient::parseApiError(const QByteArray &body, int httpStatus)
{
    ApiErrorInfo info;
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error == QJsonParseError::NoError && doc.isObject())
    {
        const QJsonObject root = doc.object();
        const QJsonValue errVal = root.value(QStringLiteral("error"));
        if (errVal.isObject())
        {
            const QJsonObject errObj = errVal.toObject();
            info.code = errObj.value(QStringLiteral("code")).toString().trimmed();
            info.message = errObj.value(QStringLiteral("message")).toString().trimmed();
        }
        else if (errVal.isString())
        {
            info.code = errVal.toString().trimmed();
            info.message = root.value(QStringLiteral("error_description")).toString().trimmed();
            if (info.message.isEmpty())
                info.message = root.value(QStringLiteral("message")).toString().trimmed();
        }
    }

    if (info.code.isEmpty())
    {
        if (httpStatus == 401)
            info.code = QStringLiteral("unauthorized");
        else if (httpStatus == 429)
            info.code = QStringLiteral("rate_limited");
        else if (httpStatus >= 500)
            info.code = QStringLiteral("server_error");
        else
            info.code = QStringLiteral("network_error");
    }
    return info;
}

void CloudClient::clearPendingDeviceFlow(bool cleanseCode)
{
    m_pollTimer.stop();
    m_countdownTimer.stop();
    if (m_deviceFlowReply)
    {
        m_deviceFlowReply->disconnect(this);
        m_deviceFlowReply->abort();
        m_deviceFlowReply->deleteLater();
        m_deviceFlowReply = nullptr;
    }
    if (cleanseCode)
        cleanseString(&m_deviceCode);
    m_userCode.clear();
    m_verificationUri.clear();
    m_verificationUriComplete.clear();
    m_codeExpiresAtSec = 0;
}

void CloudClient::startDeviceSignIn(const QString &customDeviceName)
{
    if (m_settings && m_settings->disableHttpsFeatures())
    {
        m_lastErrorCode = QStringLiteral("https_disabled");
        m_lastErrorMessage = QStringLiteral("HTTPS cloud features are disabled by settings.");
        emit stateChanged();
        emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
        return;
    }

    clearPendingDeviceFlow(true);
    m_lastErrorCode.clear();
    m_lastErrorMessage.clear();

    if (!customDeviceName.trimmed().isEmpty())
        setDeviceName(customDeviceName);

    const QString apiBase = effectiveApiUrl();
    if (!isAllowedApiBaseUrl(apiBase))
    {
        m_lastErrorCode = QStringLiteral("unsafe_api_url");
        m_lastErrorMessage = QStringLiteral("Configured cloud API URL is invalid or insecure.");
        emit stateChanged();
        emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
        return;
    }

    m_authState = CloudAuthState::RequestingCode;
    m_busy = true;
    emit stateChanged();

    const QString plat = normalizedPlatformId(
        m_platformOverride.isEmpty() ? QSysInfo::productType() : m_platformOverride);
    const QString arch = (m_archOverride.isEmpty()
                              ? QSysInfo::currentCpuArchitecture()
                              : m_archOverride)
                             .trimmed()
                             .left(32);

    QJsonObject reqObj;
    reqObj.insert(QStringLiteral("device_name"),
                  m_metadata.deviceName.isEmpty() ? defaultDeviceName() : m_metadata.deviceName);
    reqObj.insert(QStringLiteral("platform"), plat);
    reqObj.insert(QStringLiteral("arch"), arch.isEmpty() ? QStringLiteral("unknown") : arch);
    reqObj.insert(QStringLiteral("app_version"), currentClientVersionString(m_appVersionOverride));

    const QByteArray payload = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam.post(buildApiRequest(QStringLiteral("/v1/auth/device/code"), true), payload);
    m_deviceFlowReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxApiResponseBytes)
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_deviceFlowReply != reply)
            return;
        m_deviceFlowReply = nullptr;
        m_busy = false;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status != 200 || body.size() > kMaxApiResponseBytes)
        {
            const ApiErrorInfo err = parseApiError(body, status);
            cleanseByteArray(&body);
            m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
            m_lastErrorCode = err.code;
            m_lastErrorMessage = err.message.isEmpty()
                ? QStringLiteral("Could not start device sign-in.")
                : err.message;
            emit stateChanged();
            emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
            return;
        }

        QJsonParseError jsonErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &jsonErr);
        cleanseByteArray(&body);
        if (jsonErr.error != QJsonParseError::NoError || !doc.isObject())
        {
            m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
            m_lastErrorCode = QStringLiteral("invalid_response");
            m_lastErrorMessage = QStringLiteral("Invalid response from cloud sign-in service.");
            emit stateChanged();
            emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
            return;
        }

        const QJsonObject obj = doc.object();
        QString devCode = obj.value(QStringLiteral("device_code")).toString().trimmed();
        const QString usrCode = obj.value(QStringLiteral("user_code")).toString().trimmed();
        const QString verUri = obj.value(QStringLiteral("verification_uri")).toString().trimmed();
        QString verUriComplete = obj.value(QStringLiteral("verification_uri_complete")).toString().trimmed();
        if (verUriComplete.isEmpty())
            verUriComplete = verUri;

        const QString apiBaseNow = effectiveApiUrl();
        const QString portalBaseNow = effectivePortalUrl();
        if (devCode.isEmpty() || devCode.size() > 512
            || !isSafeUserCode(usrCode)
            || !isAllowedVerificationUrl(verUri, apiBaseNow, portalBaseNow)
            || !isAllowedVerificationUrl(verUriComplete, apiBaseNow, portalBaseNow))
        {
            cleanseString(&devCode);
            m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
            m_lastErrorCode = QStringLiteral("unsafe_verification_uri");
            m_lastErrorMessage = QStringLiteral("Refused untrusted verification URL from sign-in response.");
            emit stateChanged();
            emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
            return;
        }

        const int expiresInSec = std::clamp(obj.value(QStringLiteral("expires_in")).toInt(600), 10, 1800);
        const int minIntervalSec = (m_minPollIntervalMs < 1000) ? 0 : 1;
        const int rawIntervalSec = std::clamp(obj.value(QStringLiteral("interval")).toInt(5), minIntervalSec, 60);

        cleanseString(&m_deviceCode);
        m_deviceCode = std::move(devCode);
        cleanseString(&devCode);
        m_userCode = usrCode;
        m_verificationUri = verUri;
        m_verificationUriComplete = verUriComplete;
        m_codeExpiresAtSec = nowUnixSec() + expiresInSec;
        m_pollIntervalMs = std::max(m_minPollIntervalMs, rawIntervalSec * 1000);

        m_authState = CloudAuthState::AwaitingBrowserApproval;
        m_countdownTimer.start();
        m_pollTimer.start(m_pollIntervalMs);
        emit stateChanged();
    });
}

void CloudClient::cancelDeviceSignIn()
{
    if (m_authState != CloudAuthState::RequestingCode
        && m_authState != CloudAuthState::AwaitingBrowserApproval)
    {
        return;
    }
    clearPendingDeviceFlow(true);
    m_busy = false;
    m_lastErrorCode.clear();
    m_lastErrorMessage.clear();
    m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
    emit stateChanged();
}

void CloudClient::updateCountdownTick()
{
    if (m_authState != CloudAuthState::AwaitingBrowserApproval)
    {
        m_countdownTimer.stop();
        return;
    }
    if (codeRemainingSeconds() <= 0)
    {
        clearPendingDeviceFlow(true);
        m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
        m_lastErrorCode = QStringLiteral("expired_token");
        m_lastErrorMessage = QStringLiteral("The sign-in code has expired. Please start again.");
        emit stateChanged();
        emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
        return;
    }
    emit stateChanged();
}

void CloudClient::pollDeviceToken()
{
    if (m_authState != CloudAuthState::AwaitingBrowserApproval || m_deviceCode.isEmpty())
        return;

    if (codeRemainingSeconds() <= 0)
    {
        updateCountdownTick();
        return;
    }

    QJsonObject reqObj;
    reqObj.insert(QStringLiteral("grant_type"),
                  QStringLiteral("urn:ietf:params:oauth:grant-type:device_code"));
    reqObj.insert(QStringLiteral("device_code"), m_deviceCode);

    QByteArray payload = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam.post(buildApiRequest(QStringLiteral("/v1/auth/device/token"), true), payload);
    cleanseByteArray(&payload);
    m_deviceFlowReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxApiResponseBytes)
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_deviceFlowReply != reply)
            return;
        m_deviceFlowReply = nullptr;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = reply->readAll();

        if (reply->error() == QNetworkReply::NoError && status == 200 && body.size() <= kMaxApiResponseBytes)
        {
            QJsonParseError jsonErr{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &jsonErr);
            cleanseByteArray(&body);

            if (jsonErr.error != QJsonParseError::NoError || !doc.isObject())
            {
                clearPendingDeviceFlow(true);
                m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
                m_lastErrorCode = QStringLiteral("invalid_response");
                m_lastErrorMessage = QStringLiteral("Malformed token response from cloud service.");
                emit stateChanged();
                emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
                return;
            }

            const QJsonObject obj = doc.object();
            QString accessTok = obj.value(QStringLiteral("access_token")).toString().trimmed();
            QString refreshTok = obj.value(QStringLiteral("refresh_token")).toString().trimmed();
            const QString devId = obj.value(QStringLiteral("device_id")).toString().trimmed().toLower();
            const int expiresIn = std::clamp(obj.value(QStringLiteral("expires_in")).toInt(900), 60, 3600);

            if (accessTok.isEmpty() || refreshTok.isEmpty() || !EntitlementVerifier::isCanonicalUuid(devId))
            {
                cleanseString(&accessTok);
                cleanseString(&refreshTok);
                clearPendingDeviceFlow(true);
                m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
                m_lastErrorCode = QStringLiteral("invalid_response");
                m_lastErrorMessage = QStringLiteral("Incomplete token payload from cloud service.");
                emit stateChanged();
                emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
                return;
            }

            clearPendingDeviceFlow(true);

            // Reset any prior account's identity/entitlement state before binding the new session.
            m_metadata.userId.clear();
            m_metadata.userEmail.clear();
            m_claims = EntitlementClaims{};
            m_entitlementState = EntitlementState::Invalid;
            if (m_credentialStore && m_credentialStore->isAvailable())
                m_credentialStore->removeSecret(QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot));

            QString storeErr;
            if (keyStoreAvailable())
            {
                cleanseString(&m_volatileRefreshToken);
                if (!m_credentialStore->writeSecret(
                        QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), refreshTok, &storeErr))
                {
                    m_lastErrorCode = QStringLiteral("keystore_error");
                    m_lastErrorMessage = storeErr;
                }
            }
            else
            {
                cleanseString(&m_volatileRefreshToken);
                m_volatileRefreshToken = std::move(refreshTok);
            }
            cleanseString(&refreshTok);

            cleanseString(&m_accessToken);
            m_accessToken = std::move(accessTok);
            cleanseString(&accessTok);
            m_accessTokenExpiresAtSec = nowUnixSec() + expiresIn;

            m_metadata.signedIn = true;
            m_metadata.deviceId = devId;
            saveMetadataToSettings();

            m_authState = CloudAuthState::SignedIn;
            emit stateChanged();
            emit signInSucceeded();

            refreshAccountAndEntitlements();
            return;
        }

        const ApiErrorInfo err = parseApiError(body, status);
        cleanseByteArray(&body);

        if (status == 400 && err.code == QLatin1String("authorization_pending"))
        {
            m_pollTimer.start(m_pollIntervalMs);
            return;
        }

        if (status == 400 && err.code == QLatin1String("slow_down"))
        {
            m_pollIntervalMs = std::min(m_pollIntervalMs + m_slowDownStepMs, 60000);
            m_pollTimer.start(m_pollIntervalMs);
            emit stateChanged();
            return;
        }

        if (status == 429)
        {
            const int retryAfterSec = std::clamp(
                QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed().toInt(), 5, 120);
            m_pollTimer.start(retryAfterSec * 1000);
            return;
        }

        if (status == 400 && (err.code == QLatin1String("access_denied")
                              || err.code == QLatin1String("expired_token")
                              || err.code == QLatin1String("invalid_grant")))
        {
            clearPendingDeviceFlow(true);
            m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
            m_lastErrorCode = err.code;
            m_lastErrorMessage = err.message.isEmpty()
                ? QStringLiteral("Device sign-in was cancelled or expired.")
                : err.message;
            emit stateChanged();
            emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
            return;
        }

        // Transient network / 5xx failure while code is still valid: retry with capped backoff.
        if (status == 0 || status >= 500)
        {
            m_pollTimer.start(std::min(m_pollIntervalMs * 2, 30000));
            return;
        }

        clearPendingDeviceFlow(true);
        m_authState = m_metadata.signedIn ? CloudAuthState::SignedIn : CloudAuthState::SignedOut;
        m_lastErrorCode = err.code;
        m_lastErrorMessage = err.message;
        emit stateChanged();
        emit signInFailed(m_lastErrorCode, m_lastErrorMessage);
    });
}

void CloudClient::withValidAccessToken(AccessTokenCallback callback)
{
    if (!callback)
        return;

    if (m_settings && m_settings->disableHttpsFeatures())
    {
        callback(false, QString(), QStringLiteral("https_disabled"));
        return;
    }

    if (!m_metadata.signedIn || m_authState != CloudAuthState::SignedIn)
    {
        callback(false, QString(), QStringLiteral("not_signed_in"));
        return;
    }

    if (!m_accessToken.isEmpty() && (nowUnixSec() + kAccessTokenRefreshSkewSec) < m_accessTokenExpiresAtSec)
    {
        callback(true, m_accessToken, QString());
        return;
    }

    m_tokenWaiters.push_back(std::move(callback));
    if (m_refreshInFlight)
        return;

    performTokenRefresh();
}

void CloudClient::performTokenRefresh()
{
    m_refreshInFlight = true;

    QString refreshTok;
    if (keyStoreAvailable())
    {
        QString readErr;
        const bool readOk = m_credentialStore->readSecret(
            QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), &refreshTok, &readErr);
        if (!readOk)
        {
            cleanseString(&refreshTok);
            m_refreshInFlight = false;
            m_lastErrorCode = QStringLiteral("keystore_error");
            m_lastErrorMessage = readErr;
            emit stateChanged();
            finishTokenWaiters(false, QString(), m_lastErrorCode);
            return;
        }
    }
    else
    {
        refreshTok = m_volatileRefreshToken;
    }

    if (refreshTok.trimmed().isEmpty())
    {
        cleanseString(&refreshTok);
        m_refreshInFlight = false;
        handleSessionRevoked(QStringLiteral("missing_refresh_token"),
                             QStringLiteral("Session expired. Please sign in again."));
        finishTokenWaiters(false, QString(), QStringLiteral("missing_refresh_token"));
        return;
    }

    QJsonObject reqObj;
    reqObj.insert(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    reqObj.insert(QStringLiteral("refresh_token"), refreshTok.trimmed());
    cleanseString(&refreshTok);

    QByteArray payload = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam.post(buildApiRequest(QStringLiteral("/v1/auth/refresh"), true), payload);
    cleanseByteArray(&payload);
    m_refreshReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxApiResponseBytes)
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_refreshReply != reply)
            return;
        m_refreshReply = nullptr;
        m_refreshInFlight = false;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = reply->readAll();

        if (reply->error() == QNetworkReply::NoError && status == 200 && body.size() <= kMaxApiResponseBytes)
        {
            QJsonParseError jsonErr{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &jsonErr);
            cleanseByteArray(&body);

            if (jsonErr.error == QJsonParseError::NoError && doc.isObject())
            {
                const QJsonObject obj = doc.object();
                QString newAccess = obj.value(QStringLiteral("access_token")).toString().trimmed();
                QString newRefresh = obj.value(QStringLiteral("refresh_token")).toString().trimmed();
                const QString devId = obj.value(QStringLiteral("device_id")).toString().trimmed().toLower();
                const int expiresIn = std::clamp(obj.value(QStringLiteral("expires_in")).toInt(900), 60, 3600);

                if (!newAccess.isEmpty() && !newRefresh.isEmpty())
                {
                    if (keyStoreAvailable())
                    {
                        cleanseString(&m_volatileRefreshToken);
                        m_credentialStore->writeSecret(
                            QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), newRefresh);
                    }
                    else
                    {
                        cleanseString(&m_volatileRefreshToken);
                        m_volatileRefreshToken = std::move(newRefresh);
                    }
                    cleanseString(&newRefresh);

                    cleanseString(&m_accessToken);
                    m_accessToken = std::move(newAccess);
                    cleanseString(&newAccess);
                    m_accessTokenExpiresAtSec = nowUnixSec() + expiresIn;

                    if (!devId.isEmpty() && EntitlementVerifier::isCanonicalUuid(devId))
                    {
                        m_metadata.deviceId = devId;
                        saveMetadataToSettings();
                    }

                    finishTokenWaiters(true, m_accessToken, QString());
                    return;
                }
                cleanseString(&newAccess);
                cleanseString(&newRefresh);
            }

            finishTokenWaiters(false, QString(), QStringLiteral("invalid_response"));
            return;
        }

        const ApiErrorInfo err = parseApiError(body, status);
        cleanseByteArray(&body);

        // 400 invalid_grant or 401 unauthorized means the refresh token was revoked or rotated away.
        if (status == 401 || (status == 400 && err.code == QLatin1String("invalid_grant")))
        {
            handleSessionRevoked(err.code.isEmpty() ? QStringLiteral("invalid_grant") : err.code,
                                 err.message.isEmpty()
                                     ? QStringLiteral("Your cloud session has ended. Local connections remain available.")
                                     : err.message);
            finishTokenWaiters(false, QString(), err.code);
            return;
        }

        // Transient offline / 5xx error: keep cached refresh_token and offline grace entitlement intact!
        verifyAndApplyCachedEntitlementOffline();
        m_lastErrorCode = err.code;
        m_lastErrorMessage = err.message;
        emit stateChanged();
        finishTokenWaiters(false, QString(), err.code);
    });
}

void CloudClient::finishTokenWaiters(bool ok, const QString &accessToken, const QString &errorCode)
{
    std::vector<AccessTokenCallback> waiters;
    waiters.swap(m_tokenWaiters);
    for (auto &cb : waiters)
    {
        if (cb)
            cb(ok, accessToken, errorCode);
    }
}

void CloudClient::handleSessionRevoked(const QString &reasonCode, const QString &message)
{
    clearPendingDeviceFlow(true);
    cleanseString(&m_accessToken);
    cleanseString(&m_volatileRefreshToken);
    m_accessTokenExpiresAtSec = 0;

    if (m_credentialStore)
    {
        m_credentialStore->removeSecret(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot));
        m_credentialStore->removeSecret(QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot));
    }

    m_metadata.signedIn = false;
    m_metadata.deviceId.clear();
    m_metadata.userId.clear();
    m_metadata.userEmail.clear();
    saveMetadataToSettings();

    m_authState = CloudAuthState::SignedOut;
    m_entitlementState = EntitlementState::Invalid;
    m_claims = EntitlementClaims{};
    m_busy = false;
    m_autoRefreshBackoffSec = 0;
    m_autoRefreshNextAttemptAt = 0;
    m_lastErrorCode = reasonCode;
    m_lastErrorMessage = message;

    emit stateChanged();
    emit sessionRevoked(reasonCode);
}

void CloudClient::refreshAccountAndEntitlements()
{
    if (m_settings && m_settings->disableHttpsFeatures())
        return;

    if (!m_metadata.signedIn || m_authState != CloudAuthState::SignedIn)
        return;

    m_busy = true;
    m_lastErrorCode.clear();
    m_lastErrorMessage.clear();
    emit stateChanged();

    withValidAccessToken([this](bool ok, const QString &accessToken, const QString &errCode) {
        if (!ok)
        {
            m_busy = false;
            if (m_lastErrorCode.isEmpty())
                m_lastErrorCode = errCode;
            emit stateChanged();
            return;
        }
        fetchMeProfile(accessToken, /*allowRefreshRetry=*/true);
    });
}

void CloudClient::fetchMeProfile(const QString &accessToken, bool allowRefreshRetry)
{
    if (!m_metadata.signedIn || m_authState != CloudAuthState::SignedIn)
        return;

    QNetworkRequest req = buildApiRequest(QStringLiteral("/v1/me"), false);
    req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());

    if (m_meReply)
    {
        m_meReply->disconnect(this);
        m_meReply->abort();
        m_meReply->deleteLater();
    }

    QNetworkReply *reply = m_nam.get(req);
    m_meReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxApiResponseBytes)
            reply->abort();
    });

    QString tokenCopy = accessToken;
    connect(reply, &QNetworkReply::finished, this, [this, reply, tokenCopy = std::move(tokenCopy), allowRefreshRetry]() mutable {
        reply->deleteLater();
        if (m_meReply != reply)
        {
            cleanseString(&tokenCopy);
            return;
        }
        m_meReply = nullptr;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (status == 401)
        {
            cleanseString(&tokenCopy);
            cleanseString(&m_accessToken);
            m_accessTokenExpiresAtSec = 0;
            if (allowRefreshRetry)
            {
                withValidAccessToken([this](bool ok, const QString &freshToken, const QString &) {
                    if (!ok)
                    {
                        m_busy = false;
                        emit stateChanged();
                        return;
                    }
                    fetchMeProfile(freshToken, /*allowRefreshRetry=*/false);
                });
                return;
            }
            handleSessionRevoked(QStringLiteral("invalid_token"),
                                 QStringLiteral("Your device session is no longer active."));
            return;
        }

        if (reply->error() == QNetworkReply::NoError && status == 200 && body.size() <= kMaxApiResponseBytes)
        {
            QJsonParseError jsonErr{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &jsonErr);
            if (jsonErr.error == QJsonParseError::NoError && doc.isObject())
            {
                const QJsonObject obj = doc.object();
                const QString uid = obj.value(QStringLiteral("id")).toString().trimmed().toLower();
                const QString email = obj.value(QStringLiteral("email")).toString().trimmed();
                if (!uid.isEmpty())
                    m_metadata.userId = uid;
                if (!email.isEmpty())
                    m_metadata.userEmail = email;
                saveMetadataToSettings();
            }
        }

        fetchEntitlementsWithToken(tokenCopy, allowRefreshRetry);
        cleanseString(&tokenCopy);
    });
}

void CloudClient::fetchEntitlementsWithToken(const QString &accessToken, bool allowRefreshRetry)
{
    if (!m_metadata.signedIn || m_authState != CloudAuthState::SignedIn)
        return;

    QNetworkRequest req = buildApiRequest(QStringLiteral("/v1/me/entitlements"), false);
    req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());

    if (m_entitlementsReply)
    {
        m_entitlementsReply->disconnect(this);
        m_entitlementsReply->abort();
        m_entitlementsReply->deleteLater();
    }

    QNetworkReply *reply = m_nam.get(req);
    m_entitlementsReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxApiResponseBytes)
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, allowRefreshRetry]() {
        reply->deleteLater();
        if (m_entitlementsReply != reply)
            return;
        m_entitlementsReply = nullptr;
        m_busy = false;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (status == 401)
        {
            cleanseString(&m_accessToken);
            m_accessTokenExpiresAtSec = 0;
            if (allowRefreshRetry)
            {
                m_busy = true;
                withValidAccessToken([this](bool ok, const QString &freshToken, const QString &) {
                    if (!ok)
                    {
                        m_busy = false;
                        emit stateChanged();
                        return;
                    }
                    fetchEntitlementsWithToken(freshToken, /*allowRefreshRetry=*/false);
                });
                return;
            }
            handleSessionRevoked(QStringLiteral("invalid_token"),
                                 QStringLiteral("Your device session is no longer active."));
            return;
        }

        if (reply->error() != QNetworkReply::NoError || status != 200 || body.size() > kMaxApiResponseBytes)
        {
            const ApiErrorInfo err = parseApiError(body, status);
            verifyAndApplyCachedEntitlementOffline();
            m_lastErrorCode = err.code;
            m_lastErrorMessage = err.message;
            emit stateChanged();
            return;
        }

        QJsonParseError jsonErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &jsonErr);
        if (jsonErr.error != QJsonParseError::NoError || !doc.isObject())
        {
            m_lastErrorCode = QStringLiteral("invalid_response");
            m_lastErrorMessage = QStringLiteral("Invalid entitlement response from cloud service.");
            emit stateChanged();
            return;
        }

        const QJsonObject obj = doc.object();
        QString jwt = obj.value(QStringLiteral("token")).toString().trimmed();
        if (jwt.isEmpty())
            jwt = obj.value(QStringLiteral("entitlement_token")).toString().trimmed();

        reloadVerifierFromConfig();
        if (m_verifier.keyCount() == 0)
        {
            m_entitlementState = EntitlementState::Invalid;
            m_claims = EntitlementClaims{};
            emit stateChanged();
            emit entitlementsUpdated();
            return;
        }

        EntitlementVerifyOptions opts;
        opts.expectedIssuer = effectiveApiUrl();
        opts.expectedDeviceId = m_metadata.deviceId;

        const EntitlementVerifyResult verified = m_verifier.verify(jwt, nowUnixSec(), opts);
        if (!verified.isUsable())
        {
            m_entitlementState = EntitlementState::Invalid;
            m_claims = EntitlementClaims{};
            m_lastErrorCode = verified.errorCode.isEmpty()
                ? QStringLiteral("invalid_entitlement_signature")
                : verified.errorCode;
            m_lastErrorMessage = QStringLiteral("Entitlement signature verification failed.");
            emit stateChanged();
            return;
        }

        if (!m_metadata.userId.isEmpty()
            && verified.claims.subject.compare(m_metadata.userId, Qt::CaseInsensitive) != 0)
        {
            m_entitlementState = EntitlementState::Invalid;
            m_claims = EntitlementClaims{};
            m_lastErrorCode = QStringLiteral("subject_mismatch");
            m_lastErrorMessage = QStringLiteral("Entitlement user mismatch.");
            emit stateChanged();
            return;
        }

        if (m_metadata.userId.isEmpty())
        {
            m_metadata.userId = verified.claims.subject;
            saveMetadataToSettings();
        }

        if (m_credentialStore && m_credentialStore->isAvailable())
        {
            m_credentialStore->writeSecret(
                QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot), jwt);
        }
        cleanseString(&jwt);

        m_entitlementState = verified.state;
        m_claims = verified.claims;
        m_lastErrorCode.clear();
        m_lastErrorMessage.clear();

        emit stateChanged();
        emit entitlementsUpdated();
    });
}

void CloudClient::signOut()
{
    clearPendingDeviceFlow(true);

    if (m_refreshReply)
    {
        m_refreshReply->disconnect(this);
        m_refreshReply->abort();
        m_refreshReply->deleteLater();
        m_refreshReply = nullptr;
        m_refreshInFlight = false;
    }
    if (m_meReply)
    {
        m_meReply->disconnect(this);
        m_meReply->abort();
        m_meReply->deleteLater();
        m_meReply = nullptr;
    }
    if (m_entitlementsReply)
    {
        m_entitlementsReply->disconnect(this);
        m_entitlementsReply->abort();
        m_entitlementsReply->deleteLater();
        m_entitlementsReply = nullptr;
    }
    finishTokenWaiters(false, QString(), QStringLiteral("signed_out"));

    QString refreshTok;
    if (keyStoreAvailable())
    {
        m_credentialStore->readSecret(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), &refreshTok);
        m_credentialStore->removeSecret(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot));
        m_credentialStore->removeSecret(QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot));
    }
    else
    {
        refreshTok = std::move(m_volatileRefreshToken);
    }
    cleanseString(&m_volatileRefreshToken);

    QString accessTok = std::move(m_accessToken);
    cleanseString(&m_accessToken);
    m_accessTokenExpiresAtSec = 0;

    m_metadata.signedIn = false;
    m_metadata.deviceId.clear();
    m_metadata.userId.clear();
    m_metadata.userEmail.clear();
    saveMetadataToSettings();

    m_authState = CloudAuthState::SignedOut;
    m_entitlementState = EntitlementState::Invalid;
    m_claims = EntitlementClaims{};
    m_busy = false;
    m_autoRefreshBackoffSec = 0;
    m_autoRefreshNextAttemptAt = 0;
    m_lastErrorCode.clear();
    m_lastErrorMessage.clear();

    emit stateChanged();
    emit signedOut();

    const QString apiBase = effectiveApiUrl();
    if ((!refreshTok.trimmed().isEmpty() || !accessTok.trimmed().isEmpty())
        && isAllowedApiBaseUrl(apiBase)
        && (!m_settings || !m_settings->disableHttpsFeatures()))
    {
        QNetworkRequest req = buildApiRequest(QStringLiteral("/v1/auth/logout"), true, kLogoutTimeoutMs);
        QByteArray payload("{}");
        if (!refreshTok.trimmed().isEmpty())
        {
            QJsonObject obj;
            obj.insert(QStringLiteral("refresh_token"), refreshTok.trimmed());
            payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        }
        else
        {
            req.setRawHeader("Authorization", QByteArray("Bearer ") + accessTok.trimmed().toUtf8());
        }
        cleanseString(&refreshTok);
        cleanseString(&accessTok);

        QNetworkReply *reply = m_nam.post(req, payload);
        cleanseByteArray(&payload);
        connect(reply, &QNetworkReply::readyRead, this, [reply]() {
            if (reply->bytesAvailable() > kMaxApiResponseBytes)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
            emit logoutRequestFinished(ok);
        });
    }
    else
    {
        cleanseString(&refreshTok);
        cleanseString(&accessTok);
        emit logoutRequestFinished(false);
    }
}

} // namespace RagoShell
