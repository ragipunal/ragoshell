#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/cloud/SyncEngine.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    class PlatformInfo;
    class QueryHistoryStore;
    class SettingsManager;

    /**
     * @brief QML singleton bridging RagoShell::CloudClient to the Account Preferences UI (A1 / A2).
     */
    class CloudService : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(CloudService)
        QML_SINGLETON

        Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool requestingCode READ requestingCode NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool awaitingApproval READ awaitingApproval NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool keyStoreAvailable READ keyStoreAvailable NOTIFY stateChanged FINAL)

        Q_PROPERTY(QString deviceName READ deviceName NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString deviceId READ deviceId NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString deviceIdShort READ deviceIdShort NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString userEmail READ userEmail NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString userId READ userId NOTIFY stateChanged FINAL)

        Q_PROPERTY(QString userCode READ userCode NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString verificationUri READ verificationUri NOTIFY stateChanged FINAL)
        Q_PROPERTY(int codeRemainingSeconds READ codeRemainingSeconds NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString formattedRemainingTime READ formattedRemainingTime NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool codeCopied READ codeCopied NOTIFY stateChanged FINAL)

        Q_PROPERTY(QString statusId READ statusId NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString statusLabel READ statusLabel NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool offlineGraceActive READ offlineGraceActive NOTIFY stateChanged FINAL)

        Q_PROPERTY(bool syncEnabled READ syncEnabled NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool syncActive READ syncActive NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool syncBusy READ syncBusy NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool syncConfigured READ syncConfigured NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString syncStateText READ syncStateText NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString syncLastSyncText READ syncLastSyncText NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString syncLastError READ syncLastError NOTIFY stateChanged FINAL)
        Q_PROPERTY(QStringList syncCollections READ syncCollections NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool syncIncludePasswords READ syncIncludePasswords NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool hostedBridgeEnabled READ hostedBridgeEnabled NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString validitySummary READ validitySummary NOTIFY stateChanged FINAL)

        Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged FINAL)

        // Enterprise / self-hosted cloud server profile (docs/EnterpriseServerProfile.md §6 Step E)
        Q_PROPERTY(QString serverMode READ serverMode NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool customServerActive READ customServerActive NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString activeServerSummary READ activeServerSummary NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString activeApiHost READ activeApiHost NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString configuredServerMode READ configuredServerMode NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString configuredApiUrl READ configuredApiUrl NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString configuredPortalUrl READ configuredPortalUrl NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString configuredBridgeUrl READ configuredBridgeUrl NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString configuredEntitlementPubkeys READ configuredEntitlementPubkeys NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString effectiveApiUrl READ effectiveApiUrl NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString effectivePortalUrl READ effectivePortalUrl NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString effectiveBridgeUrl READ effectiveBridgeUrl NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool policyLocked READ policyLocked NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool policyInvalid READ policyInvalid NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool envOverridden READ envOverridden NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool updateChecksSuppressedByServer READ updateChecksSuppressedByServer NOTIFY stateChanged FINAL)

    public:
        CloudService(SettingsManager *settings,
                     PlatformInfo *platform,
                     QueryHistoryStore *history,
                     bool headlessRun,
                     QObject *parent = nullptr);

        static CloudService *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<CloudService>::get();
        }

        CloudClient *client() const { return _client; }

        /// Cached entitlement+jwt for the hosted-gateway hello
        /// (docs/HostedGateway.md §5); empty when unusable.
        QString entitlementJwt() const
        {
            return _client ? _client->entitlementJwt() : QString();
        }

        bool signedIn() const;
        bool requestingCode() const;
        bool awaitingApproval() const;
        bool busy() const;
        bool keyStoreAvailable() const;

        QString deviceName() const;
        QString deviceId() const;
        QString deviceIdShort() const;
        QString userEmail() const;
        QString userId() const;

        QString userCode() const;
        QString verificationUri() const;
        int codeRemainingSeconds() const;
        QString formattedRemainingTime() const;
        bool codeCopied() const { return _codeCopied; }

        QString statusId() const;
        QString statusLabel() const;
        bool offlineGraceActive() const;

        bool syncEnabled() const;
        bool hostedBridgeEnabled() const;
        QString validitySummary() const;

        QString errorMessage() const;

        QString serverMode() const;
        bool customServerActive() const;
        QString activeServerSummary() const;
        QString activeApiHost() const;
        QString configuredServerMode() const;
        QString configuredApiUrl() const;
        QString configuredPortalUrl() const;
        QString configuredBridgeUrl() const;
        QString configuredEntitlementPubkeys() const;
        QString effectiveApiUrl() const;
        QString effectivePortalUrl() const;
        QString effectiveBridgeUrl() const;
        bool policyLocked() const;
        bool policyInvalid() const;
        bool envOverridden() const;
        bool updateChecksSuppressedByServer() const;

        Q_INVOKABLE QString deriveBridgeUrlPreview(const QString &portalUrl) const;
        Q_INVOKABLE QVariantMap validateServerProfile(const QString &mode,
                                                      const QString &apiUrl,
                                                      const QString &portalUrl,
                                                      const QString &bridgeUrl,
                                                      const QString &pubkeys) const;
        Q_INVOKABLE QString applyServerProfile(const QString &mode,
                                               const QString &apiUrl,
                                               const QString &portalUrl,
                                               const QString &bridgeUrl,
                                               const QString &pubkeys);
        Q_INVOKABLE QString resetServerProfileToDefault();

        Q_INVOKABLE void startSignIn(const QString &customDeviceName = QString());
        Q_INVOKABLE void cancelSignIn();
        Q_INVOKABLE void copyUserCode();
        Q_INVOKABLE bool openVerificationBrowser();
        Q_INVOKABLE bool openManageAccountPortal();
        Q_INVOKABLE void updateDeviceName(const QString &name);
        Q_INVOKABLE void refreshStatus();
        /// Message for a hosted-channel connect without a usable token: signed in -> starts a refresh and says
        /// so; signed out -> asks to sign in (docs/HostedGateway.md §9.8).
        QString hostedTokenMissingMessage();
        Q_INVOKABLE void signOut();

        bool syncActive() const;
        bool syncBusy() const;
        bool syncConfigured() const;
        QString syncStateText() const;
        QString syncLastSyncText() const;
        QString syncLastError() const;
        QStringList syncCollections() const;
        bool syncIncludePasswords() const;

        /// Returns empty string on success, an error code otherwise.
        Q_INVOKABLE QString syncEnablePassphrase(const QString &passphrase);
        /// Joins an existing vault created on another device with the same passphrase.
        Q_INVOKABLE QString syncAttachVault(const QString &passphrase);
        Q_INVOKABLE QString syncEnableDeviceKey();
        Q_INVOKABLE void syncSetCollections(const QStringList &collections,
                                            bool includePasswords);
        Q_INVOKABLE void syncNow();
        Q_INVOKABLE void syncDisable();
        Q_INVOKABLE void syncWipeRemote();

        /// Populates synthetic UI state for --open-view prefs-account-code / prefs-account-pro.
        Q_INVOKABLE void previewDebugState(int mode);

    public Q_SLOTS:
        void retranslate();

    Q_SIGNALS:
        void stateChanged();
        void syncWipeFinished(bool ok);
        /// Sync rewrote the query history store; its model must rebuild and save
        /// (docs/SyncClient.md §10 R1).
        void syncHistoryChanged();

    private:
        SettingsManager *_settings = nullptr;
        PlatformInfo *_platform = nullptr;
        std::shared_ptr<ICloudCredentialStore> _credentialStore;
        CloudClient *_client = nullptr;
        SyncEngine *_sync = nullptr;
        QTimer _copiedResetTimer;
        bool _codeCopied = false;
        int _debugMode = 0; // 0 = live, 1 = pending code, 2 = signed-in with every cloud feature
    };
}
