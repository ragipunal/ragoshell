#include "gtest/gtest.h"

#include "ragoshell/core/utils/Minisign.h"

#include <openssl/evp.h>

#include <array>
#include <cstring>
#include <memory>

using namespace RagoShell;

namespace
{
struct TestKeyPair
{
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey{nullptr, &EVP_PKEY_free};
    std::array<std::uint8_t, 8> keyId{};
    std::array<std::uint8_t, 32> rawPub{};
    QString pubKeyBase64;
    QString pubFileText;
};

TestKeyPair makeEd25519KeyPair(std::uint64_t idSeed = 0x0102030405060708ULL)
{
    TestKeyPair out;
    EVP_PKEY *generated = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    out.pkey.reset(generated);
    for (int i = 0; i < 8; ++i)
        out.keyId[i] = static_cast<std::uint8_t>((idSeed >> (i * 8)) & 0xffU);

    std::size_t pubLen = out.rawPub.size();
    EVP_PKEY_get_raw_public_key(out.pkey.get(), out.rawPub.data(), &pubLen);

    QByteArray pubStruct;
    pubStruct.append("Ed", 2);
    pubStruct.append(reinterpret_cast<const char *>(out.keyId.data()), 8);
    pubStruct.append(reinterpret_cast<const char *>(out.rawPub.data()), 32);

    out.pubKeyBase64 = QString::fromLatin1(pubStruct.toBase64());
    out.pubFileText = QStringLiteral("untrusted comment: minisign public key TEST\n%1\n").arg(out.pubKeyBase64);
    return out;
}

QByteArray signRawEd25519(EVP_PKEY *pkey, const QByteArray &payload)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, pkey);
    std::size_t sigLen = 64;
    QByteArray sig(64, Qt::Uninitialized);
    const unsigned char empty = 0;
    const unsigned char *ptr = payload.isEmpty()
        ? &empty
        : reinterpret_cast<const unsigned char *>(payload.constData());
    EVP_DigestSign(ctx.get(),
                   reinterpret_cast<unsigned char *>(sig.data()),
                   &sigLen,
                   ptr,
                   static_cast<std::size_t>(payload.size()));
    return sig;
}

QString signMinisig(const TestKeyPair &kp,
                    const QByteArray &message,
                    const QString &trustedComment,
                    bool prehashed = true)
{
    QByteArray toSign;
    if (prehashed)
    {
        unsigned char digest[64]{};
        unsigned int digestLen = 0;
        const unsigned char empty = 0;
        const unsigned char *ptr = message.isEmpty()
            ? &empty
            : reinterpret_cast<const unsigned char *>(message.constData());
        EVP_Digest(ptr, static_cast<std::size_t>(message.size()), digest, &digestLen, EVP_blake2b512(), nullptr);
        toSign = QByteArray(reinterpret_cast<const char *>(digest), 64);
    }
    else
    {
        toSign = message;
    }

    const QByteArray rawSig = signRawEd25519(kp.pkey.get(), toSign);

    QByteArray sigStruct;
    sigStruct.append(prehashed ? "ED" : "Ed", 2);
    sigStruct.append(reinterpret_cast<const char *>(kp.keyId.data()), 8);
    sigStruct.append(rawSig);

    QByteArray globalPayload = rawSig;
    globalPayload.append(trustedComment.toUtf8());
    const QByteArray globalSig = signRawEd25519(kp.pkey.get(), globalPayload);

    return QStringLiteral("untrusted comment: signature from test signer\n%1\ntrusted comment: %2\n%3\n")
        .arg(QString::fromLatin1(sigStruct.toBase64()),
             trustedComment,
             QString::fromLatin1(globalSig.toBase64()));
}
} // namespace

TEST(MinisignTest, ParsesSingleLineAndTwoLinePublicKeys)
{
    const TestKeyPair kp = makeEd25519KeyPair(0xAABBCCDDEEFF0011ULL);
    ASSERT_TRUE(kp.pubKeyBase64.startsWith(QStringLiteral("RW")));

    Minisign::PublicKey parsed1;
    QString err1;
    ASSERT_TRUE(Minisign::parsePublicKey(kp.pubKeyBase64, &parsed1, &err1)) << err1.toStdString();
    EXPECT_EQ(parsed1.keyId, kp.keyId);
    EXPECT_EQ(parsed1.publicKey, kp.rawPub);

    Minisign::PublicKey parsed2;
    QString err2;
    ASSERT_TRUE(Minisign::parsePublicKey(kp.pubFileText, &parsed2, &err2)) << err2.toStdString();
    EXPECT_EQ(parsed2.keyId, kp.keyId);
    EXPECT_EQ(parsed2.publicKey, kp.rawPub);

    EXPECT_FALSE(Minisign::parsePublicKey(QString(), &parsed1));
    EXPECT_FALSE(Minisign::parsePublicKey(QStringLiteral("RWnotvalidbase64!!!"), &parsed1));
}

TEST(MinisignTest, VerifiesPrehashedBlake2bAndLegacyPureEd25519Signatures)
{
    const TestKeyPair kp = makeEd25519KeyPair();
    const QByteArray message("{\"schemaVersion\":1,\"latestVersion\":\"2.1.0\"}\n");
    const QString comment = QStringLiteral("release:2.1.0\ttimestamp:2026-11-03T10:00:00Z\tfile:releases.json");

    const QString sigED = signMinisig(kp, message, comment, /*prehashed=*/true);
    const Minisign::VerifyResult resED = Minisign::verify(message, sigED, kp.pubKeyBase64);
    ASSERT_TRUE(resED.ok) << resED.error.toStdString();
    EXPECT_TRUE(resED.prehashed);
    EXPECT_EQ(resED.tokens.release, QStringLiteral("2.1.0"));
    EXPECT_EQ(resED.tokens.file, QStringLiteral("releases.json"));
    EXPECT_TRUE(resED.tokens.hasTimestamp);

    const QString sigEd = signMinisig(kp, message, comment, /*prehashed=*/false);
    const Minisign::VerifyResult resEd = Minisign::verify(message, sigEd, kp.pubFileText);
    ASSERT_TRUE(resEd.ok) << resEd.error.toStdString();
    EXPECT_FALSE(resEd.prehashed);

    Minisign::VerifyOptions strictPrehashed;
    strictPrehashed.allowLegacyUnprehashed = false;
    const Minisign::VerifyResult rejectedLegacy = Minisign::verify(message, sigEd, kp.pubKeyBase64, strictPrehashed);
    EXPECT_FALSE(rejectedLegacy.ok);
}

TEST(MinisignTest, RejectsTamperedMessageSignatureGlobalSigTrustedCommentAndKeyMismatch)
{
    const TestKeyPair kp = makeEd25519KeyPair(0x1122334455667788ULL);
    const TestKeyPair wrongKeySameId = makeEd25519KeyPair(0x1122334455667788ULL);
    const TestKeyPair wrongKeyDiffId = makeEd25519KeyPair(0x99AABBCCDDEEFF00ULL);

    const QByteArray message("{\"latestVersion\":\"2.2.0\",\"mandatory\":false}");
    const QString comment = QStringLiteral("release:2.2.0\ttimestamp:2026-11-03T10:00:00Z\tfile:releases.json");
    const QString validSig = signMinisig(kp, message, comment, true);

    // 1. Tampered message payload
    QByteArray tamperedMsg = message;
    tamperedMsg.replace("false", "true");
    EXPECT_FALSE(Minisign::verify(tamperedMsg, validSig, kp.pubKeyBase64).ok);

    // 2. Tampered trusted comment (global_sig binds trusted comment!)
    QString tamperedCommentSig = validSig;
    tamperedCommentSig.replace(QStringLiteral("release:2.2.0"), QStringLiteral("release:2.1.0"));
    EXPECT_FALSE(Minisign::verify(message, tamperedCommentSig, kp.pubKeyBase64).ok);

    // 3. Tampered global_sig line
    QStringList lines = validSig.trimmed().split(QLatin1Char('\n'));
    ASSERT_EQ(lines.size(), 4);
    QByteArray globalDecoded = QByteArray::fromBase64(lines[3].toLatin1());
    globalDecoded[0] = static_cast<char>(globalDecoded[0] ^ 0x01);
    lines[3] = QString::fromLatin1(globalDecoded.toBase64());
    EXPECT_FALSE(Minisign::verify(message, lines.join(QLatin1Char('\n')) + QLatin1Char('\n'), kp.pubKeyBase64).ok);

    // 4. Wrong key with different key ID
    EXPECT_FALSE(Minisign::verify(message, validSig, wrongKeyDiffId.pubKeyBase64).ok);

    // 5. Wrong key with identical key ID
    EXPECT_FALSE(Minisign::verify(message, validSig, wrongKeySameId.pubKeyBase64).ok);

    // 6. Claim expectations mismatch (file / release / clock skew / max age)
    Minisign::VerifyOptions wrongFileOpts;
    wrongFileOpts.expectedFile = QStringLiteral("SHA256SUMS.txt");
    EXPECT_FALSE(Minisign::verify(message, validSig, kp.pubKeyBase64, wrongFileOpts).ok);

    Minisign::VerifyOptions wrongRelOpts;
    wrongRelOpts.expectedRelease = QStringLiteral("9.9.9");
    EXPECT_FALSE(Minisign::verify(message, validSig, kp.pubKeyBase64, wrongRelOpts).ok);

    Minisign::VerifyOptions skewOpts;
    skewOpts.checkClockSkew = true;
    skewOpts.nowEpochSec = 1700000000LL; // 2023 -> 2026 signature appears far in the future
    EXPECT_FALSE(Minisign::verify(message, validSig, kp.pubKeyBase64, skewOpts).ok);
}
