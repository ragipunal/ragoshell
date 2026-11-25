#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>

typedef struct evp_pkey_st EVP_PKEY;

namespace RagoShell
{
    enum class EntitlementState
    {
        ValidOnline,
        ValidOfflineGrace,
        Expired,
        Invalid
    };

    enum class EntitlementStatus
    {
        None,
        Trialing,
        Active,
        PastDue,
        Canceled
    };

    struct EntitlementFeatures
    {
        bool sync = false;
        bool hostedAi = false;
        bool hostedBridge = false;
        bool prioritySupport = false;
    };

    struct EntitlementQuotas
    {
        int syncMaxCollections = 0;
        int syncMaxBlobBytes = 0;
        qint64 aiMonthlyTokenQuota = 0;
    };

    struct EntitlementClaims
    {
        QString keyId;
        QString issuer;
        QString audience;
        QString subject;
        QString deviceId;
        QString jwtId;
        qint64 issuedAt = 0;
        qint64 notBefore = 0;
        qint64 expiresAt = 0;
        qint64 offlineGraceUntil = 0;
        EntitlementStatus status = EntitlementStatus::None;
        EntitlementFeatures features;
        EntitlementQuotas quotas;
    };

    struct EntitlementVerifyOptions
    {
        QString expectedIssuer;
        QString expectedAudience;
        QString expectedDeviceId;
        qint64 clockSkewSec = 30;
    };

    struct EntitlementVerifyResult
    {
        EntitlementState state = EntitlementState::Invalid;
        EntitlementClaims claims;
        QString errorCode;

        bool isUsable() const noexcept
        {
            return state == EntitlementState::ValidOnline || state == EntitlementState::ValidOfflineGrace;
        }
    };

    /**
     * @brief Offline-capable ES256 (P-256 / prime256v1) verifier for cloud entitlement+jwt tokens.
     *
     * Enforces RFC 7518 §3.4 64-byte IEEE P1363 R||S signature verification via OpenSSL 3 EVP_DigestVerify,
     * strict token-type ("entitlement+jwt") and audience (BrandInfo::bundleId) isolation from online at+jwt
     * access tokens, kid-indexed zero-downtime key rotation, and 7-day online / 14-day offline grace windows.
     */
    class EntitlementVerifier
    {
    public:
        static constexpr qint64 kOnlineValiditySec = 7LL * 24 * 3600;       // 604800s (7 days)
        static constexpr qint64 kMaxOfflineGraceSec = 14LL * 24 * 3600;     // 1209600s (14 days)
        static constexpr int kMaxCompactTokenBytes = 4096;
        static constexpr int kMaxKeyRingKeys = 16;
        static constexpr int kMaxKeyRingStringChars = 8192;

        EntitlementVerifier() = default;

        static QString defaultApiIssuer();
        static QString defaultAppAudience();

        /// Builds a verifier from BrandInfo::entitlementPubkeys (empty ring succeeds with keyCount() == 0).
        static EntitlementVerifier fromEmbeddedDefaults(bool *okOut = nullptr, QString *errorOut = nullptr);

        /// Parses semicolon-separated "<kid>:<standard-base64-spki-der>" P-256 public key entries.
        static EntitlementVerifier fromKeyRingString(const QString &ringSpec, bool *okOut = nullptr, QString *errorOut = nullptr);

        /// Validates a custom server key ring (same grammar as fromKeyRingString, requiring 1..kMaxKeyRingKeys keys).
        static bool isValidCustomKeyRing(const QString &ringSpec, QString *errorOut = nullptr);

        /// Registers one P-256 SubjectPublicKeyInfo DER public key under kid. Rejects duplicate kids or non-P256 keys.
        bool addPublicKeySpkiDer(const QString &kid, const QByteArray &spkiDer, QString *errorOut = nullptr);

        int keyCount() const noexcept { return static_cast<int>(m_keys.size()); }
        bool hasKey(const QString &kid) const { return m_keys.contains(kid); }
        QStringList keyIds() const;
        /// Canonical sorted "<kid>:<base64-spki-der>;..." representation for profile fingerprinting.
        QString canonicalKeyRingString() const;

        EntitlementVerifyResult verify(
            const QString &compactJwt,
            qint64 nowUnixSec,
            const EntitlementVerifyOptions &options = EntitlementVerifyOptions()) const;

        /// Strict RFC 4648 §5 Base64URL decoder (no padding, rejects non-zero trailing padding bits).
        static bool decodeStrictBase64Url(const QByteArray &input, QByteArray *out);

        /// Strict RFC 4648 §4 standard Base64 decoder (rejects whitespace and non-zero padding bits).
        static bool decodeStrictBase64Std(const QByteArray &input, QByteArray *out);

        static bool isValidKeyId(const QString &kid);
        static bool isCanonicalUuid(const QString &value);

    private:
        QHash<QString, std::shared_ptr<EVP_PKEY>> m_keys;
    };
}
