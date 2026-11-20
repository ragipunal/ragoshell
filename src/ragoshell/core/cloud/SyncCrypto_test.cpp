#include "gtest/gtest.h"

#include "ragoshell/core/cloud/SyncCrypto.h"

#include <QJsonObject>

using namespace RagoShell;

TEST(SyncCryptoTest, Argon2idDerivesDeterministicMasterKey)
{
    SyncCrypto::KdfParams p;
    p.salt = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const QByteArray k1 = SyncCrypto::deriveMasterKey(QStringLiteral("correct horse battery staple"), p);
    const QByteArray k2 = SyncCrypto::deriveMasterKey(QStringLiteral("correct horse battery staple"), p);
    ASSERT_EQ(k1.size(), SyncCrypto::kMasterKeyBytes);
    EXPECT_EQ(k1, k2);
    // Different passphrase or salt must yield a different key.
    EXPECT_NE(k1, SyncCrypto::deriveMasterKey(QStringLiteral("other passphrase"), p));
    SyncCrypto::KdfParams p2 = p;
    p2.salt = QByteArray::fromHex("ffffffffffffffffffffffffffffffff");
    EXPECT_NE(k1, SyncCrypto::deriveMasterKey(QStringLiteral("correct horse battery staple"), p2));
}

TEST(SyncCryptoTest, KdfParamsRoundTripAndStrictValidation)
{
    SyncCrypto::KdfParams p;
    p.salt = SyncCrypto::generateSalt();
    ASSERT_EQ(p.salt.size(), SyncCrypto::kSaltBytes);
    const QJsonObject obj = SyncCrypto::buildKdfParams(p);
    EXPECT_EQ(obj.value(QStringLiteral("alg")).toString(), QLatin1String("argon2id"));
    EXPECT_EQ(obj.value(QStringLiteral("cipher")).toString(), QLatin1String("aes-256-gcm"));

    SyncCrypto::KdfParams parsed;
    QString err;
    ASSERT_TRUE(SyncCrypto::parseKdfParams(obj, &parsed, &err));
    EXPECT_EQ(parsed.memoryKiB, p.memoryKiB);
    EXPECT_EQ(parsed.iterations, p.iterations);
    EXPECT_EQ(parsed.parallelism, p.parallelism);
    EXPECT_EQ(parsed.salt, p.salt);

    // Strict rejections
    QJsonObject bad;
    EXPECT_FALSE(SyncCrypto::parseKdfParams(bad, &parsed, &err));
    bad = obj; bad.insert(QStringLiteral("alg"), QStringLiteral("pbkdf2"));
    EXPECT_FALSE(SyncCrypto::parseKdfParams(bad, &parsed, &err));
    bad = obj; bad.insert(QStringLiteral("m_cost_kib"), static_cast<qint64>(1u << 22)); // memory bomb
    EXPECT_FALSE(SyncCrypto::parseKdfParams(bad, &parsed, &err));
    bad = obj; bad.insert(QStringLiteral("salt"), QStringLiteral("!!!notbase64!!!"));
    EXPECT_FALSE(SyncCrypto::parseKdfParams(bad, &parsed, &err));
    bad = obj; bad.insert(QStringLiteral("cipher"), QStringLiteral("chacha20"));
    EXPECT_FALSE(SyncCrypto::parseKdfParams(bad, &parsed, &err));
    bad = obj; bad.insert(QStringLiteral("schema_version"), 99);
    EXPECT_FALSE(SyncCrypto::parseKdfParams(bad, &parsed, &err));
}

TEST(SyncCryptoTest, HkdfDerivesDistinctKeysPerCollection)
{
    const QByteArray master = SyncCrypto::generateDeviceKey();
    const QByteArray a = SyncCrypto::deriveCollectionKey(master, QStringLiteral("connections"));
    const QByteArray b = SyncCrypto::deriveCollectionKey(master, QStringLiteral("snippets"));
    ASSERT_EQ(a.size(), SyncCrypto::kMasterKeyBytes);
    EXPECT_NE(a, b);
    EXPECT_EQ(a, SyncCrypto::deriveCollectionKey(master, QStringLiteral("connections")));
    EXPECT_TRUE(SyncCrypto::deriveCollectionKey(QByteArray(8, 'x'), QStringLiteral("connections")).isEmpty());
}

TEST(SyncCryptoTest, Aes256GcmRoundTripAndAadBinding)
{
    const QByteArray key = SyncCrypto::generateDeviceKey();
    const QByteArray collKey = SyncCrypto::deriveCollectionKey(key, QStringLiteral("connections"));
    const QByteArray plaintext = QByteArrayLiteral("{\"hello\":\"sync\"}");

    QByteArray ct, nonce;
    ASSERT_TRUE(SyncCrypto::encryptEnvelope(collKey, QStringLiteral("connections"), plaintext, &ct, &nonce));
    EXPECT_EQ(nonce.size(), SyncCrypto::kNonceBytes);
    EXPECT_EQ(ct.size(), plaintext.size() + SyncCrypto::kGcmTagBytes);

    QByteArray plain;
    ASSERT_TRUE(SyncCrypto::decryptEnvelope(collKey, QStringLiteral("connections"), ct, nonce, &plain));
    EXPECT_EQ(plain, plaintext);

    // Wrong collection (AAD mismatch) must fail the tag check.
    EXPECT_FALSE(SyncCrypto::decryptEnvelope(collKey, QStringLiteral("snippets"), ct, nonce, &plain));
    // Wrong key fails.
    const QByteArray other = SyncCrypto::generateDeviceKey();
    EXPECT_FALSE(SyncCrypto::decryptEnvelope(
        SyncCrypto::deriveCollectionKey(other, QStringLiteral("connections")), QStringLiteral("connections"),
        ct, nonce, &plain));
    // Tampered ciphertext fails.
    QByteArray tampered = ct;
    tampered[0] = tampered[0] ^ '\x01';
    EXPECT_FALSE(SyncCrypto::decryptEnvelope(collKey, QStringLiteral("connections"), tampered, nonce, &plain));
    // Truncated (tag only or less) fails.
    EXPECT_FALSE(SyncCrypto::decryptEnvelope(collKey, QStringLiteral("connections"),
                                             ct.left(SyncCrypto::kGcmTagBytes - 1), nonce, &plain));
}

TEST(SyncCryptoTest, EmptyPayloadEncryptsAndDecrypts)
{
    const QByteArray key = SyncCrypto::generateDeviceKey();
    const QByteArray collKey = SyncCrypto::deriveCollectionKey(key, QStringLiteral("preferences"));
    QByteArray ct, nonce, plain;
    ASSERT_TRUE(SyncCrypto::encryptEnvelope(collKey, QStringLiteral("preferences"), QByteArray(), &ct, &nonce));
    ASSERT_TRUE(SyncCrypto::decryptEnvelope(collKey, QStringLiteral("preferences"), ct, nonce, &plain));
    EXPECT_TRUE(plain.isEmpty());
}
