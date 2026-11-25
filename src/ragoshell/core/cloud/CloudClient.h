#pragma once

#include "ragoshell/core/cloud/EntitlementToken.h"

#include <QByteArray>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

class QNetworkReply;

namespace RagoShell
{
    class KeyStore;
    class SettingsManager;

    enum class CloudAuthState
    {
        SignedOut,
        RequestingCode,
        AwaitingBrowserApproval,
        SignedIn
    };

    /**
     * @brief Abstract secret storage for cloud tokens so production uses OS KeyStore
     *        while unit tests inject InMemoryCloudCredentialStore (zero Keychain prompts).
     */
    class ICloudCredentialStore
    {
    public:
        static constexpr const char *kRefreshTokenSlot = "refresh_token";
        static constexpr const char *kEntitlementJwtSlot = "entitlement_jwt";
        static constexpr const char *kSyncMasterKeySlot = "sync_master_key";

        virtual ~ICloudCredentialStore() = default;
        virtual bool isAvailable() const = 0;
        virtual bool readSecret(const QString &slot, QString *valueOut, QString *errorOut = nullptr) = 0;
        virtual bool writeSecret(const QString &slot, const QString &value, QString *errorOut = nullptr) = 0;
        virtual bool removeSecret(const QString &slot, QString *errorOut = nullptr) = 0;
    };

    /**
     * @brief Production credential store backed by RagoShell::KeyStore (OS Keychain / DPAPI / libsecret).
     *        Scopes account names by appDataDir() hash when RAGOSHELL_CONFIG_DIR is overridden.
     */
    class KeyStoreCloudCredentialStore final : public ICloudCredentialStore
    {
    public:
        KeyStoreCloudCredentialStore();
        explicit KeyStoreCloudCredentialStore(std::unique_ptr<KeyStore> keyStore);
        ~KeyStoreCloudCredentialStore() override;

        bool isAvailable() const override;
        bool readSecret(const QString &slot, QString *valueOut, QString *errorOut = nullptr) override;
        bool writeSecret(const QString &slot, const QString &value, QString *errorOut = nullptr) override;
        bool removeSecret(const QString &slot, QString *errorOut = nullptr) override;

        static QString accountNameForSlot(const QString &slot);

    private:
        std::unique_ptr<KeyStore> m_keyStore;
    };

    /**
     * @brief Deterministic in-memory credential store for unit tests (CloudClient_test).
     */
    class InMemoryCloudCredentialStore final : public ICloudCredentialStore
    {
    public:
        explicit InMemoryCloudCredentialStore(bool available = true) : m_available(available) {}

        bool isAvailable() const override { return m_available; }
        bool readSecret(const QString &slot, QString *valueOut, QString *errorOut = nullptr) override;
        bool writeSecret(const QString &slot, const QString &value, QString *errorOut = nullptr) override;
        bool removeSecret(const QString &slot, QString *errorOut = nullptr) override;

        bool contains(const QString &slot) const { return m_secrets.contains(slot); }
        QString peek(const QString &slot) const { return m_secrets.value(slot); }
        int size() const { return m_secrets.size(); }

    private:
        bool m_available = true;
        QHash<QString, QString> m_secrets;
    };

    struct CloudSessionMetadata
    {
        bool signedIn = false;
        QString deviceId;
        QString deviceName;
        QString userId;
        QString userEmail;
    };

    /**
     * @brief Testable C++ cloud identity and entitlement client in rago-core (Roadmap.md §6 A1).
     *
     * Implements RFC 8628 Device Authorization Grant, single-flight rotating refresh tokens,
     * KeyStore-isolated credential storage, and offline-verifiable ES256 entitlement+jwt caching.
     * Never makes unsolicited network calls on startup or when unsigned-in.
     */
    class CloudClient : public QObject
    {
        Q_OBJECT

    public:
        using AccessTokenCallback = std::function<void(bool ok, const QString &accessToken, const QString &errorCode)>;
        using ClockFn = std::function<qint64()>;

        explicit CloudClient(std::shared_ptr<ICloudCredentialStore> credentialStore,
                             SettingsManager *settings = nullptr,
                             QObject *parent = nullptr);
        ~CloudClient() override;

        /// Restores persisted non-secret metadata and verifies any cached entitlement+jwt purely offline.
        void restoreOfflineSession();

        CloudAuthState authState() const noexcept { return m_authState; }
        bool isSignedIn() const noexcept { return m_authState == CloudAuthState::SignedIn; }
        bool isBusy() const noexcept { return m_busy; }

        QString deviceId() const { return m_metadata.deviceId; }
        QString deviceName() const { return m_metadata.deviceName; }
        void setDeviceName(const QString &name);

        QString userId() const { return m_metadata.userId; }
        QString userEmail() const { return m_metadata.userEmail; }

        // Pending RFC 8628 device flow fields (empty outside AwaitingBrowserApproval)
        QString userCode() const { return m_userCode; }
        QString verificationUri() const { return m_verificationUri; }
        QString verificationUriComplete() const { return m_verificationUriComplete; }
        int codeRemainingSeconds() const;
        int pollIntervalMs() const noexcept { return m_pollIntervalMs; }

        // Verified entitlement state (no features / None when unsigned-in or unverified)
        EntitlementState entitlementState() const noexcept { return m_entitlementState; }
        EntitlementStatus subscriptionStatus() const noexcept { return m_claims.status; }
        EntitlementFeatures features() const noexcept { return m_claims.features; }
        EntitlementQuotas quotas() const noexcept { return m_claims.quotas; }
        qint64 entitlementExpiresAt() const noexcept { return m_claims.expiresAt; }
        qint64 offlineGraceUntil() const noexcept { return m_claims.offlineGraceUntil; }
        bool isOfflineGraceActive() const noexcept { return m_entitlementState == EntitlementState::ValidOfflineGrace; }

        /// The cached entitlement+jwt for the hosted web gateway hello
        /// (docs/HostedGateway.md §5). Empty when signed out / unverified / expired;
        /// the gateway re-verifies it authoritatively anyway.
        QString entitlementJwt() const;
        bool hasActiveOnlineEntitlement() const;

        QString lastErrorCode() const { return m_lastErrorCode; }
        QString lastErrorMessage() const { return m_lastErrorMessage; }
        bool keyStoreAvailable() const;

        // Configuration, enterprise server profile, & overrides (policy, env vars, and tests)
        QString effectiveServerMode() const;
        QString effectiveApiUrl() const;
        QString effectivePortalUrl() const;
        QString effectiveBridgeUrl() const;
        bool isPolicyLocked() const;
        bool isPolicyInvalid() const;
        bool isEnvOverridden() const;
        bool isCustomServerActive() const;
        QString computeProfileFingerprint() const;
        static bool hasCustomServerProfile(const SettingsManager *settings);

        void setApiUrlOverride(const QString &apiUrl);
        void setPortalUrlOverride(const QString &portalUrl);
        void setBridgeUrlOverride(const QString &bridgeUrl);
        void setPlatformOverride(const QString &platformId, const QString &arch, const QString &appVersion);
        void setEntitlementVerifier(const EntitlementVerifier &verifier);
        void setClockForTest(ClockFn clockSecondsFn);
        void setMinPollIntervalMsForTest(int minIntervalMs, int slowDownStepMs = 5000);
        void applyServerProfileChange();

        // User-initiated actions
        void startDeviceSignIn(const QString &customDeviceName = QString());
        void cancelDeviceSignIn();
        void refreshAccountAndEntitlements();
        void signOut();

        /// Automatic entitlement refresh (docs/HostedGateway.md §9.8): off by default, the app turns it on.
        /// A check every kAutoRefreshCheckMs (and one kAutoRefreshFirstCheckMs after enabling) re-verifies a
        /// cached token whose exp has passed and refreshes once the token is past half of its lifetime.
        void setAutoRefreshEnabled(bool enabled);
        bool autoRefreshEnabled() const noexcept { return m_autoRefreshEnabled; }
        /// One check now (the timer calls it; tests call it directly).
        void checkEntitlementRefresh();
        static constexpr int kAutoRefreshCheckMs = 15 * 60 * 1000;
        static constexpr int kAutoRefreshFirstCheckMs = 10 * 1000;
        static constexpr qint64 kAutoRefreshMinBackoffSec = 5 * 60;
        static constexpr qint64 kAutoRefreshMaxBackoffSec = 60 * 60;

        /// Single-flight access token provider for authenticated API calls (Sync, Hosted AI, Portal).
        void withValidAccessToken(AccessTokenCallback callback);

        /// Force-expires RAM access token in tests to exercise refresh rotation.
        void expireAccessTokenInMemoryForTest();

        // Security URL validators
        static bool isAllowedApiBaseUrl(const QString &urlText);
        static bool isAllowedBridgeUrl(const QString &urlText);
        static QString deriveBridgeUrlFromPortal(const QString &portalBaseUrl);
        static bool isAllowedVerificationUrl(const QString &candidateUrl,
                                             const QString &effectiveApiBaseUrl,
                                             const QString &effectivePortalBaseUrl);
        static QString sanitizeDeviceName(const QString &rawName);
        static QString defaultDeviceName();
        static QString normalizedPlatformId(const QString &rawOs);

    Q_SIGNALS:
        void stateChanged();
        void signInSucceeded();
        void signInFailed(const QString &errorCode, const QString &errorMessage);
        void entitlementsUpdated();
        void sessionRevoked(const QString &reasonCode);
        void signedOut();
        void logoutRequestFinished(bool networkSucceeded);
        void serverProfileSwitched();

    private:
        struct ApiErrorInfo
        {
            QString code;
            QString message;
        };

        struct EffectiveProfile
        {
            QString mode = QStringLiteral("default");
            QString apiUrl;
            QString portalUrl;
            QString bridgeUrl;
            EntitlementVerifier verifier;
            bool policyLocked = false;
            bool policyInvalid = false;
            bool envOverridden = false;
            bool customServerActive = false;
        };

        qint64 nowUnixSec() const;
        void reloadVerifierFromConfig();
        EffectiveProfile resolveEffectiveProfile() const;
        static EffectiveProfile resolveProfileForSettings(
            const SettingsManager *settings,
            const QString &apiUrlOverride,
            const QString &portalUrlOverride,
            const QString &bridgeUrlOverride,
            bool hasCustomVerifier,
            const EntitlementVerifier &customVerifier);
        QString defaultEmbeddedProfileFingerprint() const;
        bool hasTestOverrides() const noexcept;
        bool hasStoredCloudSecrets() const;
        bool isNetworkBlockedByPolicyOrSettings(QString *codeOut = nullptr, QString *messageOut = nullptr) const;
        void wipeLocalStateForServerSwitch(const QString &newFingerprint);
        void loadMetadataFromSettings();
        void saveMetadataToSettings(const QString *overrideFingerprint = nullptr);
        void verifyAndApplyCachedEntitlementOffline();

        void pollDeviceToken();
        void updateCountdownTick();
        void clearPendingDeviceFlow(bool cleanseCode = true);

        void performTokenRefresh();
        void finishTokenWaiters(bool ok, const QString &accessToken, const QString &errorCode);
        void handleSessionRevoked(const QString &reasonCode, const QString &message);

        void fetchMeProfile(const QString &accessToken, bool allowRefreshRetry);
        void fetchEntitlementsWithToken(const QString &accessToken, bool allowRefreshRetry);

        QNetworkRequest buildApiRequest(const QString &path,
                                        bool jsonBody,
                                        int timeoutMs = 15000) const;
        static ApiErrorInfo parseApiError(const QByteArray &body, int httpStatus);
        static void cleanseString(QString *secret);
        static void cleanseByteArray(QByteArray *secret);

        std::shared_ptr<ICloudCredentialStore> m_credentialStore;
        SettingsManager *m_settings = nullptr;
        QNetworkAccessManager m_nam;

        EntitlementVerifier m_verifier;
        bool m_customVerifierSet = false;
        EntitlementVerifier m_customVerifier;
        ClockFn m_clockSecFn;

        QString m_apiUrlOverride;
        QString m_portalUrlOverride;
        QString m_bridgeUrlOverride;
        QString m_platformOverride;
        QString m_archOverride;
        QString m_appVersionOverride;
        int m_minPollIntervalMs = 5000;
        int m_slowDownStepMs = 5000;

        CloudAuthState m_authState = CloudAuthState::SignedOut;
        CloudSessionMetadata m_metadata;
        bool m_busy = false;

        // RAM-only short-lived secrets (never written to disk)
        QString m_accessToken;
        qint64 m_accessTokenExpiresAtSec = 0;
        QString m_volatileRefreshToken; ///< Session-only refresh_token when KeyStore::isAvailable() is false
        QString m_deviceCode;

        // Pending RFC 8628 UI state
        QString m_userCode;
        QString m_verificationUri;
        QString m_verificationUriComplete;
        qint64 m_codeExpiresAtSec = 0;
        int m_pollIntervalMs = 5000;
        QTimer m_pollTimer;
        QTimer m_countdownTimer;
        QPointer<QNetworkReply> m_deviceFlowReply;

        // Single-flight refresh queue
        bool m_refreshInFlight = false;
        QPointer<QNetworkReply> m_refreshReply;
        std::vector<AccessTokenCallback> m_tokenWaiters;

        // Active profile / entitlement replies
        QPointer<QNetworkReply> m_meReply;
        QPointer<QNetworkReply> m_entitlementsReply;

        // Verified entitlement claims
        EntitlementState m_entitlementState = EntitlementState::Invalid;
        EntitlementClaims m_claims;

        // Automatic entitlement refresh (§9.8)
        bool m_autoRefreshEnabled = false;
        QTimer m_autoRefreshTimer;
        qint64 m_autoRefreshNextAttemptAt = 0;
        qint64 m_autoRefreshBackoffSec = 0;

        QString m_lastErrorCode;
        QString m_lastErrorMessage;
    };
}
