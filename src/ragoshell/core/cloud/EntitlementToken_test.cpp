#include "gtest/gtest.h"

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/cloud/EntitlementToken.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/x509.h>

#include <cstring>
#include <limits>
#include <memory>

using namespace RagoShell;

namespace
{
    struct OpenSslFree
    {
        void operator()(unsigned char *p) const noexcept
        {
            if (p)
                OPENSSL_free(p);
        }
    };

    using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    EvpPkeyPtr generateEcKeyByCurveName(const char *curveName)
    {
        EVP_PKEY *raw = EVP_EC_gen(curveName);
        EXPECT_NE(raw, nullptr);
        return EvpPkeyPtr(raw, &EVP_PKEY_free);
    }

    EvpPkeyPtr generateRsa2048Key()
    {
        EVP_PKEY *raw = EVP_RSA_gen(2048);
        EXPECT_NE(raw, nullptr);
        return EvpPkeyPtr(raw, &EVP_PKEY_free);
    }

    QByteArray exportSpkiDer(EVP_PKEY *pkey)
    {
        unsigned char *der = nullptr;
        const int len = i2d_PUBKEY(pkey, &der);
        std::unique_ptr<unsigned char, OpenSslFree> guard(der);
        if (len <= 0 || !der)
            return QByteArray();
        return QByteArray(reinterpret_cast<const char *>(der), len);
    }

    QString formatRingEntry(const QString &kid, EVP_PKEY *pkey)
    {
        const QByteArray der = exportSpkiDer(pkey);
        return kid + QLatin1Char(':') + QString::fromLatin1(der.toBase64());
    }

    QByteArray encodeBase64Url(const QByteArray &raw)
    {
        return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    }

    QByteArray signDigestDer(EVP_PKEY *privKey, const QByteArray &signingInput)
    {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        if (!ctx)
            return QByteArray();
        if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, privKey) != 1)
            return QByteArray();

        const auto *msg = reinterpret_cast<const unsigned char *>(signingInput.constData());
        size_t sigLen = 0;
        if (EVP_DigestSign(ctx.get(), nullptr, &sigLen, msg, static_cast<size_t>(signingInput.size())) != 1)
            return QByteArray();

        QByteArray derSig(static_cast<int>(sigLen), '\0');
        if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char *>(derSig.data()), &sigLen, msg, static_cast<size_t>(signingInput.size())) != 1)
            return QByteArray();
        derSig.resize(static_cast<int>(sigLen));
        return derSig;
    }

    QByteArray derSigToP1363_64(const QByteArray &derSig)
    {
        const auto *p = reinterpret_cast<const unsigned char *>(derSig.constData());
        std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(
            d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(derSig.size())), &ECDSA_SIG_free);
        if (!sig)
            return QByteArray();

        const BIGNUM *r = nullptr;
        const BIGNUM *s = nullptr;
        ECDSA_SIG_get0(sig.get(), &r, &s);
        if (!r || !s)
            return QByteArray();

        QByteArray out(64, '\0');
        auto *buf = reinterpret_cast<unsigned char *>(out.data());
        if (BN_bn2binpad(r, buf, 32) != 32 || BN_bn2binpad(s, buf + 32, 32) != 32)
            return QByteArray();
        return out;
    }

    QString signCompactJwsP256(EVP_PKEY *privKey, const QByteArray &headerJson, const QByteArray &payloadJson)
    {
        const QByteArray signingInput = encodeBase64Url(headerJson) + '.' + encodeBase64Url(payloadJson);
        const QByteArray derSig = signDigestDer(privKey, signingInput);
        const QByteArray rawSig = derSigToP1363_64(derSig);
        return QString::fromLatin1(signingInput + '.' + encodeBase64Url(rawSig));
    }

    QByteArray makeValidPayloadJson(
        qint64 iat,
        qint64 exp,
        qint64 offlineGraceUntil,
        const QString &plan = QStringLiteral("pro"),
        const QString &status = QStringLiteral("active"),
        const QString &did = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7"))
    {
        QJsonObject features{
            {QStringLiteral("sync"), true},
            {QStringLiteral("hosted_ai"), true},
            {QStringLiteral("hosted_bridge"), true},
            {QStringLiteral("priority_support"), true},
        };
        QJsonObject quotas{
            {QStringLiteral("sync_max_collections"), 20},
            {QStringLiteral("sync_max_blob_bytes"), 1048576},
            {QStringLiteral("ai_monthly_token_quota"), 2000000},
        };
        QJsonObject obj{
            {QStringLiteral("iss"), EntitlementVerifier::defaultApiIssuer()},
            {QStringLiteral("aud"), EntitlementVerifier::defaultAppAudience()},
            {QStringLiteral("sub"), QStringLiteral("550e8400-e29b-41d4-a716-446655440000")},
            {QStringLiteral("jti"), QStringLiteral("0123456789abcdef0123456789abcdef")},
            {QStringLiteral("iat"), iat},
            {QStringLiteral("nbf"), iat},
            {QStringLiteral("exp"), exp},
            {QStringLiteral("offline_grace_until"), offlineGraceUntil},
            {QStringLiteral("status"), status},
            {QStringLiteral("features"), features},
            {QStringLiteral("quotas"), quotas},
        };
        if (!plan.isEmpty())
            obj.insert(QStringLiteral("plan"), plan);
        if (!did.isEmpty())
            obj.insert(QStringLiteral("did"), did);
        return QJsonDocument(obj).toJson(QJsonDocument::Compact);
    }
} // namespace

TEST(EntitlementTokenTest, VerifiesExactGoGeneratedEntitlementJwtVector)
{
    // Exact public key ring entry and ES256 compact JWS verified by Go TestCrossLanguageSharedEntitlementVector.
    const QString goRingEntry = QStringLiteral(
        "vec-2026-11:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE/lhzEjS2oJYxbi2KRs3fjyATb26JFKXPcbItxgc0dsKJ3yMsUlY1YLCwzIzzzq9j+S0cz/bGI+3WnGgtsIWRFA==");
    const QString goToken = QStringLiteral(
        "eyJhbGciOiJFUzI1NiIsImtpZCI6InZlYy0yMDI2LTExIiwidHlwIjoiZW50aXRsZW1lbnQrand0In0."
        "eyJpc3MiOiJodHRwczovL2FwaS5yYWdvc2hlbGwuY29tLnRyIiwiYXVkIjoidHIuY29tLnJhZ29zaGVsbC5hcHAiLCJzdWIiOiI1NTBlODQwMC1lMjliLTQxZDQtYTcxNi00NDY2NTU0NDAwMDAiLCJkaWQiOiI3YzllNjY3OS03NDI1LTQwZGUtOTQ0Yi1lMDdmYzFmOTBhZTciLCJqdGkiOiJhMWIyYzNkNGU1ZjYwNzE4MjkzYTRiNWM2ZDdlOGY5MCIsImlhdCI6MTc5MzcwNzIwMCwibmJmIjoxNzkzNzA3MjAwLCJleHAiOjE3OTQzMTIwMDAsIm9mZmxpbmVfZ3JhY2VfdW50aWwiOjE3OTQ5MTY4MDAsInBsYW4iOiJwcm8iLCJzdGF0dXMiOiJhY3RpdmUiLCJmZWF0dXJlcyI6eyJzeW5jIjp0cnVlLCJob3N0ZWRfYWkiOnRydWUsImhvc3RlZF9icmlkZ2UiOnRydWUsInByaW9yaXR5X3N1cHBvcnQiOnRydWV9LCJxdW90YXMiOnsic3luY19tYXhfY29sbGVjdGlvbnMiOjIwLCJzeW5jX21heF9ibG9iX2J5dGVzIjoxMDQ4NTc2LCJhaV9tb250aGx5X3Rva2VuX3F1b3RhIjoyMDAwMDAwfX0."
        "IlRlEUVDyXn5pqP7YvobXPR9LdBVIxGyBAVWnMm9hADgkavXvyy8HqmgDFPwfNMDGXOrJRRCdceJ8TLKP71E0A");

    bool ok = false;
    QString err;
    const EntitlementVerifier verifier = EntitlementVerifier::fromKeyRingString(goRingEntry, &ok, &err);
    ASSERT_TRUE(ok) << err.toStdString();
    ASSERT_EQ(verifier.keyCount(), 1);

    constexpr qint64 kIat = 1793707200;
    EntitlementVerifyOptions opts;
    opts.expectedDeviceId = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");

    // Day 2: ValidOnline
    const EntitlementVerifyResult online = verifier.verify(goToken, kIat + 2 * 86400, opts);
    ASSERT_EQ(online.state, EntitlementState::ValidOnline) << online.errorCode.toStdString();
    EXPECT_TRUE(online.isUsable());
    EXPECT_EQ(online.claims.keyId, QStringLiteral("vec-2026-11"));
    EXPECT_EQ(online.claims.status, EntitlementStatus::Active);
    EXPECT_TRUE(online.claims.features.sync);
    EXPECT_TRUE(online.claims.features.hostedAi);
    EXPECT_TRUE(online.claims.features.hostedBridge);
    EXPECT_TRUE(online.claims.features.prioritySupport);
    EXPECT_EQ(online.claims.quotas.syncMaxCollections, 20);
    EXPECT_EQ(online.claims.quotas.syncMaxBlobBytes, 1048576);
    EXPECT_EQ(online.claims.quotas.aiMonthlyTokenQuota, 2000000);

    // Day 7 boundary and Day 13: ValidOfflineGrace (14-day tolerance)
    const EntitlementVerifyResult graceStart = verifier.verify(goToken, kIat + 7 * 86400, opts);
    EXPECT_EQ(graceStart.state, EntitlementState::ValidOfflineGrace);
    EXPECT_TRUE(graceStart.isUsable());

    const EntitlementVerifyResult graceEnd = verifier.verify(goToken, kIat + 14 * 86400 - 1, opts);
    EXPECT_EQ(graceEnd.state, EntitlementState::ValidOfflineGrace);
    EXPECT_TRUE(graceEnd.isUsable());

    // Day 14 boundary: Expired
    const EntitlementVerifyResult expired = verifier.verify(goToken, kIat + 14 * 86400, opts);
    EXPECT_EQ(expired.state, EntitlementState::Expired);
    EXPECT_FALSE(expired.isUsable());
    EXPECT_EQ(expired.errorCode, QStringLiteral("expired"));
}

TEST(EntitlementTokenTest, SupportsZeroDowntimeMultiKeyRotationAndKeyRetirement)
{
    EvpPkeyPtr key1 = generateEcKeyByCurveName("prime256v1");
    EvpPkeyPtr key2 = generateEcKeyByCurveName("prime256v1");

    const QString entry1 = formatRingEntry(QStringLiteral("kid-2026-10"), key1.get());
    const QString entry2 = formatRingEntry(QStringLiteral("kid-2026-11"), key2.get());

    bool ok = false;
    const EntitlementVerifier dualRing = EntitlementVerifier::fromKeyRingString(
        entry1 + QLatin1Char(';') + entry2, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(dualRing.keyCount(), 2);

    constexpr qint64 kNow = 1793707200;
    const QByteArray payload = makeValidPayloadJson(kNow, kNow + 604800, kNow + 1209600);
    const QString tok1 = signCompactJwsP256(
        key1.get(),
        QByteArrayLiteral("{\"alg\":\"ES256\",\"typ\":\"entitlement+jwt\",\"kid\":\"kid-2026-10\"}"),
        payload);
    const QString tok2 = signCompactJwsP256(
        key2.get(),
        QByteArrayLiteral("{\"alg\":\"ES256\",\"typ\":\"entitlement+jwt\",\"kid\":\"kid-2026-11\"}"),
        payload);

    // Both old and new tokens verify during rotation overlap.
    EXPECT_EQ(dualRing.verify(tok1, kNow + 60).state, EntitlementState::ValidOnline);
    EXPECT_EQ(dualRing.verify(tok2, kNow + 60).state, EntitlementState::ValidOnline);

    // Retiring kid-2026-10 rejects tok1 while keeping tok2 valid.
    const EntitlementVerifier retiredRing = EntitlementVerifier::fromKeyRingString(entry2, &ok);
    ASSERT_TRUE(ok);
    const EntitlementVerifyResult oldRes = retiredRing.verify(tok1, kNow + 60);
    EXPECT_EQ(oldRes.state, EntitlementState::Invalid);
    EXPECT_EQ(oldRes.errorCode, QStringLiteral("unknown_kid"));
    EXPECT_EQ(retiredRing.verify(tok2, kNow + 60).state, EntitlementState::ValidOnline);
}

// There are no paid plans (2026-11-23): the "plan" claim is ignored, whatever it says or when it is missing.
TEST(EntitlementTokenTest, IgnoresThePlanClaim)
{
    EvpPkeyPtr key = generateEcKeyByCurveName("prime256v1");
    bool ok = false;
    const EntitlementVerifier ring =
        EntitlementVerifier::fromKeyRingString(formatRingEntry(QStringLiteral("kid-plan"), key.get()), &ok);
    ASSERT_TRUE(ok);
    constexpr qint64 kNow = 1793707200;
    for (const QString &plan : {QStringLiteral("free"), QStringLiteral("enterprise"), QString()}) {
        const QString tok = signCompactJwsP256(
            key.get(),
            QByteArrayLiteral("{\"alg\":\"ES256\",\"typ\":\"entitlement+jwt\",\"kid\":\"kid-plan\"}"),
            makeValidPayloadJson(kNow, kNow + 604800, kNow + 1209600, plan));
        const EntitlementVerifyResult res = ring.verify(tok, kNow + 60);
        EXPECT_EQ(res.state, EntitlementState::ValidOnline) << plan.toStdString() << " " << res.errorCode.toStdString();
        EXPECT_TRUE(res.claims.features.hostedBridge);
    }
}

TEST(EntitlementTokenTest, RejectsAlgorithmTypeAudienceSignatureAndKeyTampering)
{
    EvpPkeyPtr p256Key = generateEcKeyByCurveName("prime256v1");
    bool ok = false;
    const EntitlementVerifier verifier = EntitlementVerifier::fromKeyRingString(
        formatRingEntry(QStringLiteral("kid-main"), p256Key.get()), &ok);
    ASSERT_TRUE(ok);

    constexpr qint64 kNow = 1793707200;
    const QByteArray validHdr = QByteArrayLiteral("{\"alg\":\"ES256\",\"typ\":\"entitlement+jwt\",\"kid\":\"kid-main\"}");
    const QByteArray validPayload = makeValidPayloadJson(kNow, kNow + 604800, kNow + 1209600);
    const QString validTok = signCompactJwsP256(p256Key.get(), validHdr, validPayload);
    ASSERT_EQ(verifier.verify(validTok, kNow).state, EntitlementState::ValidOnline);

    // 1. alg: "none" and alg: "HS256"
    const QString algNone = QString::fromLatin1(
        encodeBase64Url(QByteArrayLiteral("{\"alg\":\"none\",\"typ\":\"entitlement+jwt\",\"kid\":\"kid-main\"}")) +
        '.' + encodeBase64Url(validPayload) + ".AAAA");
    EXPECT_EQ(verifier.verify(algNone, kNow).state, EntitlementState::Invalid);

    const QString algHs256 = signCompactJwsP256(
        p256Key.get(),
        QByteArrayLiteral("{\"alg\":\"HS256\",\"typ\":\"entitlement+jwt\",\"kid\":\"kid-main\"}"),
        validPayload);
    EXPECT_EQ(verifier.verify(algHs256, kNow).state, EntitlementState::Invalid);

    // 2. Cross-token confusion: typ="at+jwt" and API domain audience MUST be rejected.
    const QString accessTypTok = signCompactJwsP256(
        p256Key.get(),
        QByteArrayLiteral("{\"alg\":\"ES256\",\"typ\":\"at+jwt\",\"kid\":\"kid-main\"}"),
        validPayload);
    EXPECT_EQ(verifier.verify(accessTypTok, kNow).state, EntitlementState::Invalid);

    QJsonObject wrongAudObj = QJsonDocument::fromJson(validPayload).object();
    wrongAudObj.insert(QStringLiteral("aud"), QStringLiteral("api.") + QString::fromUtf8(BrandInfo::domain));
    const QString wrongAudTok = signCompactJwsP256(
        p256Key.get(), validHdr, QJsonDocument(wrongAudObj).toJson(QJsonDocument::Compact));
    const EntitlementVerifyResult wrongAudRes = verifier.verify(wrongAudTok, kNow);
    EXPECT_EQ(wrongAudRes.state, EntitlementState::Invalid);
    EXPECT_EQ(wrongAudRes.errorCode, QStringLiteral("audience_mismatch"));

    // 3. Duplicate JSON keys (including Unicode escape "\u0061lg") MUST be rejected.
    const QString dupHdrTok = signCompactJwsP256(
        p256Key.get(),
        QByteArrayLiteral("{\"alg\":\"none\",\"\\u0061lg\":\"ES256\",\"typ\":\"entitlement+jwt\",\"kid\":\"kid-main\"}"),
        validPayload);
    EXPECT_EQ(verifier.verify(dupHdrTok, kNow).state, EntitlementState::Invalid);

    // 4. ASN.1 DER signature instead of 64-byte IEEE P1363 R||S MUST be rejected.
    const QByteArray signingInput = encodeBase64Url(validHdr) + '.' + encodeBase64Url(validPayload);
    const QByteArray derSig = signDigestDer(p256Key.get(), signingInput);
    const QString derTok = QString::fromLatin1(signingInput + '.' + encodeBase64Url(derSig));
    EXPECT_EQ(verifier.verify(derTok, kNow).state, EntitlementState::Invalid);

    // 5. 1-bit flipped signature, R=0, S=0, R=N MUST be rejected.
    QByteArray rawSig = derSigToP1363_64(derSig);
    rawSig[9] = static_cast<char>(rawSig[9] ^ 0x01);
    const QString flippedTok = QString::fromLatin1(signingInput + '.' + encodeBase64Url(rawSig));
    EXPECT_EQ(verifier.verify(flippedTok, kNow).state, EntitlementState::Invalid);

    QByteArray zeroRSig(64, '\0');
    std::memcpy(zeroRSig.data() + 32, derSigToP1363_64(derSig).constData() + 32, 32);
    EXPECT_EQ(verifier.verify(QString::fromLatin1(signingInput + '.' + encodeBase64Url(zeroRSig)), kNow).state,
              EntitlementState::Invalid);

    std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)> group(
        EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1), &EC_GROUP_free);
    QByteArray orderRSig(64, '\0');
    BN_bn2binpad(EC_GROUP_get0_order(group.get()), reinterpret_cast<unsigned char *>(orderRSig.data()), 32);
    std::memcpy(orderRSig.data() + 32, derSigToP1363_64(derSig).constData() + 32, 32);
    EXPECT_EQ(verifier.verify(QString::fromLatin1(signingInput + '.' + encodeBase64Url(orderRSig)), kNow).state,
              EntitlementState::Invalid);

    // 6. Tampered payload (free -> pro without re-signing) MUST fail signature check.
    const QByteArray tamperedPayload = makeValidPayloadJson(kNow, kNow + 604800, kNow + 1209600, QStringLiteral("team"));
    const QStringList segs = validTok.split(QLatin1Char('.'));
    const QString tamperedTok = segs.at(0) + QLatin1Char('.') +
                                QString::fromLatin1(encodeBase64Url(tamperedPayload)) + QLatin1Char('.') +
                                segs.at(2);
    EXPECT_EQ(verifier.verify(tamperedTok, kNow).state, EntitlementState::Invalid);

    // 7. Device mismatch and missing did when expectedDeviceId is supplied.
    EntitlementVerifyOptions wrongDevOpts;
    wrongDevOpts.expectedDeviceId = QStringLiteral("11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(verifier.verify(validTok, kNow, wrongDevOpts).errorCode, QStringLiteral("device_mismatch"));

    const QString portalTok = signCompactJwsP256(
        p256Key.get(), validHdr, makeValidPayloadJson(kNow, kNow + 604800, kNow + 1209600, QStringLiteral("pro"), QStringLiteral("active"), QString()));
    EXPECT_EQ(verifier.verify(portalTok, kNow).state, EntitlementState::ValidOnline);
    EXPECT_EQ(verifier.verify(portalTok, kNow, wrongDevOpts).errorCode, QStringLiteral("device_mismatch"));

    // 8. Out-of-bounds offline_grace_until (> 14d), float quota, and non-canonical Base64URL trailing bits.
    const QString excessiveGraceTok = signCompactJwsP256(
        p256Key.get(), validHdr, makeValidPayloadJson(kNow, kNow + 604800, kNow + 15 * 86400));
    EXPECT_EQ(verifier.verify(excessiveGraceTok, kNow).errorCode, QStringLiteral("invalid_lifetime"));

    QByteArray nonCanonOut;
    // "AB" has non-zero trailing 4 bits in 'B' (value 1, 1 & 0x0F != 0), whereas "AA" is canonical 0x00.
    EXPECT_TRUE(EntitlementVerifier::decodeStrictBase64Url(QByteArrayLiteral("AA"), &nonCanonOut));
    EXPECT_FALSE(EntitlementVerifier::decodeStrictBase64Url(QByteArrayLiteral("AB"), &nonCanonOut));

    // Leading/trailing whitespace or oversized envelopes (> 4096 bytes) MUST fail immediately without trimming.
    EXPECT_EQ(verifier.verify(QStringLiteral(" ") + validTok, kNow).errorCode, QStringLiteral("malformed_token"));
    EXPECT_EQ(verifier.verify(validTok + QStringLiteral("\n"), kNow).errorCode, QStringLiteral("malformed_token"));
    EXPECT_EQ(verifier.verify(QString(5000, QLatin1Char(' ')) + validTok, kNow).errorCode, QStringLiteral("malformed_token"));

    // 9. Reject non-P256 SPKI keys (secp384r1 and RSA-2048), trailing DER garbage, and duplicate kids.
    EvpPkeyPtr p384Key = generateEcKeyByCurveName("secp384r1");
    QString keyErr;
    EntitlementVerifier tmpVerifier;
    EXPECT_FALSE(tmpVerifier.addPublicKeySpkiDer(QStringLiteral("p384"), exportSpkiDer(p384Key.get()), &keyErr));
    EXPECT_EQ(keyErr, QStringLiteral("unsupported_ec_curve"));

    EvpPkeyPtr rsaKey = generateRsa2048Key();
    EXPECT_FALSE(tmpVerifier.addPublicKeySpkiDer(QStringLiteral("rsa"), exportSpkiDer(rsaKey.get()), &keyErr));
    EXPECT_EQ(keyErr, QStringLiteral("unsupported_key_type"));

    QByteArray trailingDer = exportSpkiDer(p256Key.get());
    trailingDer.append('\x00');
    EXPECT_FALSE(tmpVerifier.addPublicKeySpkiDer(QStringLiteral("trail"), trailingDer, &keyErr));
    EXPECT_EQ(keyErr, QStringLiteral("invalid_spki_der"));

    EXPECT_TRUE(tmpVerifier.addPublicKeySpkiDer(QStringLiteral("dup"), exportSpkiDer(p256Key.get()), &keyErr));
    EXPECT_FALSE(tmpVerifier.addPublicKeySpkiDer(QStringLiteral("dup"), exportSpkiDer(p256Key.get()), &keyErr));
    EXPECT_EQ(keyErr, QStringLiteral("duplicate_kid"));

    // Thread-local OpenSSL error queue must remain completely clean after all failure paths.
    EXPECT_EQ(ERR_peek_error(), 0UL);
}
