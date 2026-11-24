#include "ragoshell-app/CloudService.h"

#include "ragoshell/core/settings/SettingsManager.h"
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

    QString CloudService::planId() const
    {
        if (_debugMode == 2)
            return QStringLiteral("pro");
        if (!_client || !_client->isSignedIn())
            return QStringLiteral("free");
        switch (_client->plan())
        {
        case EntitlementPlan::Pro:
            return QStringLiteral("pro");
        case EntitlementPlan::Team:
            return QStringLiteral("team");
        case EntitlementPlan::Free:
        default:
            return QStringLiteral("free");
        }
    }

    QString CloudService::planLabel() const
    {
        if (_debugMode == 2)
            return tr("Pro");
        if (!_client || !_client->isSignedIn())
            return tr("Free");
        switch (_client->plan())
        {
        case EntitlementPlan::Pro:
            return tr("Pro");
        case EntitlementPlan::Team:
            return tr("Team");
        case EntitlementPlan::Free:
        default:
            return tr("Free");
        }
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
        // ValidOnline only — in grace the gateway would deny anyway, so the
        // channel stays hidden (8.5 security review, finding 5).
        return _client && _client->isSignedIn()
            && _client->entitlementState() == EntitlementState::ValidOnline
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
        if (_settings && _settings->disableHttpsFeatures())
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
        if (!_client)
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
        if (!_client)
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
        if (_client && _client->isSignedIn()
            && _client->entitlementState() == EntitlementState::ValidOnline
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
        if (_settings && _settings->disableHttpsFeatures())
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
        case SyncEngine::State::Blocked: return tr("Blocked (plan required)");
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

    void CloudService::retranslate()
    {
        emit stateChanged();
    }
}
