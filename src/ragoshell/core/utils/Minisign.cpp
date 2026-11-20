#include "ragoshell/core/utils/Minisign.h"

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/utils/AppPaths.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#include <cstring>
#include <memory>

namespace RagoShell
{
namespace Minisign
{
namespace
{
    constexpr int kMaxMinisigChars = 8192;
    constexpr int kMaxTrustedCommentChars = 1024;

    struct OpenSslErrorScope
    {
        OpenSslErrorScope() noexcept { ERR_clear_error(); }
        ~OpenSslErrorScope() noexcept { ERR_clear_error(); }
    };

    int base64StdValue(unsigned char c) noexcept
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    }

    QString bytesToUpperHex(const std::uint8_t *data, std::size_t len)
    {
        return QString::fromLatin1(
            QByteArray(reinterpret_cast<const char *>(data), static_cast<int>(len)).toHex().toUpper());
    }

    bool decodeStrictStdBase64(const QString &b64Text,
                               int expectedBytes,
                               QByteArray *out,
                               const char *label,
                               QString *errorOut)
    {
        const QString trimmed = b64Text.trimmed();
        if (trimmed.isEmpty() || (trimmed.size() % 4) != 0)
        {
            if (errorOut)
                *errorOut = QStringLiteral("Invalid %1: malformed Base64 length.").arg(QString::fromLatin1(label));
            return false;
        }

        const QByteArray ascii = trimmed.toLatin1();
        if (ascii.size() != trimmed.size())
        {
            if (errorOut)
                *errorOut = QStringLiteral("Invalid %1: non-ASCII Base64 characters.").arg(QString::fromLatin1(label));
            return false;
        }

        QByteArray decoded;
        decoded.reserve((ascii.size() / 4) * 3);

        const int groups = ascii.size() / 4;
        for (int g = 0; g < groups; ++g)
        {
            const int base = g * 4;
            const unsigned char c0 = static_cast<unsigned char>(ascii[base]);
            const unsigned char c1 = static_cast<unsigned char>(ascii[base + 1]);
            const unsigned char c2 = static_cast<unsigned char>(ascii[base + 2]);
            const unsigned char c3 = static_cast<unsigned char>(ascii[base + 3]);

            const int v0 = base64StdValue(c0);
            const int v1 = base64StdValue(c1);
            if (v0 < 0 || v1 < 0)
            {
                if (errorOut)
                    *errorOut = QStringLiteral("Invalid %1: malformed Base64 character.").arg(QString::fromLatin1(label));
                return false;
            }

            if (c2 == '=')
            {
                if (g != groups - 1 || c3 != '=' || (v1 & 0x0f) != 0)
                {
                    if (errorOut)
                        *errorOut = QStringLiteral("Invalid %1: non-canonical Base64 padding.").arg(QString::fromLatin1(label));
                    return false;
                }
                decoded.append(static_cast<char>((v0 << 2) | (v1 >> 4)));
                break;
            }

            const int v2 = base64StdValue(c2);
            if (v2 < 0)
            {
                if (errorOut)
                    *errorOut = QStringLiteral("Invalid %1: malformed Base64 character.").arg(QString::fromLatin1(label));
                return false;
            }

            if (c3 == '=')
            {
                if (g != groups - 1 || (v2 & 0x03) != 0)
                {
                    if (errorOut)
                        *errorOut = QStringLiteral("Invalid %1: non-canonical Base64 padding.").arg(QString::fromLatin1(label));
                    return false;
                }
                decoded.append(static_cast<char>((v0 << 2) | (v1 >> 4)));
                decoded.append(static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2)));
                break;
            }

            const int v3 = base64StdValue(c3);
            if (v3 < 0)
            {
                if (errorOut)
                    *errorOut = QStringLiteral("Invalid %1: malformed Base64 character.").arg(QString::fromLatin1(label));
                return false;
            }

            decoded.append(static_cast<char>((v0 << 2) | (v1 >> 4)));
            decoded.append(static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2)));
            decoded.append(static_cast<char>(((v2 & 0x03) << 6) | v3));
        }

        if (decoded.size() != expectedBytes)
        {
            if (errorOut)
            {
                *errorOut = QStringLiteral("Invalid %1: expected %2 bytes, got %3.")
                                .arg(QString::fromLatin1(label))
                                .arg(expectedBytes)
                                .arg(decoded.size());
            }
            return false;
        }

        if (out)
            *out = decoded;
        return true;
    }

    bool verifyEd25519Raw(EVP_PKEY *pkey,
                          const unsigned char *sig64,
                          const unsigned char *msg,
                          std::size_t msgLen) noexcept
    {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        if (!ctx)
            return false;
        if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey) != 1)
            return false;
        return EVP_DigestVerify(ctx.get(), sig64, 64, msg, msgLen) == 1;
    }

    bool hasForbiddenCommentControlChars(const QString &text)
    {
        for (const QChar ch : text)
        {
            const ushort u = ch.unicode();
            if ((u < 0x20 && u != 0x09) || u == 0x7f)
                return true;
        }
        return false;
    }

    bool parseTimestampValue(const QString &rawVal, qint64 *epochSecOut)
    {
        static const QRegularExpression kIsoUtcRe(
            QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$"));
        static const QRegularExpression kUnixSecRe(QStringLiteral("^\\d{1,12}$"));

        if (kIsoUtcRe.match(rawVal).hasMatch())
        {
            const QDateTime dt = QDateTime::fromString(rawVal, Qt::ISODate);
            if (!dt.isValid())
                return false;
            const QDateTime utc = dt.toTimeZone(QTimeZone::UTC);
            if (utc.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ")) != rawVal)
                return false;
            if (epochSecOut)
                *epochSecOut = utc.toSecsSinceEpoch();
            return true;
        }

        if (kUnixSecRe.match(rawVal).hasMatch())
        {
            bool ok = false;
            const qint64 secs = rawVal.toLongLong(&ok);
            if (!ok || secs < 0)
                return false;
            if (epochSecOut)
                *epochSecOut = secs;
            return true;
        }

        return false;
    }

    bool isSafeBasename(const QString &name)
    {
        if (name.isEmpty() || name.size() > 128 || name.contains(QStringLiteral("..")))
            return false;
        static const QRegularExpression kSafeNameRe(
            QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$"));
        return kSafeNameRe.match(name).hasMatch();
    }
} // namespace

QString PublicKey::keyIdHex() const
{
    return bytesToUpperHex(keyId.data(), keyId.size());
}

QString Signature::keyIdHex() const
{
    return bytesToUpperHex(keyId.data(), keyId.size());
}

bool parsePublicKey(const QString &pubKeyText, PublicKey *out, QString *errorOut)
{
    if (pubKeyText.trimmed().isEmpty())
    {
        if (errorOut)
            *errorOut = QStringLiteral("Minisign public key is empty.");
        return false;
    }

    QString normalized = pubKeyText;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QStringList rawLines = normalized.split(QLatin1Char('\n'));

    QStringList lines;
    for (const QString &line : rawLines)
    {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            lines.append(trimmed);
    }

    QString b64Line;
    if (lines.size() == 1)
    {
        b64Line = lines[0];
    }
    else if (lines.size() == 2 && lines[0].startsWith(QStringLiteral("untrusted comment:")))
    {
        b64Line = lines[1];
    }
    else
    {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid Minisign public key format (expected \"RW...\" or 2-line .pub file).");
        return false;
    }

    QByteArray decoded;
    if (!decodeStrictStdBase64(b64Line, 42, &decoded, "Minisign public key", errorOut))
        return false;

    if (decoded[0] != 'E' || decoded[1] != 'd')
    {
        if (errorOut)
            *errorOut = QStringLiteral("Unsupported Minisign public key algorithm (expected \"Ed\").");
        return false;
    }

    if (out)
    {
        std::memcpy(out->keyId.data(), decoded.constData() + 2, 8);
        std::memcpy(out->publicKey.data(), decoded.constData() + 10, 32);
    }
    return true;
}

bool parseSignature(const QString &minisigText, Signature *out, QString *errorOut)
{
    if (minisigText.isEmpty() || minisigText.size() > kMaxMinisigChars || minisigText.contains(QChar(0)))
    {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid .minisig content.");
        return false;
    }

    QString normalized = minisigText;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (normalized.contains(QLatin1Char('\r')))
    {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid .minisig line endings.");
        return false;
    }

    QStringList lines = normalized.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.back().isEmpty())
        lines.removeLast();

    if (lines.size() != 4)
    {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid .minisig structure: expected 4 lines, got %1.").arg(lines.size());
        return false;
    }

    if (!lines[0].startsWith(QStringLiteral("untrusted comment:")))
    {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid .minisig line 1: missing \"untrusted comment:\" prefix.");
        return false;
    }

    QByteArray sigStruct;
    if (!decodeStrictStdBase64(lines[1], 74, &sigStruct, ".minisig signature line", errorOut))
        return false;

    bool prehashed = false;
    if (sigStruct[0] == 'E' && sigStruct[1] == 'D')
    {
        prehashed = true;
    }
    else if (sigStruct[0] == 'E' && sigStruct[1] == 'd')
    {
        prehashed = false;
    }
    else
    {
        if (errorOut)
            *errorOut = QStringLiteral("Unsupported .minisig signature algorithm (expected \"ED\" or \"Ed\").");
        return false;
    }

    const QString trustedPrefix = QStringLiteral("trusted comment: ");
    if (!lines[2].startsWith(trustedPrefix))
    {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid .minisig line 3: missing \"trusted comment: \" prefix.");
        return false;
    }

    const QString trustedComment = lines[2].mid(trustedPrefix.size());
    if (trustedComment.isEmpty() || trustedComment.size() > kMaxTrustedCommentChars
        || hasForbiddenCommentControlChars(trustedComment))
    {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid .minisig line 3: malformed trusted comment.");
        return false;
    }

    QByteArray globalSigBytes;
    if (!decodeStrictStdBase64(lines[3], 64, &globalSigBytes, ".minisig global signature line", errorOut))
        return false;

    if (out)
    {
        out->prehashed = prehashed;
        std::memcpy(out->keyId.data(), sigStruct.constData() + 2, 8);
        std::memcpy(out->rawSig.data(), sigStruct.constData() + 10, 64);
        out->trustedComment = trustedComment;
        out->trustedCommentUtf8 = trustedComment.toUtf8();
        std::memcpy(out->globalSig.data(), globalSigBytes.constData(), 64);
    }
    return true;
}

bool parseTrustedComment(const QString &trustedComment,
                         TrustedCommentTokens *out,
                         QString *errorOut)
{
    if (trustedComment.isEmpty() || trustedComment.size() > kMaxTrustedCommentChars
        || hasForbiddenCommentControlChars(trustedComment))
    {
        if (errorOut)
            *errorOut = QStringLiteral("Trusted comment must be a non-empty string without control characters.");
        return false;
    }

    static const QRegularExpression kKeyRe(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    static const QRegularExpression kWhitespaceRe(QStringLiteral("\\s"));

    TrustedCommentTokens parsed;
    const QStringList parts = trustedComment.split(QLatin1Char('\t'));
    for (const QString &part : parts)
    {
        if (part.isEmpty() || kWhitespaceRe.match(part).hasMatch())
        {
            if (errorOut)
                *errorOut = QStringLiteral("Malformed trusted comment segment.");
            return false;
        }
        const int colonIdx = part.indexOf(QLatin1Char(':'));
        if (colonIdx <= 0 || colonIdx == part.size() - 1)
        {
            if (errorOut)
                *errorOut = QStringLiteral("Malformed trusted comment segment (expected key:value).");
            return false;
        }
        const QString key = part.left(colonIdx);
        const QString val = part.mid(colonIdx + 1);
        if (!kKeyRe.match(key).hasMatch())
        {
            if (errorOut)
                *errorOut = QStringLiteral("Invalid trusted comment claim key \"%1\".").arg(key);
            return false;
        }
        if (parsed.claims.contains(key))
        {
            if (errorOut)
                *errorOut = QStringLiteral("Duplicate claim key \"%1\" in trusted comment.").arg(key);
            return false;
        }
        parsed.claims.insert(key, val);
    }

    if (parsed.claims.contains(QStringLiteral("timestamp")))
    {
        const QString rawTs = parsed.claims.value(QStringLiteral("timestamp"));
        qint64 epochSec = 0;
        if (!parseTimestampValue(rawTs, &epochSec))
        {
            if (errorOut)
                *errorOut = QStringLiteral("Invalid trusted comment timestamp \"%1\".").arg(rawTs);
            return false;
        }
        parsed.hasTimestamp = true;
        parsed.timestampEpochSec = epochSec;
        parsed.rawTimestamp = rawTs;
    }

    if (parsed.claims.contains(QStringLiteral("file")))
    {
        const QString fileVal = parsed.claims.value(QStringLiteral("file"));
        if (!isSafeBasename(fileVal))
        {
            if (errorOut)
                *errorOut = QStringLiteral("Invalid trusted comment file claim \"%1\".").arg(fileVal);
            return false;
        }
        parsed.file = fileVal;
    }

    if (parsed.claims.contains(QStringLiteral("release")))
    {
        parsed.release = parsed.claims.value(QStringLiteral("release"));
    }

    if (out)
        *out = parsed;
    return true;
}

VerifyResult verify(const QByteArray &message,
                    const QString &minisigText,
                    const PublicKey &pubKey,
                    const VerifyOptions &options)
{
    OpenSslErrorScope sslScope;
    VerifyResult res;

    Signature sig;
    if (!parseSignature(minisigText, &sig, &res.error))
        return res;

    if (!sig.prehashed && !options.allowLegacyUnprehashed)
    {
        res.error = QStringLiteral("Legacy unprehashed Minisign \"Ed\" signature is disabled; expected prehashed \"ED\".");
        return res;
    }

    if (CRYPTO_memcmp(sig.keyId.data(), pubKey.keyId.data(), 8) != 0)
    {
        res.error = QStringLiteral("Minisign key ID mismatch: signature uses %1, expected %2.")
                        .arg(sig.keyIdHex(), pubKey.keyIdHex());
        return res;
    }

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pubKey.publicKey.data(), pubKey.publicKey.size()),
        &EVP_PKEY_free);
    if (!pkey)
    {
        res.error = QStringLiteral("Failed to construct OpenSSL Ed25519 public key.");
        return res;
    }

    const unsigned char *rawMsgPtr = reinterpret_cast<const unsigned char *>(message.constData());
    const unsigned char emptyByte = 0;
    if (!rawMsgPtr)
        rawMsgPtr = &emptyByte;
    const std::size_t rawMsgLen = static_cast<std::size_t>(message.size());

    if (sig.prehashed)
    {
        unsigned char digest[64]{};
        unsigned int digestLen = 0;
        if (EVP_Digest(rawMsgPtr, rawMsgLen, digest, &digestLen, EVP_blake2b512(), nullptr) != 1
            || digestLen != 64)
        {
            res.error = QStringLiteral("Failed to compute BLAKE2b-512 digest for Minisign verification.");
            return res;
        }
        if (!verifyEd25519Raw(pkey.get(), sig.rawSig.data(), digest, sizeof(digest)))
        {
            res.error = QStringLiteral("Minisign Ed25519 file signature verification failed.");
            return res;
        }
    }
    else
    {
        if (!verifyEd25519Raw(pkey.get(), sig.rawSig.data(), rawMsgPtr, rawMsgLen))
        {
            res.error = QStringLiteral("Minisign Ed25519 file signature verification failed.");
            return res;
        }
    }

    QByteArray globalPayload;
    globalPayload.reserve(64 + sig.trustedCommentUtf8.size());
    globalPayload.append(reinterpret_cast<const char *>(sig.rawSig.data()), 64);
    globalPayload.append(sig.trustedCommentUtf8);

    if (!verifyEd25519Raw(pkey.get(),
                          sig.globalSig.data(),
                          reinterpret_cast<const unsigned char *>(globalPayload.constData()),
                          static_cast<std::size_t>(globalPayload.size())))
    {
        res.error = QStringLiteral("Minisign Ed25519 global signature over trusted comment failed.");
        return res;
    }

    TrustedCommentTokens parsedTokens;
    QString tokenError;
    const bool structuredTokensOk = parseTrustedComment(sig.trustedComment, &parsedTokens, &tokenError);

    const bool needsClaims = options.requireReleaseClaims
        || !options.expectedFile.isEmpty()
        || !options.expectedRelease.isEmpty()
        || options.checkClockSkew
        || options.maxAgeSec > 0
        || options.minTimestampSec > 0;

    if (!structuredTokensOk && needsClaims)
    {
        res.error = tokenError;
        return res;
    }

    if (options.requireReleaseClaims)
    {
        if (parsedTokens.release.isEmpty())
        {
            res.error = QStringLiteral("Signed trusted comment is missing required \"release:<version>\" claim.");
            return res;
        }
        if (!parsedTokens.hasTimestamp)
        {
            res.error = QStringLiteral("Signed trusted comment is missing required \"timestamp\" claim.");
            return res;
        }
        if (parsedTokens.file.isEmpty())
        {
            res.error = QStringLiteral("Signed trusted comment is missing required \"file:<basename>\" claim.");
            return res;
        }
    }

    if (!options.expectedFile.isEmpty())
    {
        if (parsedTokens.file != options.expectedFile)
        {
            res.error = QStringLiteral("Trusted comment file mismatch: signed for \"%1\", expected \"%2\".")
                            .arg(parsedTokens.file, options.expectedFile);
            return res;
        }
    }

    if (!options.expectedRelease.isEmpty())
    {
        if (parsedTokens.release != options.expectedRelease)
        {
            res.error = QStringLiteral("Trusted comment release mismatch: signed for \"%1\", expected \"%2\".")
                            .arg(parsedTokens.release, options.expectedRelease);
            return res;
        }
    }

    if (options.checkClockSkew || options.maxAgeSec > 0 || options.minTimestampSec > 0)
    {
        if (!parsedTokens.hasTimestamp)
        {
            res.error = QStringLiteral("Signed trusted comment is missing timestamp required for freshness check.");
            return res;
        }
        if (options.minTimestampSec > 0 && parsedTokens.timestampEpochSec < options.minTimestampSec)
        {
            res.error = QStringLiteral("Signed trusted comment timestamp \"%1\" precedes previously verified manifest timestamp (rollback rejected).")
                            .arg(parsedTokens.rawTimestamp);
            return res;
        }
        const qint64 nowSec = options.nowEpochSec > 0
            ? options.nowEpochSec
            : QDateTime::currentSecsSinceEpoch();
        if (options.checkClockSkew && parsedTokens.timestampEpochSec > nowSec + options.maxFutureSkewSec)
        {
            res.error = QStringLiteral("Signed trusted comment timestamp \"%1\" is in the future.")
                            .arg(parsedTokens.rawTimestamp);
            return res;
        }
        if (options.maxAgeSec > 0 && (nowSec - parsedTokens.timestampEpochSec) > options.maxAgeSec)
        {
            res.error = QStringLiteral("Signed trusted comment timestamp \"%1\" exceeds maximum age.")
                            .arg(parsedTokens.rawTimestamp);
            return res;
        }
    }

    res.ok = true;
    res.prehashed = sig.prehashed;
    res.keyIdHex = pubKey.keyIdHex();
    res.trustedComment = sig.trustedComment;
    if (structuredTokensOk)
        res.tokens = parsedTokens;
    return res;
}

VerifyResult verify(const QByteArray &message,
                    const QString &minisigText,
                    const QString &pubKeyText,
                    const VerifyOptions &options)
{
    PublicKey pubKey;
    QString err;
    if (!parsePublicKey(pubKeyText, &pubKey, &err))
    {
        VerifyResult res;
        res.error = err;
        return res;
    }
    return verify(message, minisigText, pubKey, options);
}

QString effectiveEmbeddedPublicKey()
{
    const QString fromEnv = productEnv("MINISIGN_PUBKEY").trimmed();
    if (!fromEnv.isEmpty())
        return fromEnv;
    return QString::fromUtf8(BrandInfo::minisignPubkey).trimmed();
}

} // namespace Minisign
} // namespace RagoShell
