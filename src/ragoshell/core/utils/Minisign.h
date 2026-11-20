#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include <array>
#include <cstdint>

namespace RagoShell
{
namespace Minisign
{
    struct PublicKey
    {
        std::array<std::uint8_t, 8> keyId{};
        std::array<std::uint8_t, 32> publicKey{};

        QString keyIdHex() const;
    };

    struct TrustedCommentTokens
    {
        bool hasTimestamp = false;
        qint64 timestampEpochSec = 0;
        QString rawTimestamp;
        QString file;
        QString release;
        QMap<QString, QString> claims;
    };

    struct Signature
    {
        bool prehashed = false; ///< true for "ED" (BLAKE2b-512), false for "Ed" (raw Ed25519)
        std::array<std::uint8_t, 8> keyId{};
        std::array<std::uint8_t, 64> rawSig{};
        QByteArray trustedCommentUtf8;
        QString trustedComment;
        std::array<std::uint8_t, 64> globalSig{};

        QString keyIdHex() const;
    };

    struct VerifyOptions
    {
        bool allowLegacyUnprehashed = true;
        bool requireReleaseClaims = false; ///< Require valid release, timestamp, and file claims (ci/sign-release.sh)
        bool checkClockSkew = false;       ///< When true and timestamp is present, enforce maxFutureSkewSec
        QString expectedFile;
        QString expectedRelease;
        qint64 maxFutureSkewSec = 300;
        qint64 maxAgeSec = 0;              ///< > 0 enforces (nowEpochSec - timestampEpochSec) <= maxAgeSec
        qint64 minTimestampSec = 0;        ///< > 0 rejects timestamps older than minTimestampSec (anti-rollback)
        qint64 nowEpochSec = 0;            ///< 0 uses QDateTime::currentSecsSinceEpoch()
    };

    struct VerifyResult
    {
        bool ok = false;
        bool prehashed = false;
        QString keyIdHex;
        QString trustedComment;
        TrustedCommentTokens tokens;
        QString error;
    };

    /// Parses a single-line ("RW...") or 2-line (.pub) Minisign Ed25519 public key.
    bool parsePublicKey(const QString &pubKeyText, PublicKey *out, QString *errorOut = nullptr);

    /// Parses a 4-line detached .minisig file without performing cryptographic verification.
    bool parseSignature(const QString &minisigText, Signature *out, QString *errorOut = nullptr);

    /// Parses tab-separated key:value claims from a trusted comment (e.g. timestamp, file, release).
    bool parseTrustedComment(const QString &trustedComment,
                             TrustedCommentTokens *out,
                             QString *errorOut = nullptr);

    /// Cryptographically verifies message against minisigText using pubKey (file sig + global trusted-comment sig).
    VerifyResult verify(const QByteArray &message,
                        const QString &minisigText,
                        const PublicKey &pubKey,
                        const VerifyOptions &options = VerifyOptions{});

    /// Convenience overload parsing pubKeyText first.
    VerifyResult verify(const QByteArray &message,
                        const QString &minisigText,
                        const QString &pubKeyText,
                        const VerifyOptions &options = VerifyOptions{});

    /// Resolves RAGOSHELL_MINISIGN_PUBKEY override, falling back to BrandInfo::minisignPubkey.
    QString effectiveEmbeddedPublicKey();
}
}
