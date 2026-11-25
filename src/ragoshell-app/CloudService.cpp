#include "ragoshell-app/CloudService.h"

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/UpdatePulse.h"
#include "ragoshell-app/PlatformInfo.h"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QLocale>
#include <QSysInfo>
#include <QUrl>

namespace RagoShell
{
    CloudService::CloudService(SettingsManager *settings,
                               PlatformInfo *platform,
                               QueryHistoryStore *history,
                               bool headlessRun,
                               QObject *parent)
        : QObject(parent),
          _settings(settings),
          _platform(platform)
    {
        if (headlessRun)
            _credentialStore = std::make_shared<InMemoryCloudCredentialStore>(true);
        else
            _credentialStore = std::make_shared<KeyStoreCloudCredentialStore>();

        _client = new CloudClient(_credentialStore, _settings, this);
        _sync = new SyncEngine(_client, _settings, history, _credentialStore, this);
        connect(_sync, &SyncEngine::stateChanged, this, &CloudService::stateChanged);
        connect(_sync, &SyncEngine::syncFinished, this, &CloudService::stateChanged);
        connect(_sync, &SyncEngine::wipeFinished, this, &CloudService::syncWipeFinished);
        connect(_sync, &SyncEngine::historyChanged, this, &CloudService::syncHistoryChanged);
        if (_platform)
        {
            _client->setPlatformOverride(_platform->os(),
                                         QSysInfo::currentCpuArchitecture(),
                                         QStringLiteral(PROJECT_VERSION));
        }

        _copiedResetTimer.setSingleShot(true);
        _copiedResetTimer.setInterval(2500);
        connect(&_copiedResetTimer, &QTimer::timeout, this, [this]() {
            if (!_codeCopied)
                return;
            _codeCopied = false;
            emit stateChanged();
        });

        connect(_client, &CloudClient::stateChanged, this, &CloudService::stateChanged);

        if (!headlessRun) {
            _client->restoreOfflineSession();
            // Keeps the hosted gateway token fresh without the manual refresh (docs/HostedGateway.md §9.8).
            if ((!_settings || !_settings->disableHttpsFeatures()) && !_client->isPolicyInvalid())
                _client->setAutoRefreshEnabled(true);
        }
    }

    bool CloudService::signedIn() const
    {
        if (_debugMode != 0)
            return _debugMode == 2;
        return _client && _client->isSignedIn();
    }

    bool CloudService::requestingCode() const
    {
        if (_debugMode != 0)
            return false;
        return _client && _client->authState() == CloudAuthState::RequestingCode;
    }

    bool CloudService::awaitingApproval() const
    {
        if (_debugMode != 0)
            return _debugMode == 1;
        return _client && _client->authState() == CloudAuthState::AwaitingBrowserApproval;
    }

    bool CloudService::busy() const
    {
        return _client && _client->isBusy();
    }

    bool CloudService::keyStoreAvailable() const
    {
        if (_debugMode != 0)
            return true;
        return _client && _client->keyStoreAvailable();
    }

    QString CloudService::deviceName() const
    {
        return _client ? _client->deviceName() : CloudClient::defaultDeviceName();
    }

    QString CloudService::deviceId() const
    {
        return _client ? _client->deviceId() : QString();
    }

    QString CloudService::deviceIdShort() const
    {
        if (_debugMode == 2)
            return QStringLiteral("7c9e6679");
        const QString full = deviceId();
        return full.isEmpty() ? QString() : full.left(8);
    }

    QString CloudService::userEmail() const
    {
        if (_debugMode == 2)
            return QStringLiteral("dba@example.org");
        return _client ? _client->userEmail() : QString();
    }

    QString CloudService::userId() const
    {
        return _client ? _client->userId() : QString();
    }

    QString CloudService::userCode() const
    {
        if (_debugMode == 1)
            return QStringLiteral("ABCD-2345");
        return _client ? _client->userCode() : QString();
    }

    QString CloudService::verificationUri() const
    {
        if (_debugMode == 1)
            return _client ? (_client->effectivePortalUrl() + QStringLiteral("/device")) : QString();
        return _client ? _client->verificationUri() : QString();
    }

    int CloudService::codeRemainingSeconds() const
    {
        if (_debugMode == 1)
            return 595;
        return _client ? _client->codeRemainingSeconds() : 0;
    }

    QString CloudService::formattedRemainingTime() const
    {
        const int sec = std::max(0, codeRemainingSeconds());
        const int mins = sec / 60;
        const int rem = sec % 60;
        return QStringLiteral("%1:%2")
            .arg(mins, 2, 10, QLatin1Char('0'))
            .arg(rem, 2, 10, QLatin1Char('0'));
    }

    QString CloudService::statusId() const
    {
        if (!_client || !_client->isSignedIn())
            return QStringLiteral("none");
        if (_client->isOfflineGraceActive())
            return QStringLiteral("offline_grace");
        switch (_client->subscriptionStatus())
        {
        case EntitlementStatus::Trialing:
            return QStringLiteral("trialing");
        case EntitlementStatus::Active:
            return QStringLiteral("active");
        case EntitlementStatus::PastDue:
            return QStringLiteral("past_due");
        case EntitlementStatus::Canceled:
            return QStringLiteral("canceled");
        case EntitlementStatus::None:
        default:
            return QStringLiteral("none");
        }
    }

    QString CloudService::statusLabel() const
    {
        if (!_client || !_client->isSignedIn())
            return tr("Not signed in");
        if (_client->isOfflineGraceActive())
            return tr("Offline grace");
        switch (_client->subscriptionStatus())
        {
        case EntitlementStatus::Trialing:
            return tr("Trial");
        case EntitlementStatus::Active:
            return tr("Active");
        case EntitlementStatus::PastDue:
            return tr("Past due");
        case EntitlementStatus::Canceled:
            return tr("Canceled");
        case EntitlementStatus::None:
        default:
            return tr("Active");
        }
    }

    bool CloudService::offlineGraceActive() const
    {
        return _client && _client->isOfflineGraceActive();
    }

    bool CloudService::syncEnabled() const
    {
        if (_debugMode == 2)
            return true;
        return _client && _client->isSignedIn() && _client->features().sync;
    }

    bool CloudService::hostedBridgeEnabled() const
    {
        if (_debugMode == 2)
            return true;
        // ValidOnline and unexpired only — in grace or past exp the gateway would
        // deny anyway, so the channel stays hidden (8.5 security review, finding 5).
        return _client && _client->isSignedIn()
            && !_client->isPolicyInvalid()
            && (!_settings || !_settings->disableHttpsFeatures())
            && _client->hasActiveOnlineEntitlement()
            && _client->features().hostedBridge;
    }

    QString CloudService::validitySummary() const
    {
        if (!_client || !_client->isSignedIn())
            return QString();
        if (_client->isOfflineGraceActive() && _client->offlineGraceUntil() > 0)
        {
            const QString dt = QLocale().toString(
                QDateTime::fromSecsSinceEpoch(_client->offlineGraceUntil()), QLocale::ShortFormat);
            return tr("Offline grace valid until %1").arg(dt);
        }
        if (_client->entitlementExpiresAt() > 0)
        {
            const QString dt = QLocale().toString(
                QDateTime::fromSecsSinceEpoch(_client->entitlementExpiresAt()), QLocale::ShortFormat);
            return tr("Verified until %1").arg(dt);
        }
        return QString();
    }

    QString CloudService::errorMessage() const
    {
        if (!_client)
            return QString();
        const QString code = _client->lastErrorCode();
        if (code.isEmpty())
            return QString();
        if (code == QLatin1String("policy_invalid"))
            return tr("Cloud features are blocked because the enterprise policy file is invalid.");
        if (code == QLatin1String("https_disabled"))
            return tr("Cloud features are disabled in configuration.");
        if (code == QLatin1String("access_denied"))
            return tr("Sign-in request was declined in the browser.");
        if (code == QLatin1String("expired_token"))
            return tr("The sign-in code has expired. Please request a new code.");
        if (code == QLatin1String("unsafe_verification_uri") || code == QLatin1String("unsafe_api_url"))
            return tr("Cloud service URL failed security validation.");
        if (code == QLatin1String("invalid_grant") || code == QLatin1String("invalid_token")
            || code == QLatin1String("missing_refresh_token"))
            return tr("Your cloud session has ended. Local connections remain unaffected.");
        if (code == QLatin1String("rate_limited"))
            return tr("Too many requests. Please wait a moment and try again.");
        if (code == QLatin1String("keystore_error"))
            return tr("Could not save session token to the OS keychain.");
        const QString raw = _client->lastErrorMessage();
        return raw.isEmpty() ? tr("Could not reach the cloud account service.") : raw;
    }

    void CloudService::startSignIn(const QString &customDeviceName)
    {
        if (!_client)
            return;
        _codeCopied = false;
        _client->startDeviceSignIn(customDeviceName);
    }

    void CloudService::cancelSignIn()
    {
        if (!_client)
            return;
        _codeCopied = false;
        _client->cancelDeviceSignIn();
    }

    void CloudService::copyUserCode()
    {
        const QString code = userCode();
        if (code.isEmpty())
            return;
        if (QClipboard *cb = QGuiApplication::clipboard())
            cb->setText(code);
        _codeCopied = true;
        _copiedResetTimer.start();
        emit stateChanged();
    }

    bool CloudService::openVerificationBrowser()
    {
        if (!_client || _client->isPolicyInvalid())
            return false;
        const QString target = _client->verificationUriComplete();
        if (!CloudClient::isAllowedVerificationUrl(
                target, _client->effectiveApiUrl(), _client->effectivePortalUrl()))
        {
            return false;
        }
        return QDesktopServices::openUrl(QUrl(target, QUrl::StrictMode));
    }

    bool CloudService::openManageAccountPortal()
    {
#ifdef SYSTEM_MOBILE
        return false;
#else
        if (_platform && _platform->isMobile())
            return false;
        if (!_client || _client->isPolicyInvalid())
            return false;
        const QString portal = _client->effectivePortalUrl();
        if (!CloudClient::isAllowedApiBaseUrl(portal))
            return false;
        return QDesktopServices::openUrl(QUrl(portal, QUrl::StrictMode));
#endif
    }

    void CloudService::updateDeviceName(const QString &name)
    {
        if (_client)
            _client->setDeviceName(name);
    }

    QString CloudService::hostedTokenMissingMessage()
    {
        if (_client && _client->isPolicyInvalid())
            return tr("Cloud features are blocked because the enterprise policy file is invalid.");
        if (_settings && _settings->disableHttpsFeatures())
            return tr("Cloud features are disabled in configuration.");
        if (_client && _client->isSignedIn()
            && _client->hasActiveOnlineEntitlement()
            && !_client->features().hostedBridge)
            return tr("The cloud bridge is not enabled for this account.");
        if (_client && _client->isSignedIn()) {
            if (!_client->isBusy()) // repeated clicks must not restart a refresh already running
                _client->refreshAccountAndEntitlements();
            return tr("Your cloud access is being renewed. Try again in a few seconds.");
        }
        return tr("Sign in to your cloud account to use the cloud gateway.");
    }

    void CloudService::refreshStatus()
    {
        if (!_client)
            return;
        _client->refreshAccountAndEntitlements();
    }

    void CloudService::signOut()
    {
        _debugMode = 0;
        if (!_client)
            return;
        _codeCopied = false;
        _client->signOut();
    }

    void CloudService::previewDebugState(int mode)
    {
        _debugMode = mode;
        emit stateChanged();
    }

    bool CloudService::syncActive() const
    {
        return _sync && _sync->syncEnabled();
    }

    bool CloudService::syncBusy() const
    {
        return _sync && _sync->state() == SyncEngine::State::Syncing;
    }

    bool CloudService::syncConfigured() const
    {
        return _sync && _sync->hasConfiguredKey();
    }

    QString CloudService::syncStateText() const
    {
        if (!_sync || !_sync->syncEnabled())
            return tr("Off");
        switch (_sync->state())
        {
        case SyncEngine::State::Syncing: return tr("Syncing…");
        case SyncEngine::State::Blocked: return tr("Not available for this account");
        case SyncEngine::State::Error:   return tr("Error");
        case SyncEngine::State::Disabled:return tr("Off");
        case SyncEngine::State::Idle:    return tr("Ready");
        }
        return {};
    }

    QString CloudService::syncLastSyncText() const
    {
        const qint64 ms = _sync ? _sync->lastSyncAtMs() : 0;
        if (ms <= 0)
            return tr("Never");
        return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms),
                                  QLocale::ShortFormat);
    }

    QString CloudService::syncLastError() const
    {
        return _sync ? _sync->lastError() : QString();
    }

    QStringList CloudService::syncCollections() const
    {
        return _sync ? _sync->enabledCollections() : QStringList();
    }

    bool CloudService::syncIncludePasswords() const
    {
        return _sync && _sync->passwordsIncluded();
    }

    QString CloudService::syncEnablePassphrase(const QString &passphrase)
    {
        QString err;
        if (!_sync || !_sync->configurePassphrase(passphrase, &err))
            return err.isEmpty() ? QStringLiteral("unavailable") : err;
        _sync->syncNow();
        emit stateChanged();
        return {};
    }

    QString CloudService::syncAttachVault(const QString &passphrase)
    {
        QString err;
        if (!_sync || !_sync->attachToRemoteVault(passphrase, &err))
            return err.isEmpty() ? QStringLiteral("unavailable") : err;
        _sync->syncNow();
        emit stateChanged();
        return {};
    }

    QString CloudService::syncEnableDeviceKey()
    {
        QString err;
        if (!_sync || !_sync->configureDeviceKey(&err))
            return err.isEmpty() ? QStringLiteral("unavailable") : err;
        _sync->syncNow();
        emit stateChanged();
        return {};
    }

    void CloudService::syncSetCollections(const QStringList &collections,
                                        bool includePasswords)
    {
        if (!_sync)
            return;
        _sync->setCollections(collections, includePasswords);
        emit stateChanged();
    }

    void CloudService::syncNow()
    {
        if (_sync)
            _sync->syncNow();
    }

    void CloudService::syncDisable()
    {
        if (!_sync)
            return;
        _sync->disable();
        emit stateChanged();
    }

    void CloudService::syncWipeRemote()
    {
        if (_sync)
            _sync->wipeRemote();
    }

    QString CloudService::serverMode() const
    {
        return _client ? _client->effectiveServerMode() : QStringLiteral("default");
    }

    bool CloudService::customServerActive() const
    {
        return _client && _client->isCustomServerActive();
    }

    QString CloudService::activeApiHost() const
    {
        const QString apiUrl = effectiveApiUrl();
        const QUrl url(apiUrl, QUrl::StrictMode);
        if (!url.host().isEmpty()) {
            return url.port(-1) > 0
                ? QStringLiteral("%1:%2").arg(url.host(), QString::number(url.port()))
                : url.host();
        }
        return apiUrl;
    }

    QString CloudService::activeServerSummary() const
    {
        if (policyInvalid())
            return tr("Blocked by invalid organization policy");
        if (customServerActive())
            return activeApiHost();
        return tr("%1 Cloud").arg(QString::fromUtf8(BrandInfo::productName));
    }

    static SettingsManager::CloudServerConfig activeConfiguredProfile(const SettingsManager *settings)
    {
        if (!settings)
            return {};
        if (settings->isCloudServerPolicyLocked())
            return settings->policyCloudServerConfig();
        if (settings->hasUserCloudServerConfig())
            return settings->userCloudServerConfig();
        if (settings->hasPolicyCloudServer())
            return settings->policyCloudServerConfig();
        return {};
    }

    QString CloudService::configuredServerMode() const
    {
        return activeConfiguredProfile(_settings).mode;
    }

    QString CloudService::configuredApiUrl() const
    {
        return activeConfiguredProfile(_settings).apiUrl;
    }

    QString CloudService::configuredPortalUrl() const
    {
        return activeConfiguredProfile(_settings).portalUrl;
    }

    QString CloudService::configuredBridgeUrl() const
    {
        return activeConfiguredProfile(_settings).bridgeUrl;
    }

    QString CloudService::configuredEntitlementPubkeys() const
    {
        return activeConfiguredProfile(_settings).entitlementPubkeys;
    }

    QString CloudService::effectiveApiUrl() const
    {
        return _client ? _client->effectiveApiUrl() : QString();
    }

    QString CloudService::effectivePortalUrl() const
    {
        return _client ? _client->effectivePortalUrl() : QString();
    }

    QString CloudService::effectiveBridgeUrl() const
    {
        return _client ? _client->effectiveBridgeUrl() : QString();
    }

    bool CloudService::policyLocked() const
    {
        return _client && _client->isPolicyLocked();
    }

    bool CloudService::policyInvalid() const
    {
        return _client && _client->isPolicyInvalid();
    }

    bool CloudService::envOverridden() const
    {
        return _client && _client->isEnvOverridden();
    }

    bool CloudService::updateChecksSuppressedByServer() const
    {
        if (!_settings || policyInvalid())
            return false;
        return customServerActive()
            && _settings->policyUpdateUrl().trimmed().isEmpty()
            && (_settings->isEnterprisePolicyLocked() || productEnv("UPDATE_URL").trimmed().isEmpty());
    }

    QString CloudService::deriveBridgeUrlPreview(const QString &portalUrl) const
    {
        return CloudClient::deriveBridgeUrlFromPortal(portalUrl);
    }

    static QString stripTrailingSlashesApp(const QString &url)
    {
        QString out = url.trimmed();
        while (out.size() > 1 && out.endsWith(QLatin1Char('/')))
            out.chop(1);
        return out;
    }

    QVariantMap CloudService::validateServerProfile(const QString &mode,
                                                    const QString &apiUrl,
                                                    const QString &portalUrl,
                                                    const QString &bridgeUrl,
                                                    const QString &pubkeys) const
    {
        if (policyLocked()) {
            return {{QStringLiteral("ok"), false},
                    {QStringLiteral("error"), tr("Cloud server settings are managed by organization policy.")},
                    {QStringLiteral("wouldSwitchSession"), false}};
        }

        const QString cleanMode = (mode.trimmed() == QLatin1String("custom"))
            ? QStringLiteral("custom")
            : QStringLiteral("default");
        const QString cleanApi = stripTrailingSlashesApp(apiUrl);
        const QString cleanPortal = stripTrailingSlashesApp(portalUrl);
        const QString cleanBridge = stripTrailingSlashesApp(bridgeUrl);
        const QString cleanKeys = pubkeys.trimmed();

        QString targetApi = stripTrailingSlashesApp(QString::fromUtf8(BrandInfo::apiUrl));
        QString targetPortal = stripTrailingSlashesApp(QString::fromUtf8(BrandInfo::portalUrl));
        QString targetBridge = CloudClient::deriveBridgeUrlFromPortal(targetPortal);
        QString targetCanonicalKeys = EntitlementVerifier::fromEmbeddedDefaults().canonicalKeyRingString();

        if (cleanMode == QLatin1String("custom")) {
            if (!CloudClient::isAllowedApiBaseUrl(cleanApi)) {
                return {{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), tr("Enter a valid https:// API origin (http:// is allowed only on localhost).")},
                        {QStringLiteral("wouldSwitchSession"), false}};
            }
            if (!CloudClient::isAllowedApiBaseUrl(cleanPortal)) {
                return {{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), tr("Enter a valid https:// Portal origin (http:// is allowed only on localhost).")},
                        {QStringLiteral("wouldSwitchSession"), false}};
            }
            if (!cleanBridge.isEmpty() && !CloudClient::isAllowedBridgeUrl(cleanBridge)) {
                return {{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), tr("Enter a valid wss:// Bridge URL (ws:// is allowed only on localhost), or leave empty.")},
                        {QStringLiteral("wouldSwitchSession"), false}};
            }
            QString ringErr;
            if (!EntitlementVerifier::isValidCustomKeyRing(cleanKeys, &ringErr)) {
                return {{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), tr("Enter one or more valid ES256 public keys (%1).").arg(QStringLiteral("<kid>:<base64-spki>"))},
                        {QStringLiteral("wouldSwitchSession"), false}};
            }
            targetApi = cleanApi;
            targetPortal = cleanPortal;
            targetBridge = cleanBridge.isEmpty()
                ? CloudClient::deriveBridgeUrlFromPortal(cleanPortal)
                : cleanBridge;
            targetCanonicalKeys = EntitlementVerifier::fromKeyRingString(cleanKeys).canonicalKeyRingString();
        }

        const bool hasSession = (_client && _client->isSignedIn()) || (_settings && _settings->syncEnabled());
        bool changed = false;
        if (_client && !envOverridden()) {
            const SettingsManager::CloudServerConfig curCfg = activeConfiguredProfile(_settings);
            const QString curCanonicalKeys = (curCfg.mode == QLatin1String("custom"))
                ? EntitlementVerifier::fromKeyRingString(curCfg.entitlementPubkeys).canonicalKeyRingString()
                : (!curCfg.entitlementPubkeys.trimmed().isEmpty()
                       ? EntitlementVerifier::fromKeyRingString(curCfg.entitlementPubkeys).canonicalKeyRingString()
                       : EntitlementVerifier::fromEmbeddedDefaults().canonicalKeyRingString());
            changed = (cleanMode != _client->effectiveServerMode())
                      || (targetApi != _client->effectiveApiUrl())
                      || (targetPortal != _client->effectivePortalUrl())
                      || (targetBridge != _client->effectiveBridgeUrl())
                      || (targetCanonicalKeys != curCanonicalKeys);
        }

        return {{QStringLiteral("ok"), true},
                {QStringLiteral("error"), QString()},
                {QStringLiteral("wouldSwitchSession"), hasSession && changed}};
    }

    QString CloudService::applyServerProfile(const QString &mode,
                                             const QString &apiUrl,
                                             const QString &portalUrl,
                                             const QString &bridgeUrl,
                                             const QString &pubkeys)
    {
        const QVariantMap check = validateServerProfile(mode, apiUrl, portalUrl, bridgeUrl, pubkeys);
        if (!check.value(QStringLiteral("ok")).toBool())
            return check.value(QStringLiteral("error")).toString();
        if (!_settings || !_client)
            return tr("Cloud settings are unavailable.");

        const bool shouldSwitch = check.value(QStringLiteral("wouldSwitchSession")).toBool();
        const bool hadSession = _client->isSignedIn() || _settings->syncEnabled();
        if (shouldSwitch && _client->isSignedIn()) {
            _codeCopied = false;
            _client->signOut();
        }

        SettingsManager::CloudServerConfig cfg;
        cfg.mode = (mode.trimmed() == QLatin1String("custom"))
            ? QStringLiteral("custom")
            : QStringLiteral("default");
        if (cfg.mode == QLatin1String("custom")) {
            cfg.apiUrl = stripTrailingSlashesApp(apiUrl);
            cfg.portalUrl = stripTrailingSlashesApp(portalUrl);
            cfg.bridgeUrl = stripTrailingSlashesApp(bridgeUrl);
            cfg.entitlementPubkeys = pubkeys.trimmed();
        }
        _settings->setUserCloudServerConfig(cfg);
        _settings->save();
        if (shouldSwitch || !hadSession)
            _client->applyServerProfileChange();
        else
            emit stateChanged();
        return {};
    }

    QString CloudService::resetServerProfileToDefault()
    {
        if (policyLocked())
            return tr("Cloud server settings are locked by organization policy.");
        if (!_settings || !_client)
            return tr("Cloud settings are unavailable.");

        const bool hasSession = _client->isSignedIn() || _settings->syncEnabled();
        const bool willChange = hasSession && (_client->isCustomServerActive() || _settings->hasPolicyCloudServer()) && !envOverridden();
        if (willChange && _client->isSignedIn()) {
            _codeCopied = false;
            _client->signOut();
        }

        _settings->clearUserCloudServerConfig();
        _settings->save();
        _client->applyServerProfileChange();
        return {};
    }

    void CloudService::retranslate()
    {
        emit stateChanged();
    }
}
