#include "gtest/gtest.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "ragoshell/utils/KeyStore.h"

using namespace RagoShell;

// Passphrase-encrypted file keystore used by the WASM build when
// RAGOSHELL_KEYSTORE_PASS is set (docs/WebStorage.md §2). Exercised natively —
// the store itself is platform-neutral.

class KeyStorePassfileTest : public testing::Test
{
protected:
    QTemporaryDir dir;
    QString path() const { return dir.filePath("keystore.json"); }
};

TEST_F(KeyStorePassfileTest, write_read_roundtrip)
{
    auto store = createPassphraseFileKeyStore(path(), QStringLiteral("hunter2"));
    ASSERT_TRUE(store);
    ASSERT_TRUE(store->isAvailable());
    EXPECT_TRUE(store->write(QStringLiteral("svc-acct"), QStringLiteral("s3cret")));
    QString value;
    EXPECT_EQ(store->read(QStringLiteral("svc-acct"), &value), KeyStore::Result::Ok);
    EXPECT_EQ(value, QStringLiteral("s3cret"));
}

TEST_F(KeyStorePassfileTest, reopen_with_same_passphrase)
{
    {
        auto store = createPassphraseFileKeyStore(path(), QStringLiteral("hunter2"));
        ASSERT_TRUE(store);
        ASSERT_TRUE(store->write(QStringLiteral("svc"), QStringLiteral("v1")));
    }
    // A new store instance must derive the key from the persisted salt.
    auto store = createPassphraseFileKeyStore(path(), QStringLiteral("hunter2"));
    ASSERT_TRUE(store);
    QString value;
    EXPECT_EQ(store->read(QStringLiteral("svc"), &value), KeyStore::Result::Ok);
    EXPECT_EQ(value, QStringLiteral("v1"));
}

TEST_F(KeyStorePassfileTest, corrupt_file_fails_closed)
{
    QFile f(path());
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("not json at all");
    f.close();
    // Unparseable blob: no store is handed out, the file is left in place.
    EXPECT_EQ(createPassphraseFileKeyStore(path(), QStringLiteral("pw")), nullptr);
    EXPECT_TRUE(QFile::exists(path()));
}

TEST_F(KeyStorePassfileTest, missing_account_returns_notfound)
{
    auto store = createPassphraseFileKeyStore(path(), QStringLiteral("pw"));
    ASSERT_TRUE(store);
    QString value;
    EXPECT_EQ(store->read(QStringLiteral("nobody"), &value), KeyStore::Result::NotFound);
}

TEST_F(KeyStorePassfileTest, remove_entry)
{
    auto store = createPassphraseFileKeyStore(path(), QStringLiteral("pw"));
    ASSERT_TRUE(store->write(QStringLiteral("svc"), QStringLiteral("v1")));
    EXPECT_TRUE(store->remove(QStringLiteral("svc")));
    QString value;
    EXPECT_EQ(store->read(QStringLiteral("svc"), &value), KeyStore::Result::NotFound);
}

TEST_F(KeyStorePassfileTest, aad_binds_account_names)
{
    // Two accounts sealed under the same key must not be interchangeable:
    // swapping ciphertext between accounts has to fail the GCM tag.
    {
        auto store = createPassphraseFileKeyStore(path(), QStringLiteral("pw"));
        ASSERT_TRUE(store->write(QStringLiteral("a"), QStringLiteral("secret-a")));
        ASSERT_TRUE(store->write(QStringLiteral("b"), QStringLiteral("secret-b")));
    }

    QFile f(path());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    QJsonObject entries = root.value("entries").toObject();
    const QString keyA = QString::fromUtf8(QByteArray("a").toBase64());
    const QString keyB = QString::fromUtf8(QByteArray("b").toBase64());
    const QJsonObject swap = entries.value(keyA).toObject();
    entries.insert(keyA, entries.value(keyB).toObject());
    entries.insert(keyB, swap);
    root.insert("entries", entries);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.close();

    auto store = createPassphraseFileKeyStore(path(), QStringLiteral("pw"));
    ASSERT_TRUE(store);
    QString value, error;
    // "a" now holds b's ciphertext: the per-account AAD must reject it.
    EXPECT_EQ(store->read(QStringLiteral("a"), &value, &error), KeyStore::Result::Error);
}

TEST_F(KeyStorePassfileTest, wrong_passphrase_falls_back)
{
    {
        auto store = createPassphraseFileKeyStore(path(), QStringLiteral("right"));
        ASSERT_TRUE(store->write(QStringLiteral("svc"), QStringLiteral("v1")));
    }
    // Wrong passphrase → canary tag fails → factory returns nullptr so the
    // WASM wrapper falls back to the session store (no plaintext downgrade).
    EXPECT_EQ(createPassphraseFileKeyStore(path(), QStringLiteral("wrong")), nullptr);
    // The sealed blob is still intact for the correct passphrase.
    auto store = createPassphraseFileKeyStore(path(), QStringLiteral("right"));
    ASSERT_TRUE(store);
    QString value;
    EXPECT_EQ(store->read(QStringLiteral("svc"), &value), KeyStore::Result::Ok);
    EXPECT_EQ(value, QStringLiteral("v1"));
}

TEST_F(KeyStorePassfileTest, entry_size_limit)
{
    auto store = createPassphraseFileKeyStore(path(), QStringLiteral("pw"));
    const QString huge(70 * 1024, QLatin1Char('x'));
    QString error;
    EXPECT_FALSE(store->write(QStringLiteral("svc"), huge, &error));
    EXPECT_FALSE(error.isEmpty());
}
