#include "ragoshell/core/cloud/EntitlementToken.h"

#include "ragoshell/core/BrandInfo.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace RagoShell
{
    namespace
    {
        constexpr qint64 kMaxSafeJsonInt = (1LL << 53) - 1; // 9007199254740991 (exact in IEEE-754 double)
        constexpr int kMaxSpkiDerBytes = 512;
        constexpr int kMaxJsonNestingDepth = 8;
        constexpr qint64 kMaxSyncCollectionsQuota = 10000;
        constexpr qint64 kMaxSyncBlobBytesQuota = 100LL * 1024 * 1024;
        constexpr qint64 kMaxAiMonthlyTokenQuota = 1'000'000'000'000LL;

        struct OpenSslErrorScope
        {
            OpenSslErrorScope() noexcept { ERR_clear_error(); }
            ~OpenSslErrorScope() noexcept { ERR_clear_error(); }
        };

        struct OpenSslFreeDeleter
        {
            void operator()(unsigned char *p) const noexcept
            {
                if (p)
                    OPENSSL_free(p);
            }
        };

        int base64UrlAlphabetValue(unsigned char c) noexcept
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '-')
                return 62;
            if (c == '_')
                return 63;
            return -1;
        }

        int base64StdAlphabetValue(unsigned char c) noexcept
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

        void skipJsonWhitespace(const char *&cur, const char *end) noexcept
        {
            while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n'))
                ++cur;
        }

        bool scanJsonStringToken(const char *&cur, const char *end, QByteArray *rawTokenOut)
        {
            if (cur >= end || *cur != '"')
                return false;
            const char *start = cur;
            ++cur;
            while (cur < end)
            {
                const unsigned char ch = static_cast<unsigned char>(*cur);
                if (ch < 0x20)
                    return false;
                if (ch == '"')
                {
                    ++cur;
                    if (rawTokenOut)
                        *rawTokenOut = QByteArray(start, static_cast<int>(cur - start));
                    return true;
                }
                if (ch == '\\')
                {
                    ++cur;
                    if (cur >= end)
                        return false;
                }
                ++cur;
            }
            return false;
        }

        bool decodeJsonStringLiteral(const QByteArray &quotedLiteral, QString *decodedOut)
        {
            QByteArray wrapped;
            wrapped.reserve(quotedLiteral.size() + 2);
            wrapped.append('[');
            wrapped.append(quotedLiteral);
            wrapped.append(']');

            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(wrapped, &err);
            if (err.error != QJsonParseError::NoError || !doc.isArray())
                return false;
            const QJsonArray arr = doc.array();
            if (arr.size() != 1 || !arr.at(0).isString())
                return false;
            if (decodedOut)
                *decodedOut = arr.at(0).toString();
            return true;
        }

        // Verifies that no JSON object at any nesting level contains duplicate property names
        // (including Unicode-escaped duplicates such as "\u0061lg") and that numbers do not use
        // fractional or exponent notation when integral values are expected.
        bool checkNoDuplicateKeysValue(const char *&cur, const char *end, int depth)
        {
            if (depth > kMaxJsonNestingDepth)
                return false;
            skipJsonWhitespace(cur, end);
            if (cur >= end)
                return false;

            const char ch = *cur;
            if (ch == '{')
            {
                ++cur;
                skipJsonWhitespace(cur, end);
                if (cur < end && *cur == '}')
                {
                    ++cur;
                    return true;
                }

                QSet<QString> seenKeys;
                while (cur < end)
                {
                    skipJsonWhitespace(cur, end);
                    QByteArray rawKeyToken;
                    if (!scanJsonStringToken(cur, end, &rawKeyToken))
                        return false;

                    QString decodedKey;
                    if (!decodeJsonStringLiteral(rawKeyToken, &decodedKey))
                        return false;
                    if (seenKeys.contains(decodedKey))
                        return false;
                    seenKeys.insert(decodedKey);

                    skipJsonWhitespace(cur, end);
                    if (cur >= end || *cur != ':')
                        return false;
                    ++cur;

                    if (!checkNoDuplicateKeysValue(cur, end, depth + 1))
                        return false;

                    skipJsonWhitespace(cur, end);
                    if (cur < end && *cur == ',')
                    {
                        ++cur;
                        continue;
                    }
                    if (cur < end && *cur == '}')
                    {
                        ++cur;
                        return true;
                    }
                    return false;
                }
                return false;
            }

            if (ch == '[')
            {
                ++cur;
                skipJsonWhitespace(cur, end);
                if (cur < end && *cur == ']')
                {
                    ++cur;
                    return true;
                }
                while (cur < end)
                {
                    if (!checkNoDuplicateKeysValue(cur, end, depth + 1))
                        return false;
                    skipJsonWhitespace(cur, end);
                    if (cur < end && *cur == ',')
                    {
                        ++cur;
                        continue;
                    }
                    if (cur < end && *cur == ']')
                    {
                        ++cur;
                        return true;
                    }
                    return false;
                }
                return false;
            }

            if (ch == '"')
                return scanJsonStringToken(cur, end, nullptr);

            // Primitive literal (true, false, null, or number): scan up to structural delimiter.
            const char *primStart = cur;
            while (cur < end)
            {
                const char c = *cur;
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == '}' || c == ']')
                    break;
                ++cur;
            }
            if (cur == primStart)
                return false;

            const QByteArray token(primStart, static_cast<int>(cur - primStart));
            if (token != QByteArrayLiteral("true") && token != QByteArrayLiteral("false") && token != QByteArrayLiteral("null"))
            {
                // Reject fractional/scientific notation numbers so integer claims cannot smuggle floats like 20.0 or 1e5.
                if (token.contains('.') || token.contains('e') || token.contains('E'))
                    return false;
            }
            return true;
        }

        bool parseStrictJsonObject(const QByteArray &raw, QJsonObject *objOut)
        {
            if (raw.isEmpty())
                return false;
            const char *cur = raw.constData();
            const char *end = cur + raw.size();
            if (!checkNoDuplicateKeysValue(cur, end, 0))
                return false;
            skipJsonWhitespace(cur, end);
            if (cur != end)
                return false;

            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return false;
            if (objOut)
                *objOut = doc.object();
            return true;
        }

        bool extractStrictString(const QJsonObject &obj, QLatin1String key, QString *out)
        {
            const auto it = obj.find(key);
            if (it == obj.end() || !it->isString())
                return false;
            if (out)
                *out = it->toString();
            return true;
        }

        bool extractStrictBool(const QJsonObject &obj, QLatin1String key, bool *out)
        {
            const auto it = obj.find(key);
            if (it == obj.end() || !it->isBool())
                return false;
            if (out)
                *out = it->toBool();
            return true;
        }

        bool extractStrictInt64Bounded(const QJsonObject &obj, QLatin1String key, qint64 minVal, qint64 maxVal, qint64 *out)
        {
            const auto it = obj.find(key);
            if (it == obj.end() || !it->isDouble())
                return false;

            const qint64 effectiveMax = std::min(maxVal, kMaxSafeJsonInt);
            const qint64 effectiveMin = std::max(minVal, -kMaxSafeJsonInt);
            const double d = it->toDouble();
            if (!std::isfinite(d) || std::floor(d) != d)
                return false;
            if (d < static_cast<double>(effectiveMin) || d > static_cast<double>(effectiveMax))
                return false;

            if (out)
                *out = static_cast<qint64>(d);
            return true;
        }

        bool isSafeJwtId(const QString &jti)
        {
            if (jti.size() < 8 || jti.size() > 128)
                return false;
            for (QChar ch : jti)
            {
                const ushort u = ch.unicode();
                if (u < 0x21 || u > 0x7e)
                    return false;
            }
            return true;
        }

        bool verifyP256Es256Signature(
            EVP_PKEY *pubKey,
            const QByteArray &signingInput,
            const QByteArray &rawSig64)
        {
            if (!pubKey || rawSig64.size() != 64)
                return false;

            std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)> group(
                EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1), &EC_GROUP_free);
            if (!group)
                return false;

            const BIGNUM *curveOrder = EC_GROUP_get0_order(group.get());
            if (!curveOrder)
                return false;

            const auto *sigBytes = reinterpret_cast<const unsigned char *>(rawSig64.constData());
            std::unique_ptr<BIGNUM, decltype(&BN_free)> rBn(BN_bin2bn(sigBytes, 32, nullptr), &BN_free);
            std::unique_ptr<BIGNUM, decltype(&BN_free)> sBn(BN_bin2bn(sigBytes + 32, 32, nullptr), &BN_free);
            if (!rBn || !sBn)
                return false;

            // Enforce 1 <= R < N and 1 <= S < N before DER encoding.
            if (BN_is_zero(rBn.get()) || BN_is_negative(rBn.get()) || BN_cmp(rBn.get(), curveOrder) >= 0)
                return false;
            if (BN_is_zero(sBn.get()) || BN_is_negative(sBn.get()) || BN_cmp(sBn.get(), curveOrder) >= 0)
                return false;

            std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> ecSig(ECDSA_SIG_new(), &ECDSA_SIG_free);
            if (!ecSig)
                return false;
            if (ECDSA_SIG_set0(ecSig.get(), rBn.get(), sBn.get()) != 1)
                return false;
            // Ownership transferred to ecSig on ECDSA_SIG_set0 == 1.
            rBn.release();
            sBn.release();

            unsigned char *rawDer = nullptr;
            const int derLen = i2d_ECDSA_SIG(ecSig.get(), &rawDer);
            std::unique_ptr<unsigned char, OpenSslFreeDeleter> derGuard(rawDer);
            if (derLen <= 0 || !rawDer)
                return false;

            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> mdCtx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
            if (!mdCtx)
                return false;
            if (EVP_DigestVerifyInit(mdCtx.get(), nullptr, EVP_sha256(), nullptr, pubKey) != 1)
                return false;

            const auto *msgPtr = reinterpret_cast<const unsigned char *>(signingInput.constData());
            return EVP_DigestVerify(mdCtx.get(), rawDer, static_cast<size_t>(derLen), msgPtr, static_cast<size_t>(signingInput.size())) == 1;
        }
    } // namespace

    QString EntitlementVerifier::defaultApiIssuer()
    {
        return QStringLiteral("https://api.") + QString::fromUtf8(BrandInfo::domain).trimmed();
    }

    QString EntitlementVerifier::defaultAppAudience()
    {
        return QString::fromUtf8(BrandInfo::bundleId).trimmed().toLower();
    }

    bool EntitlementVerifier::isValidKeyId(const QString &kid)
    {
        if (kid.isEmpty() || kid.size() > 64)
            return false;
        for (QChar ch : kid)
        {
            const ushort u = ch.unicode();
            const bool ok = (u >= 'a' && u <= 'z') ||
                            (u >= 'A' && u <= 'Z') ||
                            (u >= '0' && u <= '9') ||
                            u == '.' || u == '_' || u == '-';
            if (!ok)
                return false;
        }
        return true;
    }

    bool EntitlementVerifier::isCanonicalUuid(const QString &value)
    {
        if (value.size() != 36)
            return false;
        for (int i = 0; i < 36; ++i)
        {
            const ushort u = value.at(i).unicode();
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (u != '-')
                    return false;
            }
            else
            {
                const bool isHex = (u >= '0' && u <= '9') ||
                                   (u >= 'a' && u <= 'f') ||
                                   (u >= 'A' && u <= 'F');
                if (!isHex)
                    return false;
            }
        }
        return true;
    }

    bool EntitlementVerifier::decodeStrictBase64Url(const QByteArray &input, QByteArray *out)
    {
        const qsizetype len = input.size();
        const qsizetype rem = len % 4;
        if (rem == 1)
            return false;

        QByteArray decoded;
        decoded.reserve((len / 4) * 3 + (rem > 1 ? rem - 1 : 0));

        qsizetype i = 0;
        while (i + 4 <= len)
        {
            const int v0 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i)));
            const int v1 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i + 1)));
            const int v2 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i + 2)));
            const int v3 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i + 3)));
            if ((v0 | v1 | v2 | v3) < 0)
                return false;

            const uint32_t triple = (static_cast<uint32_t>(v0) << 18) |
                                    (static_cast<uint32_t>(v1) << 12) |
                                    (static_cast<uint32_t>(v2) << 6) |
                                    static_cast<uint32_t>(v3);
            decoded.append(static_cast<char>((triple >> 16) & 0xFF));
            decoded.append(static_cast<char>((triple >> 8) & 0xFF));
            decoded.append(static_cast<char>(triple & 0xFF));
            i += 4;
        }

        if (rem == 2)
        {
            const int v0 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i)));
            const int v1 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i + 1)));
            if ((v0 | v1) < 0)
                return false;
            // Reject non-zero unused 4 trailing bits (RFC 4648 §3.5 canonical check).
            if ((v1 & 0x0F) != 0)
                return false;
            decoded.append(static_cast<char>(((v0 << 2) | (v1 >> 4)) & 0xFF));
        }
        else if (rem == 3)
        {
            const int v0 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i)));
            const int v1 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i + 1)));
            const int v2 = base64UrlAlphabetValue(static_cast<unsigned char>(input.at(i + 2)));
            if ((v0 | v1 | v2) < 0)
                return false;
            // Reject non-zero unused 2 trailing bits.
            if ((v2 & 0x03) != 0)
                return false;
            decoded.append(static_cast<char>(((v0 << 2) | (v1 >> 4)) & 0xFF));
            decoded.append(static_cast<char>(((v1 << 4) | (v2 >> 2)) & 0xFF));
        }

        if (out)
            *out = decoded;
        return true;
    }

    bool EntitlementVerifier::decodeStrictBase64Std(const QByteArray &input, QByteArray *out)
    {
        const qsizetype len = input.size();
        if (len == 0 || (len % 4) != 0)
            return false;

        qsizetype padCount = 0;
        if (input.at(len - 1) == '=')
        {
            ++padCount;
            if (input.at(len - 2) == '=')
                ++padCount;
        }

        const qsizetype dataEnd = len - padCount;
        QByteArray decoded;
        decoded.reserve(((len / 4) * 3) - padCount);

        qsizetype i = 0;
        while (i + 4 <= dataEnd)
        {
            const int v0 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i)));
            const int v1 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i + 1)));
            const int v2 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i + 2)));
            const int v3 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i + 3)));
            if ((v0 | v1 | v2 | v3) < 0)
                return false;

            const uint32_t triple = (static_cast<uint32_t>(v0) << 18) |
                                    (static_cast<uint32_t>(v1) << 12) |
                                    (static_cast<uint32_t>(v2) << 6) |
                                    static_cast<uint32_t>(v3);
            decoded.append(static_cast<char>((triple >> 16) & 0xFF));
            decoded.append(static_cast<char>((triple >> 8) & 0xFF));
            decoded.append(static_cast<char>(triple & 0xFF));
            i += 4;
        }

        if (padCount == 2)
        {
            const int v0 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i)));
            const int v1 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i + 1)));
            if ((v0 | v1) < 0 || (v1 & 0x0F) != 0)
                return false;
            decoded.append(static_cast<char>(((v0 << 2) | (v1 >> 4)) & 0xFF));
        }
        else if (padCount == 1)
        {
            const int v0 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i)));
            const int v1 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i + 1)));
            const int v2 = base64StdAlphabetValue(static_cast<unsigned char>(input.at(i + 2)));
            if ((v0 | v1 | v2) < 0 || (v2 & 0x03) != 0)
                return false;
            decoded.append(static_cast<char>(((v0 << 2) | (v1 >> 4)) & 0xFF));
            decoded.append(static_cast<char>(((v1 << 4) | (v2 >> 2)) & 0xFF));
        }

        if (out)
            *out = decoded;
        return true;
    }

    EntitlementVerifier EntitlementVerifier::fromEmbeddedDefaults(bool *okOut, QString *errorOut)
    {
        return fromKeyRingString(QString::fromUtf8(BrandInfo::entitlementPubkeys), okOut, errorOut);
    }

    EntitlementVerifier EntitlementVerifier::fromKeyRingString(const QString &ringSpec, bool *okOut, QString *errorOut)
    {
        if (ringSpec.size() > kMaxKeyRingStringChars)
        {
            if (okOut)
                *okOut = false;
            if (errorOut)
                *errorOut = QStringLiteral("keyring_too_large");
            return EntitlementVerifier();
        }

        EntitlementVerifier verifier;
        const QStringList entries = ringSpec.split(QLatin1Char(';'));
        for (const QString &rawEntry : entries)
        {
            const QString trimmed = rawEntry.trimmed();
            if (trimmed.isEmpty())
                continue;

            if (verifier.keyCount() >= kMaxKeyRingKeys)
            {
                if (okOut)
                    *okOut = false;
                if (errorOut)
                    *errorOut = QStringLiteral("too_many_keys");
                return EntitlementVerifier();
            }

            const int colonIdx = trimmed.indexOf(QLatin1Char(':'));
            if (colonIdx <= 0 || colonIdx >= trimmed.size() - 1)
            {
                if (okOut)
                    *okOut = false;
                if (errorOut)
                    *errorOut = QStringLiteral("malformed_keyring_entry");
                return EntitlementVerifier();
            }

            const QString kid = trimmed.left(colonIdx).trimmed();
            const QString b64Part = trimmed.mid(colonIdx + 1).trimmed();

            bool asciiOk = true;
            for (QChar ch : b64Part)
            {
                if (ch.unicode() > 0x7F)
                {
                    asciiOk = false;
                    break;
                }
            }
            QByteArray spkiDer;
            if (!asciiOk || !decodeStrictBase64Std(b64Part.toLatin1(), &spkiDer))
            {
                if (okOut)
                    *okOut = false;
                if (errorOut)
                    *errorOut = QStringLiteral("invalid_spki_base64");
                return EntitlementVerifier();
            }

            QString addErr;
            if (!verifier.addPublicKeySpkiDer(kid, spkiDer, &addErr))
            {
                if (okOut)
                    *okOut = false;
                if (errorOut)
                    *errorOut = addErr;
                return EntitlementVerifier();
            }
        }

        if (okOut)
            *okOut = true;
        if (errorOut)
            errorOut->clear();
        return verifier;
    }

    bool EntitlementVerifier::addPublicKeySpkiDer(const QString &kid, const QByteArray &spkiDer, QString *errorOut)
    {
        const OpenSslErrorScope errGuard;

        const QString cleanKid = kid.trimmed();
        if (!isValidKeyId(cleanKid))
        {
            if (errorOut)
                *errorOut = QStringLiteral("invalid_kid");
            return false;
        }
        if (m_keys.contains(cleanKid))
        {
            if (errorOut)
                *errorOut = QStringLiteral("duplicate_kid");
            return false;
        }
        if (spkiDer.isEmpty() || spkiDer.size() > kMaxSpkiDerBytes)
        {
            if (errorOut)
                *errorOut = QStringLiteral("spki_size_out_of_bounds");
            return false;
        }

        const auto *derStart = reinterpret_cast<const unsigned char *>(spkiDer.constData());
        const unsigned char *cursor = derStart;
        EVP_PKEY *rawKey = d2i_PUBKEY(nullptr, &cursor, static_cast<long>(spkiDer.size()));
        std::shared_ptr<EVP_PKEY> pkey(rawKey, &EVP_PKEY_free);
        if (!pkey || cursor != derStart + spkiDer.size())
        {
            if (errorOut)
                *errorOut = QStringLiteral("invalid_spki_der");
            return false;
        }

        if (EVP_PKEY_get_base_id(pkey.get()) != EVP_PKEY_EC)
        {
            if (errorOut)
                *errorOut = QStringLiteral("unsupported_key_type");
            return false;
        }

        char groupName[64] = {};
        size_t groupLen = 0;
        if (EVP_PKEY_get_utf8_string_param(pkey.get(), OSSL_PKEY_PARAM_GROUP_NAME, groupName, sizeof(groupName), &groupLen) != 1)
        {
            if (errorOut)
                *errorOut = QStringLiteral("missing_ec_group");
            return false;
        }
        if (QByteArray(groupName, static_cast<int>(groupLen)) != QByteArrayLiteral("prime256v1"))
        {
            if (errorOut)
                *errorOut = QStringLiteral("unsupported_ec_curve");
            return false;
        }

        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> pctx(
            EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr), &EVP_PKEY_CTX_free);
        if (!pctx || EVP_PKEY_public_check(pctx.get()) != 1)
        {
            if (errorOut)
                *errorOut = QStringLiteral("invalid_ec_public_point");
            return false;
        }

        m_keys.insert(cleanKid, std::move(pkey));
        if (errorOut)
            errorOut->clear();
        return true;
    }

    QStringList EntitlementVerifier::keyIds() const
    {
        QStringList ids = m_keys.keys();
        ids.sort();
        return ids;
    }

    bool EntitlementVerifier::isValidCustomKeyRing(const QString &ringSpec, QString *errorOut)
    {
        bool ok = false;
        QString err;
        const EntitlementVerifier v = fromKeyRingString(ringSpec, &ok, &err);
        if (!ok)
        {
            if (errorOut)
                *errorOut = err;
            return false;
        }
        if (v.keyCount() <= 0)
        {
            if (errorOut)
                *errorOut = QStringLiteral("empty_keyring");
            return false;
        }
        if (errorOut)
            errorOut->clear();
        return true;
    }

    QString EntitlementVerifier::canonicalKeyRingString() const
    {
        const OpenSslErrorScope errGuard;
        QStringList parts;
        const QStringList ids = keyIds();
        parts.reserve(ids.size());
        for (const QString &kid : ids)
        {
            const auto &pkey = m_keys.value(kid);
            if (!pkey)
                continue;
            unsigned char *der = nullptr;
            const int len = i2d_PUBKEY(pkey.get(), &der);
            std::unique_ptr<unsigned char, OpenSslFreeDeleter> guard(der);
            if (len <= 0 || !der)
                continue;
            const QByteArray derBytes(reinterpret_cast<const char *>(der), len);
            parts.append(kid + QLatin1Char(':') + QString::fromLatin1(derBytes.toBase64()));
        }
        return parts.join(QLatin1Char(';'));
    }

    EntitlementVerifyResult EntitlementVerifier::verify(
        const QString &compactJwt,
        qint64 nowUnixSec,
        const EntitlementVerifyOptions &options) const
    {
        const OpenSslErrorScope errGuard;

        auto failInvalid = [](const QString &code) {
            EntitlementVerifyResult r;
            r.state = EntitlementState::Invalid;
            r.errorCode = code;
            return r;
        };

        if (m_keys.isEmpty())
            return failInvalid(QStringLiteral("empty_keyring"));
        if (nowUnixSec <= 0)
            return failInvalid(QStringLiteral("invalid_clock"));

        if (compactJwt.isEmpty() || compactJwt.size() > kMaxCompactTokenBytes)
            return failInvalid(QStringLiteral("malformed_token"));

        QByteArray asciiJwt;
        asciiJwt.reserve(compactJwt.size());
        for (QChar ch : compactJwt)
        {
            const ushort u = ch.unicode();
            if (u <= 0x20 || u >= 0x7F)
                return failInvalid(QStringLiteral("malformed_token"));
            asciiJwt.append(static_cast<char>(u));
        }

        const int firstDot = asciiJwt.indexOf('.');
        const int lastDot = asciiJwt.lastIndexOf('.');
        if (firstDot <= 0 || lastDot <= firstDot + 1 || lastDot >= asciiJwt.size() - 1)
            return failInvalid(QStringLiteral("malformed_token"));
        if (asciiJwt.indexOf('.', firstDot + 1) != lastDot)
            return failInvalid(QStringLiteral("malformed_token"));

        const QByteArray hdrSeg = asciiJwt.left(firstDot);
        const QByteArray payloadSeg = asciiJwt.mid(firstDot + 1, lastDot - firstDot - 1);
        const QByteArray sigSeg = asciiJwt.mid(lastDot + 1);

        QByteArray hdrBytes;
        if (!decodeStrictBase64Url(hdrSeg, &hdrBytes))
            return failInvalid(QStringLiteral("invalid_header_encoding"));

        QJsonObject hdrObj;
        if (!parseStrictJsonObject(hdrBytes, &hdrObj) || hdrObj.size() != 3)
            return failInvalid(QStringLiteral("invalid_header"));

        QString alg;
        QString typ;
        QString kid;
        if (!extractStrictString(hdrObj, QLatin1String("alg"), &alg) ||
            !extractStrictString(hdrObj, QLatin1String("typ"), &typ) ||
            !extractStrictString(hdrObj, QLatin1String("kid"), &kid))
        {
            return failInvalid(QStringLiteral("invalid_header"));
        }

        if (alg != QStringLiteral("ES256") || typ != QStringLiteral("entitlement+jwt") || !isValidKeyId(kid))
            return failInvalid(QStringLiteral("invalid_header"));

        const auto keyIt = m_keys.constFind(kid);
        if (keyIt == m_keys.constEnd() || !keyIt.value())
            return failInvalid(QStringLiteral("unknown_kid"));

        QByteArray sigBytes;
        if (!decodeStrictBase64Url(sigSeg, &sigBytes) || sigBytes.size() != 64)
            return failInvalid(QStringLiteral("invalid_signature"));

        const QByteArray signingInput = asciiJwt.left(lastDot);
        if (!verifyP256Es256Signature(keyIt.value().get(), signingInput, sigBytes))
            return failInvalid(QStringLiteral("invalid_signature"));

        // Payload claims are parsed ONLY after cryptographic ES256 verification succeeds.
        QByteArray payloadBytes;
        if (!decodeStrictBase64Url(payloadSeg, &payloadBytes))
            return failInvalid(QStringLiteral("invalid_payload_encoding"));

        QJsonObject payloadObj;
        if (!parseStrictJsonObject(payloadBytes, &payloadObj))
            return failInvalid(QStringLiteral("invalid_claims"));

        const bool hasDidField = payloadObj.contains(QLatin1String("did"));
        // "plan" is optional and ignored (no paid plans); when present it must still be a string.
        const bool hasPlanField = payloadObj.contains(QLatin1String("plan"));
        if (hasPlanField && !payloadObj.value(QLatin1String("plan")).isString())
            return failInvalid(QStringLiteral("invalid_claims"));
        const int expectedTopKeys = 11 + (hasDidField ? 1 : 0) + (hasPlanField ? 1 : 0);
        if (payloadObj.size() != expectedTopKeys)
            return failInvalid(QStringLiteral("invalid_claims"));

        EntitlementClaims claims;
        claims.keyId = kid;

        // The "plan" claim is not read: there are no paid plans (the cloud services are free, 2026-11-23),
        // so what the account may use comes from the features and quotas only. A token that still carries
        // "plan" (the API sends "free") or one without it verifies the same way.
        QString statusStr;
        if (!extractStrictString(payloadObj, QLatin1String("iss"), &claims.issuer) ||
            !extractStrictString(payloadObj, QLatin1String("aud"), &claims.audience) ||
            !extractStrictString(payloadObj, QLatin1String("sub"), &claims.subject) ||
            !extractStrictString(payloadObj, QLatin1String("jti"), &claims.jwtId) ||
            !extractStrictInt64Bounded(payloadObj, QLatin1String("iat"), 1, kMaxSafeJsonInt, &claims.issuedAt) ||
            !extractStrictInt64Bounded(payloadObj, QLatin1String("nbf"), 1, kMaxSafeJsonInt, &claims.notBefore) ||
            !extractStrictInt64Bounded(payloadObj, QLatin1String("exp"), 1, kMaxSafeJsonInt, &claims.expiresAt) ||
            !extractStrictInt64Bounded(payloadObj, QLatin1String("offline_grace_until"), 1, kMaxSafeJsonInt, &claims.offlineGraceUntil) ||
            !extractStrictString(payloadObj, QLatin1String("status"), &statusStr))
        {
            return failInvalid(QStringLiteral("invalid_claims"));
        }

        if (hasDidField)
        {
            QString rawDid;
            if (!extractStrictString(payloadObj, QLatin1String("did"), &rawDid))
                return failInvalid(QStringLiteral("invalid_claims"));
            rawDid = rawDid.trimmed().toLower();
            if (!isCanonicalUuid(rawDid))
                return failInvalid(QStringLiteral("invalid_claims"));
            claims.deviceId = rawDid;
        }

        const QString expectedIss = options.expectedIssuer.trimmed().isEmpty()
            ? defaultApiIssuer()
            : options.expectedIssuer.trimmed();
        const QString expectedAud = options.expectedAudience.trimmed().isEmpty()
            ? defaultAppAudience()
            : options.expectedAudience.trimmed().toLower();

        if (claims.issuer != expectedIss)
            return failInvalid(QStringLiteral("issuer_mismatch"));
        if (claims.audience.compare(expectedAud, Qt::CaseInsensitive) != 0)
            return failInvalid(QStringLiteral("audience_mismatch"));

        claims.subject = claims.subject.trimmed().toLower();
        if (!isCanonicalUuid(claims.subject) || !isSafeJwtId(claims.jwtId))
            return failInvalid(QStringLiteral("invalid_claims"));

        const QString wantDeviceId = options.expectedDeviceId.trimmed().toLower();
        if (!wantDeviceId.isEmpty())
        {
            if (!isCanonicalUuid(wantDeviceId) || claims.deviceId.isEmpty() || claims.deviceId != wantDeviceId)
                return failInvalid(QStringLiteral("device_mismatch"));
        }

        if (statusStr == QStringLiteral("none"))
            claims.status = EntitlementStatus::None;
        else if (statusStr == QStringLiteral("trialing"))
            claims.status = EntitlementStatus::Trialing;
        else if (statusStr == QStringLiteral("active"))
            claims.status = EntitlementStatus::Active;
        else if (statusStr == QStringLiteral("past_due"))
            claims.status = EntitlementStatus::PastDue;
        else if (statusStr == QStringLiteral("canceled"))
            claims.status = EntitlementStatus::Canceled;
        else
            return failInvalid(QStringLiteral("invalid_status"));

        const auto featIt = payloadObj.find(QLatin1String("features"));
        const auto quotaIt = payloadObj.find(QLatin1String("quotas"));
        if (featIt == payloadObj.end() || !featIt->isObject() ||
            quotaIt == payloadObj.end() || !quotaIt->isObject())
        {
            return failInvalid(QStringLiteral("invalid_claims"));
        }

        const QJsonObject featObj = featIt->toObject();
        const QJsonObject quotaObj = quotaIt->toObject();
        if (featObj.size() != 4 || quotaObj.size() != 3)
            return failInvalid(QStringLiteral("invalid_claims"));

        if (!extractStrictBool(featObj, QLatin1String("sync"), &claims.features.sync) ||
            !extractStrictBool(featObj, QLatin1String("hosted_ai"), &claims.features.hostedAi) ||
            !extractStrictBool(featObj, QLatin1String("hosted_bridge"), &claims.features.hostedBridge) ||
            !extractStrictBool(featObj, QLatin1String("priority_support"), &claims.features.prioritySupport))
        {
            return failInvalid(QStringLiteral("invalid_claims"));
        }

        qint64 syncCols = 0;
        qint64 syncBytes = 0;
        if (!extractStrictInt64Bounded(quotaObj, QLatin1String("sync_max_collections"), 0, kMaxSyncCollectionsQuota, &syncCols) ||
            !extractStrictInt64Bounded(quotaObj, QLatin1String("sync_max_blob_bytes"), 0, kMaxSyncBlobBytesQuota, &syncBytes) ||
            !extractStrictInt64Bounded(quotaObj, QLatin1String("ai_monthly_token_quota"), 0, kMaxAiMonthlyTokenQuota, &claims.quotas.aiMonthlyTokenQuota))
        {
            return failInvalid(QStringLiteral("invalid_quotas"));
        }
        claims.quotas.syncMaxCollections = static_cast<int>(syncCols);
        claims.quotas.syncMaxBlobBytes = static_cast<int>(syncBytes);

        // Overflow-safe lifetime bounds enforcement.
        if (claims.expiresAt <= claims.issuedAt || (claims.expiresAt - claims.issuedAt) > kOnlineValiditySec)
            return failInvalid(QStringLiteral("invalid_lifetime"));
        if (claims.offlineGraceUntil < claims.expiresAt || (claims.offlineGraceUntil - claims.issuedAt) > kMaxOfflineGraceSec)
            return failInvalid(QStringLiteral("invalid_lifetime"));

        const qint64 skew = std::clamp<qint64>(options.clockSkewSec, 0, 300);
        if (nowUnixSec > std::numeric_limits<qint64>::max() - skew)
            return failInvalid(QStringLiteral("invalid_clock"));
        const qint64 maxAllowedIssued = nowUnixSec + skew;
        if (claims.notBefore > maxAllowedIssued || claims.issuedAt > maxAllowedIssued)
            return failInvalid(QStringLiteral("token_not_yet_valid"));

        EntitlementVerifyResult result;
        result.claims = std::move(claims);
        if (nowUnixSec < result.claims.expiresAt)
        {
            result.state = EntitlementState::ValidOnline;
            return result;
        }
        if (nowUnixSec < result.claims.offlineGraceUntil)
        {
            result.state = EntitlementState::ValidOfflineGrace;
            return result;
        }
        result.state = EntitlementState::Expired;
        result.errorCode = QStringLiteral("expired");
        return result;
    }
}
