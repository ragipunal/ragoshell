#include "ragoshell-app/UpdateService.h"

#include <algorithm>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QUrl>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/Minisign.h"
#include "ragoshell-app/LanguageManager.h"
#include "ragoshell-app/PlatformInfo.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/ShellController.h"

namespace RagoShell
{
    namespace
    {
        constexpr qint64 kAutoCheckIntervalSec = 24 * 3600;
        constexpr int kRequestTimeoutMs = 8000;
        constexpr qint64 kMaxResponseBytes = 65536;
        constexpr qint64 kMaxMinisigBytes = 8192;
        constexpr qint64 kMaxSessionSec = 7 * 24 * 3600;

        QString currentAppVersionString()
        {
            const QString envVer = productEnv("APP_VERSION_OVERRIDE").trimmed();
            if (!envVer.isEmpty())
                return envVer;
            return QStringLiteral(PROJECT_VERSION);
        }
    }

    UpdateService::UpdateService(SettingsManager *settings,
                                 PlatformInfo *platform,
                                 LanguageManager *language,
                                 bool headlessRun,
                                 QObject *parent)
        : QObject(parent),
          _settings(settings),
          _platform(platform),
          _language(language),
          _headlessRun(headlessRun)
    {
        _sessionTimer.start();
        _copiedResetTimer.setSingleShot(true);
        _copiedResetTimer.setInterval(2500);
        connect(&_copiedResetTimer, &QTimer::timeout, this, [this]() {
            if (!_linkCopied)
                return;
            _linkCopied = false;
            emit stateChanged();
        });

        const bool forceCheck = productEnvInt("FORCE_UPDATE_CHECK") == 1;
        if (_settings && (!_headlessRun || forceCheck)) {
            const int prevLaunches = _settings->cacheData(QStringLiteral("usageLaunchCount")).toInt();
            _settings->addCacheData(QStringLiteral("usageLaunchCount"), std::max(0, prevLaunches) + 1);
            if (!BrandInfo::isStoreBuild)
                restoreCachedState();
        }

        if (BrandInfo::isStoreBuild)
            return;

        if (!QNetworkInformation::instance()) {
            if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability))
                QNetworkInformation::loadDefaultBackend();
        }
        if (QNetworkInformation *info = QNetworkInformation::instance()) {
            connect(info, &QNetworkInformation::reachabilityChanged, this,
                    [this](QNetworkInformation::Reachability reachability) {
                if (_pendingOnlineCheck
                    && reachability != QNetworkInformation::Reachability::Disconnected) {
                    _pendingOnlineCheck = false;
                    startCheck(false);
                }
            });
        }
    }

    void UpdateService::attachBackend(QmlBackend *backend)
    {
        if (!backend)
            return;
        connect(backend, &QmlBackend::shellOpened, this, [this](QObject *tab) {
            recordShellOpened();
            auto *ctl = qobject_cast<ShellController *>(tab);
            if (!ctl)
                return;
            connect(ctl, &ShellController::busyChanged, this, [this, ctl]() {
                if (!ctl->busy() && !ctl->cancelled())
                    recordQueryExecuted();
            });
        });
    }

    void UpdateService::recordShellOpened()
    {
        if (!_settings || _headlessRun)
            return;
        const int count = _settings->cacheData(QStringLiteral("usageShellsOpened")).toInt();
        _settings->addCacheData(QStringLiteral("usageShellsOpened"), std::max(0, count) + 1);
    }

    void UpdateService::recordQueryExecuted()
    {
        if (!_settings || _headlessRun)
            return;
        const int count = _settings->cacheData(QStringLiteral("usageQueriesExecuted")).toInt();
        _settings->addCacheData(QStringLiteral("usageQueriesExecuted"), std::max(0, count) + 1);
    }

    void UpdateService::flushSessionMetrics()
    {
        if (!_settings || _headlessRun || !_sessionTimer.isValid())
            return;
        const qint64 elapsedSec = std::clamp(_sessionTimer.elapsed() / 1000, qint64(0), kMaxSessionSec);
        _settings->addCacheData(QStringLiteral("usageLastSessionSec"), static_cast<int>(elapsedSec));
    }

    bool UpdateService::storeBuild() const
    {
        return BrandInfo::isStoreBuild;
    }

    bool UpdateService::bannerVisible() const
    {
        if (BrandInfo::isStoreBuild)
            return false;
        return _updateAvailable
               && !_mandatory
               && !_latestVersion.isEmpty()
               && _latestVersion != _dismissedVersion;
    }

    bool UpdateService::shareAnonymousUsageEnabled() const
    {
        if (!_settings)
            return false;
        const QVariant v = _settings->cacheData(QStringLiteral("shareAnonymousUsage"));
        return v.isValid() ? v.toBool() : true;
    }

    bool UpdateService::isOnlineForEndpoint(const QString &endpoint) const
    {
        const QUrl url(endpoint);
        const QString host = url.host().toLower();
        if (host == QLatin1String("127.0.0.1")
            || host == QLatin1String("localhost")
            || host == QLatin1String("::1")) {
            return true;
        }
        const QNetworkInformation *info = QNetworkInformation::instance();
        if (!info || !info->supports(QNetworkInformation::Feature::Reachability))
            return true;
        return info->reachability() != QNetworkInformation::Reachability::Disconnected;
    }

    UpdatePulse::TelemetrySnapshot UpdateService::buildSnapshot() const
    {
        UpdatePulse::TelemetrySnapshot snap;
        snap.appVersion = currentAppVersionString();
        snap.platform = _platform ? _platform->os() : QSysInfo::productType();
        snap.arch = QSysInfo::currentCpuArchitecture();
        snap.formFactor = _platform ? _platform->formFactor() : QStringLiteral("desktop");
        snap.osVersion = QSysInfo::productVersion();
        snap.uiLanguage = _language ? _language->effective() : QStringLiteral("en");

        if (!_settings)
            return snap;

        snap.anonymousId = _settings->anonymousID();
        snap.launchCount = _settings->cacheData(QStringLiteral("usageLaunchCount")).toInt();
        const int prevSessionSec = _settings->cacheData(QStringLiteral("usageLastSessionSec")).toInt();
        const int liveSessionSec = _sessionTimer.isValid()
            ? static_cast<int>(std::clamp(_sessionTimer.elapsed() / 1000, qint64(0), kMaxSessionSec))
            : 0;
        snap.lastSessionSec = std::max(prevSessionSec, liveSessionSec);
        snap.shellsOpened = _settings->cacheData(QStringLiteral("usageShellsOpened")).toInt();
        snap.queriesExecuted = _settings->cacheData(QStringLiteral("usageQueriesExecuted")).toInt();
        snap.aiEnabled = _settings->aiAssistantEnabled();
        snap.uiTheme = _settings->uiTheme();
        snap.uiDensity = _settings->uiDensity();
        snap.dbVersions = _settings->dbVersionsConnected().values();

        const auto conns = _settings->connections();
        snap.savedConnections = static_cast<int>(conns.size());
        for (const ConnectionSettings *c : conns) {
            if (!c)
                continue;
            if (c->sshSettings() && c->sshSettings()->enabled())
                ++snap.sshConnections;
            if ((c->sslSettings() && c->sslSettings()->sslEnabled()) || c->isSrv())
                ++snap.tlsConnections;
            if (c->isReplicaSet() || c->isSrv())
                ++snap.replicaSetConnections;
        }
        return snap;
    }

    void UpdateService::restoreCachedState()
    {
        if (!_settings)
            return;

        _dismissedVersion = _settings->cacheData(QStringLiteral("dismissedUpdateVersion")).toString().trimmed();
        const QString currentVer = currentAppVersionString();
        const QString cachedLatest = _settings->cacheData(QStringLiteral("cachedLatestVersion")).toString().trimmed();
        const QString cachedMin = _settings->cacheData(QStringLiteral("cachedMinSupportedVersion")).toString().trimmed();
        const bool cachedMandFlag = _settings->cacheData(QStringLiteral("cachedMandatory")).toBool();
        const QString cachedMandApp = _settings->cacheData(QStringLiteral("cachedMandatoryForAppVersion")).toString().trimmed();

        const bool belowMin = !cachedMin.isEmpty() && UpdatePulse::compareVersions(currentVer, cachedMin) < 0;
        const bool newerCached = !cachedLatest.isEmpty() && UpdatePulse::compareVersions(currentVer, cachedLatest) < 0;
        const bool stillMandatory = belowMin || (cachedMandFlag && cachedMandApp == currentVer && newerCached);

        if (!newerCached && !belowMin)
            return;

        _updateAvailable = true;
        _mandatory = stillMandatory;
        _latestVersion = !cachedLatest.isEmpty() ? cachedLatest : cachedMin;
        _minSupportedVersion = cachedMin;
        _releaseNotes = _settings->cacheData(QStringLiteral("cachedReleaseNotes")).toString();
        const QString cachedUrl = _settings->cacheData(QStringLiteral("cachedDownloadUrl")).toString().trimmed();
        _downloadUrl = UpdatePulse::isSafeDownloadUrl(cachedUrl) ? cachedUrl : UpdatePulse::defaultReleasesUrl();
        const QString plat = _platform ? _platform->os() : QString();
        const QString cachedLbl = _settings->cacheData(QStringLiteral("cachedDownloadLabel")).toString().trimmed();
        _downloadLabel = cachedLbl.isEmpty() ? UpdatePulse::defaultPlatformLabel(plat) : cachedLbl;
    }

    void UpdateService::persistCachedResult(const UpdatePulse::CheckResult &res)
    {
        if (!_settings)
            return;
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        _settings->addCacheData(QStringLiteral("lastAutoUpdateCheckUtc"), nowSec);
        _settings->addCacheData(QStringLiteral("lastUpdateCheckEpochSec"), nowSec);
        _settings->addCacheData(QStringLiteral("cachedLatestVersion"), res.latestVersion);
        _settings->addCacheData(QStringLiteral("cachedMinSupportedVersion"), res.minSupportedVersion);
        _settings->addCacheData(QStringLiteral("cachedMandatory"), res.mandatory);
        _settings->addCacheData(QStringLiteral("cachedMandatoryForAppVersion"),
                                res.mandatory ? currentAppVersionString() : QString());
        _settings->addCacheData(QStringLiteral("cachedDownloadUrl"), res.downloadUrl);
        _settings->addCacheData(QStringLiteral("cachedDownloadLabel"), res.downloadLabel);
        _settings->addCacheData(QStringLiteral("cachedReleaseNotes"), res.releaseNotes);
        if (res.manifestTimestampSec > 0) {
            const qint64 prevTs = _settings->cacheData(QStringLiteral("cachedManifestTimestampSec")).toLongLong();
            if (res.manifestTimestampSec > prevTs)
                _settings->addCacheData(QStringLiteral("cachedManifestTimestampSec"), res.manifestTimestampSec);
        }
        _settings->save();
    }

    QString UpdateService::resolveActiveEndpoint(bool manual,
                                                 bool *suppressedByPolicyOut,
                                                 bool *suppressedByServerOut) const
    {
        const bool policyInvalid = _settings && _settings->isEnterprisePolicyInvalid();
        const bool policyLocked = _settings && _settings->isEnterprisePolicyLocked();
        const bool checkLockedOff = _settings && _settings->isCheckForUpdatesLocked() && !_settings->checkForUpdates();
        const bool checkEnabled = manual
            ? (!policyInvalid && !checkLockedOff)
            : (_settings && _settings->checkForUpdates());
        const bool customServer = _settings && CloudClient::hasCustomServerProfile(_settings);
        const QString policyUrl = _settings ? _settings->policyUpdateUrl() : QString();
        const QString envUrl = productEnv("UPDATE_URL").trimmed();

        if (suppressedByPolicyOut)
            *suppressedByPolicyOut = policyInvalid || checkLockedOff;
        if (suppressedByServerOut)
            *suppressedByServerOut = !policyInvalid && !checkLockedOff && customServer
                                     && policyUrl.isEmpty()
                                     && (policyLocked || envUrl.isEmpty() || !UpdatePulse::isSafeEndpointUrl(envUrl));

        return UpdatePulse::resolveActiveUpdateEndpoint(
            policyInvalid, checkEnabled, customServer, policyUrl, policyLocked);
    }

    void UpdateService::onWelcomeShown()
    {
        if (BrandInfo::isStoreBuild)
            return;
        const bool forceCheck = productEnvInt("FORCE_UPDATE_CHECK") == 1;
        if ((_headlessRun && !forceCheck) || _welcomeCheckedThisSession)
            return;
        if (!_settings || _settings->disableHttpsFeatures() || !_settings->checkForUpdates())
            return;

        if (!forceCheck && !_mandatory) {
            qint64 lastSec = _settings->cacheData(QStringLiteral("lastAutoUpdateCheckUtc")).toLongLong();
            if (lastSec <= 0)
                lastSec = _settings->cacheData(QStringLiteral("lastUpdateCheckEpochSec")).toLongLong();
            const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
            if (lastSec > 0 && (nowSec - lastSec) >= 0 && (nowSec - lastSec) < kAutoCheckIntervalSec) {
                _welcomeCheckedThisSession = true;
                return;
            }
        }

        const QString endpoint = resolveActiveEndpoint(false);
        if (endpoint.isEmpty())
            return;

        if (!isOnlineForEndpoint(endpoint)) {
            _pendingOnlineCheck = true;
            return;
        }

        _welcomeCheckedThisSession = true;
        startCheck(false);
    }

    void UpdateService::checkNow()
    {
        if (BrandInfo::isStoreBuild)
            return;
        if (_settings && _settings->isEnterprisePolicyInvalid()) {
            _statusKind = StatusKind::PolicyDisabled;
            _statusIsError = true;
            updateStatusText();
            emit stateChanged();
            return;
        }
        if (_settings && _settings->disableHttpsFeatures()) {
            _statusKind = StatusKind::Disabled;
            _statusIsError = true;
            updateStatusText();
            emit stateChanged();
            return;
        }
        bool suppressedByPolicy = false;
        bool suppressedByServer = false;
        const QString endpoint = resolveActiveEndpoint(true, &suppressedByPolicy, &suppressedByServer);
        if (endpoint.isEmpty()) {
            _statusKind = suppressedByServer ? StatusKind::CustomServerSuppressed : StatusKind::PolicyDisabled;
            _statusIsError = !suppressedByServer;
            updateStatusText();
            emit stateChanged();
            return;
        }
        if (!isOnlineForEndpoint(endpoint)) {
            _statusKind = StatusKind::Offline;
            _statusIsError = true;
            updateStatusText();
            emit stateChanged();
            return;
        }
        startCheck(true);
    }

    void UpdateService::startCheck(bool manual)
    {
        if (BrandInfo::isStoreBuild || _checking)
            return;
        if (_settings && _settings->disableHttpsFeatures())
            return;

        const QString endpoint = resolveActiveEndpoint(manual);
        if (endpoint.isEmpty() || !UpdatePulse::isSafeEndpointUrl(endpoint))
            return;

        _debugPreviewMode = 0;
        _checking = true;
        if (manual) {
            _statusKind = StatusKind::Checking;
            _statusIsError = false;
            updateStatusText();
        }
        emit stateChanged();

        QNetworkRequest req{QUrl(endpoint)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("%1/%2")
                          .arg(QLatin1String(BrandInfo::productName), currentAppVersionString()));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setMaximumRedirectsAllowed(0);
        req.setTransferTimeout(kRequestTimeoutMs);

        const QByteArray payload = UpdatePulse::buildRequestPayload(
            buildSnapshot(), shareAnonymousUsageEnabled());
        QNetworkReply *reply = _nam.post(req, payload);
        _activeReply = reply;
        connect(reply, &QNetworkReply::readyRead, this, [reply]() {
            if (reply->bytesAvailable() > kMaxResponseBytes)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, endpoint, manual]() {
            handleReplyFinished(reply, endpoint, manual);
        });
    }

    void UpdateService::failCheck(bool manual, StatusKind kind)
    {
        _checking = false;
        if (manual) {
            _statusKind = kind;
            _statusIsError = true;
            updateStatusText();
        }
        emit stateChanged();
        emit checkCompleted(false, _updateAvailable, _mandatory);
    }

    void UpdateService::applyVerifiedResult(const UpdatePulse::CheckResult &res, const QByteArray &responseBytes)
    {
        _checking = false;
        _lastResponseBytes = responseBytes;
        _updateAvailable = res.updateAvailable;
        _mandatory = res.mandatory;
        _latestVersion = res.latestVersion;
        _minSupportedVersion = res.minSupportedVersion;
        _publishedAt = res.publishedAt;
        _releaseNotes = res.releaseNotes;
        _downloadUrl = res.downloadUrl;
        _downloadLabel = res.downloadLabel;

        persistCachedResult(res);

        _statusIsError = false;
        _statusKind = res.updateAvailable ? StatusKind::Available : StatusKind::UpToDate;
        updateStatusText();

        const QString plat = _platform ? _platform->os() : QString();
        if (res.updateAvailable || productEnvInt("FORCE_UPDATE_CHECK") == 1) {
            qInfo().noquote() << QStringLiteral("[update] checked: latest=%1 available=%2 mandatory=%3 platform=%4")
                                     .arg(_latestVersion)
                                     .arg(_updateAvailable ? 1 : 0)
                                     .arg(_mandatory ? 1 : 0)
                                     .arg(plat);
        }

        emit stateChanged();
        emit checkCompleted(true, _updateAvailable, _mandatory);
    }

    void UpdateService::fetchAndVerifySignedManifest(const QString &endpoint, const QString &pubKey, bool manual)
    {
        Minisign::PublicKey parsedPub;
        QString pubErr;
        if (!Minisign::parsePublicKey(pubKey, &parsedPub, &pubErr)) {
            qWarning().noquote() << QStringLiteral("[update] Invalid configured Minisign public key: %1").arg(pubErr);
            failCheck(manual, StatusKind::SignatureFailed);
            return;
        }

        const QString manifestUrl = UpdatePulse::releasesManifestUrl(
            QString(), endpoint, _settings && _settings->isEnterprisePolicyLocked());
        const QString minisigUrl = manifestUrl + QStringLiteral(".minisig");
        if (!UpdatePulse::isSafeEndpointUrl(manifestUrl) || !UpdatePulse::isSafeEndpointUrl(minisigUrl)) {
            qWarning().noquote() << QStringLiteral("[update] Unsafe signed manifest URL refused.");
            failCheck(manual, StatusKind::SignatureFailed);
            return;
        }

        const QString ua = QStringLiteral("%1/%2")
                               .arg(QLatin1String(BrandInfo::productName), currentAppVersionString());

        QNetworkRequest manifestReq{QUrl(manifestUrl)};
        manifestReq.setHeader(QNetworkRequest::UserAgentHeader, ua);
        manifestReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
        manifestReq.setMaximumRedirectsAllowed(0);
        manifestReq.setTransferTimeout(kRequestTimeoutMs);

        QNetworkReply *mReply = _nam.get(manifestReq);
        _manifestReply = mReply;
        connect(mReply, &QNetworkReply::readyRead, this, [mReply]() {
            if (mReply->bytesAvailable() > kMaxResponseBytes)
                mReply->abort();
        });
        connect(mReply, &QNetworkReply::finished, this, [this, mReply, minisigUrl, ua, pubKey, manual]() {
            mReply->deleteLater();
            if (_manifestReply == mReply)
                _manifestReply = nullptr;

            const int mStatus = mReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (mReply->error() != QNetworkReply::NoError || mStatus < 200 || mStatus >= 300) {
                qWarning().noquote() << QStringLiteral("[update] Failed to fetch signed releases.json (HTTP %1).").arg(mStatus);
                failCheck(manual, StatusKind::SignatureFailed);
                return;
            }
            const QByteArray manifestBytes = mReply->readAll();
            if (manifestBytes.isEmpty() || manifestBytes.size() > kMaxResponseBytes) {
                failCheck(manual, StatusKind::SignatureFailed);
                return;
            }

            QNetworkRequest sigReq{QUrl(minisigUrl)};
            sigReq.setHeader(QNetworkRequest::UserAgentHeader, ua);
            sigReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::NoLessSafeRedirectPolicy);
            sigReq.setMaximumRedirectsAllowed(0);
            sigReq.setTransferTimeout(kRequestTimeoutMs);

            QNetworkReply *sReply = _nam.get(sigReq);
            _minisigReply = sReply;
            connect(sReply, &QNetworkReply::readyRead, this, [sReply]() {
                if (sReply->bytesAvailable() > kMaxMinisigBytes)
                    sReply->abort();
            });
            connect(sReply, &QNetworkReply::finished, this, [this, sReply, manifestBytes, pubKey, manual]() {
                sReply->deleteLater();
                if (_minisigReply == sReply)
                    _minisigReply = nullptr;

                const int sStatus = sReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (sReply->error() != QNetworkReply::NoError || sStatus < 200 || sStatus >= 300) {
                    qWarning().noquote() << QStringLiteral("[update] Failed to fetch releases.json.minisig (HTTP %1).").arg(sStatus);
                    failCheck(manual, StatusKind::SignatureFailed);
                    return;
                }
                const QByteArray sigBytes = sReply->readAll();
                if (sigBytes.isEmpty() || sigBytes.size() > kMaxMinisigBytes) {
                    failCheck(manual, StatusKind::SignatureFailed);
                    return;
                }

                const QString plat = _platform ? _platform->os() : QString();
                const QString lang = _language ? _language->effective() : QStringLiteral("en");
                const qint64 minTs = _settings
                    ? _settings->cacheData(QStringLiteral("cachedManifestTimestampSec")).toLongLong()
                    : 0;
                const UpdatePulse::CheckResult verified = UpdatePulse::parseAndVerifySignedManifest(
                    manifestBytes,
                    QString::fromUtf8(sigBytes),
                    pubKey,
                    currentAppVersionString(),
                    plat,
                    lang,
                    minTs);
                if (!verified.ok) {
                    qWarning().noquote() << QStringLiteral("[update] Minisign verification rejected update manifest: %1")
                                                .arg(verified.error);
                    failCheck(manual, StatusKind::SignatureFailed);
                    return;
                }

                applyVerifiedResult(verified, manifestBytes);
            });
        });
    }

    void UpdateService::handleReplyFinished(QNetworkReply *reply, const QString &endpoint, bool manual)
    {
        reply->deleteLater();
        if (_activeReply == reply)
            _activeReply = nullptr;

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
            failCheck(manual, StatusKind::Failed);
            return;
        }

        const QByteArray body = reply->readAll();
        if (body.size() > kMaxResponseBytes) {
            failCheck(manual, StatusKind::Failed);
            return;
        }
        const QString plat = _platform ? _platform->os() : QString();
        const QString lang = _language ? _language->effective() : QStringLiteral("en");
        const UpdatePulse::CheckResult res = UpdatePulse::parseCheckResponse(
            body, currentAppVersionString(), plat, lang);

        if (!res.ok) {
            failCheck(manual, StatusKind::Failed);
            return;
        }

        const QString pubKey = Minisign::effectiveEmbeddedPublicKey();
        if (!pubKey.isEmpty() && (res.updateAvailable || res.mandatory || _updateAvailable || _mandatory)) {
            fetchAndVerifySignedManifest(endpoint, pubKey, manual);
            return;
        }

        applyVerifiedResult(res, body);
    }

    void UpdateService::dismissOptionalUpdate()
    {
        if (_mandatory || _latestVersion.isEmpty())
            return;
        _dismissedVersion = _latestVersion;
        if (_settings && !_headlessRun) {
            _settings->addCacheData(QStringLiteral("dismissedUpdateVersion"), _dismissedVersion);
            _settings->save();
        }
        emit stateChanged();
    }

    bool UpdateService::openDownload()
    {
        const QString target = UpdatePulse::isSafeDownloadUrl(_downloadUrl)
            ? _downloadUrl
            : UpdatePulse::defaultReleasesUrl();
        return QDesktopServices::openUrl(QUrl(target));
    }

    void UpdateService::copyDownloadUrl()
    {
        const QString target = UpdatePulse::isSafeDownloadUrl(_downloadUrl)
            ? _downloadUrl
            : UpdatePulse::defaultReleasesUrl();
        if (QClipboard *cb = QGuiApplication::clipboard())
            cb->setText(target);
        _linkCopied = true;
        _copiedResetTimer.start();
        emit stateChanged();
    }

    void UpdateService::previewDebugUpdate(bool mandatoryMode)
    {
        _debugPreviewMode = mandatoryMode ? 2 : 1;
        _checking = false;
        _updateAvailable = true;
        _mandatory = mandatoryMode;
        _dismissedVersion.clear();
        _latestVersion = QStringLiteral("2.4.0");
        _minSupportedVersion = mandatoryMode ? QStringLiteral("2.4.0") : QStringLiteral("1.0.0");
        _publishedAt = QStringLiteral("2026-10-30");
        _downloadUrl = UpdatePulse::defaultReleasesUrl();
        const QString plat = _platform ? _platform->os() : QString();
        _downloadLabel = UpdatePulse::defaultPlatformLabel(plat);
        _releaseNotes = mandatoryMode
            ? tr("This release includes critical protocol and security updates required to continue using %1 safely.")
                  .arg(QLatin1String(BrandInfo::productName))
            : tr("Faster MongoDB 8.0 shell completions, improved SSH tunnel stability, and lower memory usage on large result sets.");
        _statusKind = StatusKind::Available;
        _statusIsError = false;
        updateStatusText();
        emit stateChanged();
    }

    void UpdateService::updateStatusText()
    {
        switch (_statusKind) {
        case StatusKind::None:
            _statusMessage.clear();
            break;
        case StatusKind::Checking:
            _statusMessage = tr("Checking for updates…");
            break;
        case StatusKind::UpToDate:
            _statusMessage = tr("You are using the latest version (%1).").arg(currentAppVersionString());
            break;
        case StatusKind::Available:
            _statusMessage = tr("%1 %2 is available for %3.")
                                 .arg(QLatin1String(BrandInfo::productName), _latestVersion, _downloadLabel);
            break;
        case StatusKind::Disabled:
            _statusMessage = tr("HTTPS network features are disabled in configuration.");
            break;
        case StatusKind::PolicyDisabled:
            _statusMessage = tr("Update checks are disabled by organization policy.");
            break;
        case StatusKind::CustomServerSuppressed:
            _statusMessage = tr("Update checks are disabled while a custom cloud server is configured.");
            break;
        case StatusKind::Offline:
            _statusMessage = tr("No internet connection available.");
            break;
        case StatusKind::Failed:
            _statusMessage = tr("Could not check for updates right now.");
            break;
        case StatusKind::SignatureFailed:
            _statusMessage = tr("Update signature verification failed.");
            break;
        }
    }

    void UpdateService::retranslate()
    {
        if (_debugPreviewMode != 0) {
            previewDebugUpdate(_debugPreviewMode == 2);
            return;
        }
        if (!_lastResponseBytes.isEmpty()) {
            const QString plat = _platform ? _platform->os() : QString();
            const QString lang = _language ? _language->effective() : QStringLiteral("en");
            const UpdatePulse::CheckResult res = UpdatePulse::parseCheckResponse(
                _lastResponseBytes, currentAppVersionString(), plat, lang);
            if (res.ok) {
                _releaseNotes = res.releaseNotes;
                _downloadLabel = res.downloadLabel;
            }
        }
        updateStatusText();
        emit stateChanged();
    }
}
