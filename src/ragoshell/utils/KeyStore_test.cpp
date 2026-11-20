#include "gtest/gtest.h"

#include <functional>
#include <iostream>
#include <map>
#include <set>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>

#include "ragoshell/utils/KeyStore.h"

using namespace RagoShell;

namespace
{
    class FakeKeyStore : public KeyStore
    {
    public:
        QString name() const override { return QStringLiteral("Fake store"); }
        bool isAvailable() override { return available; }

        Result read(const QString &account, QString *value, QString *error) override
        {
            if (beforeRead) {
                auto hook = std::move(beforeRead);
                beforeRead = nullptr;
                hook();
            }
            if (failRead) {
                if (error) *error = QStringLiteral("read failed");
                return Result::Error;
            }
            auto it = items.find(account);
            if (it == items.end())
                return Result::NotFound;
            if (value) *value = it->second;
            return Result::Ok;
        }

        bool write(const QString &account, const QString &value, QString *error) override
        {
            ++writes;
            if (failWrite) {
                if (error) *error = QStringLiteral("write failed");
                return false;
            }
            items[account] = corruptWrites ? QStringLiteral("garbage") : value;
            return true;
        }

        bool remove(const QString &account, QString *) override { return items.erase(account) > 0; }

        bool available = true;
        bool failRead = false;
        bool failWrite = false;
        bool corruptWrites = false;
        int writes = 0;
        std::map<QString, QString> items;
        std::function<void()> beforeRead;   ///< runs once, before the next read
    };

    void writeFile(const QString &path, const QString &text)
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(text.toUtf8());
    }

    bool hasMessage(const EncryptionKeyResult &r, LogSeverity severity, const QString &part)
    {
        for (const auto &m : r.messages)
            if (m.severity == severity && m.text.contains(part))
                return true;
        return false;
    }

    const QString kAccount = QStringLiteral("test-key");
}

TEST(KeyStoreTest, an_existing_key_file_moves_into_the_store_and_the_file_goes)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    writeFile(file, "2305843009213693953");
    FakeKeyStore store;

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_EQ(r.key, 2305843009213693953LL);       // the same key: saved passwords still decrypt
    EXPECT_TRUE(r.inKeyStore);
    EXPECT_TRUE(r.migrated);
    EXPECT_FALSE(r.generated);
    EXPECT_EQ(store.items[kAccount], QStringLiteral("2305843009213693953"));
    EXPECT_FALSE(QFileInfo::exists(file));
}

TEST(KeyStoreTest, the_file_stays_when_the_stored_key_cannot_be_read_back)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    writeFile(file, "2305843009213693953");
    FakeKeyStore store;
    store.corruptWrites = true;                    // write "succeeds", read-back differs

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_EQ(r.key, 2305843009213693953LL);
    EXPECT_FALSE(r.inKeyStore);
    EXPECT_FALSE(r.migrated);
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(hasMessage(r, LogSeverity::Warning, "using a key file"));
}

TEST(KeyStoreTest, a_failing_store_write_keeps_the_file_key)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    writeFile(file, "2305843009213693953");
    FakeKeyStore store;
    store.failWrite = true;

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_EQ(r.key, 2305843009213693953LL);
    EXPECT_FALSE(r.inKeyStore);
    EXPECT_TRUE(QFileInfo::exists(file));
}

TEST(KeyStoreTest, the_store_key_wins_and_a_matching_old_file_is_removed)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    writeFile(file, "2305843009213693953");        // left over from an interrupted migration
    FakeKeyStore store;
    store.items[kAccount] = QStringLiteral("2305843009213693953");

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_EQ(r.key, 2305843009213693953LL);
    EXPECT_TRUE(r.inKeyStore);
    EXPECT_FALSE(r.migrated);
    EXPECT_EQ(store.writes, 0);
    EXPECT_FALSE(QFileInfo::exists(file));
}

TEST(KeyStoreTest, different_keys_in_store_and_file_use_the_store_and_keep_the_file)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    writeFile(file, "2305843009213693953");
    FakeKeyStore store;
    store.items[kAccount] = QStringLiteral("2305843009213693999");

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_EQ(r.key, 2305843009213693999LL);
    EXPECT_TRUE(r.inKeyStore);
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(hasMessage(r, LogSeverity::Warning, "different key"));
}

TEST(KeyStoreTest, no_key_anywhere_makes_one_in_the_store_and_no_file)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    FakeKeyStore store;

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_TRUE(r.generated);
    EXPECT_TRUE(r.inKeyStore);
    EXPECT_GE(r.key, 1LL << 61);
    EXPECT_EQ(store.items[kAccount], QString::number(r.key));
    EXPECT_FALSE(QFileInfo::exists(file));

    // The next start finds the same key.
    const EncryptionKeyResult again = resolveEncryptionKey(&store, kAccount, file);
    EXPECT_EQ(again.key, r.key);
    EXPECT_FALSE(again.generated);
}

// docs/SyncClient.md §10 R8: the generator changed (OS CSPRNG instead of a 32-bit seeded
// mt19937_64) but the stored format and range must not, or existing passwords stop decrypting.
TEST(KeyStoreTest, generated_keys_keep_the_old_range_and_do_not_repeat)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    std::set<long long> seen;
    for (int i = 0; i < 64; ++i) {
        FakeKeyStore store;
        const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);
        ASSERT_TRUE(r.generated);
        EXPECT_GE(r.key, 1LL << 61);
        EXPECT_LT(r.key, 1LL << 62);
        bool ok = false;
        EXPECT_EQ(store.items[kAccount].toLongLong(&ok), r.key);
        EXPECT_TRUE(ok);
        seen.insert(r.key);
    }
    EXPECT_EQ(seen.size(), 64u);
}

TEST(KeyStoreTest, without_a_usable_store_the_key_file_is_used_with_a_warning)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    FakeKeyStore store;
    store.available = false;

    const EncryptionKeyResult first = resolveEncryptionKey(&store, kAccount, file);
    EXPECT_TRUE(first.generated);
    EXPECT_FALSE(first.inKeyStore);
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(hasMessage(first, LogSeverity::Warning, "not available"));
    EXPECT_EQ(store.writes, 0);

    const EncryptionKeyResult second = resolveEncryptionKey(nullptr, kAccount, file);   // no store at all
    EXPECT_EQ(second.key, first.key);
    EXPECT_FALSE(second.inKeyStore);
    EXPECT_TRUE(hasMessage(second, LogSeverity::Warning, "No key store"));
}

TEST(KeyStoreTest, a_store_that_cannot_be_read_falls_back_to_the_file)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    FakeKeyStore store;
    store.failRead = true;

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_TRUE(r.generated);
    EXPECT_FALSE(r.inKeyStore);
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_EQ(store.writes, 0);                    // a store that cannot be read is not written to
}

TEST(KeyStoreTest, an_empty_key_file_keeps_the_old_meaning_and_is_never_moved)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    writeFile(file, "");
    FakeKeyStore store;

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_EQ(r.key, 0);
    EXPECT_FALSE(r.inKeyStore);
    EXPECT_EQ(store.writes, 0);
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(hasMessage(r, LogSeverity::Error, "empty"));
}

TEST(KeyStoreTest, an_overridden_settings_folder_gets_its_own_account)
{
    EXPECT_EQ(encryptionKeyAccount("C:/Users/me/.ragoshell/", false), QStringLiteral("ragoshell-key"));
    const QString a = encryptionKeyAccount("C:/tmp/one", true);
    const QString b = encryptionKeyAccount("C:/tmp/two", true);
    EXPECT_NE(a, b);
    EXPECT_NE(a, QStringLiteral("ragoshell-key"));
    EXPECT_TRUE(a.startsWith(QStringLiteral("ragoshell-key-")));
    EXPECT_EQ(a, encryptionKeyAccount("C:/tmp/one/", true));      // the same folder, however it is written
}

namespace
{
    // Sets one environment variable for a test (null: unset) and puts the old value back.
    class EnvOverride
    {
    public:
        EnvOverride(const char *name, const char *value)
            : _name(name), _hadValue(qEnvironmentVariableIsSet(name)), _old(qgetenv(name))
        {
            if (value)
                qputenv(name, value);
            else
                qunsetenv(name);
        }
        ~EnvOverride()
        {
            if (_hadValue)
                qputenv(_name.constData(), _old);
            else
                qunsetenv(_name.constData());
        }

    private:
        QByteArray _name;
        bool _hadValue;
        QByteArray _old;
    };
}

TEST(KeyStoreTest, the_file_value_can_be_forced_with_an_environment_variable)
{
    EnvOverride current("RAGOSHELL_KEYSTORE", "file");
    EXPECT_EQ(createPlatformKeyStore(), nullptr);
}

TEST(KeyStoreTest, a_store_entry_that_is_not_a_key_is_never_overwritten)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("ragoshell.key");
    FakeKeyStore store;
    store.items[kAccount] = QStringLiteral("garbage");

    const EncryptionKeyResult r = resolveEncryptionKey(&store, kAccount, file);

    EXPECT_EQ(store.writes, 0);                    // whatever is there is left for a person to look at
    EXPECT_EQ(store.items[kAccount], QStringLiteral("garbage"));
    EXPECT_FALSE(r.inKeyStore);
    EXPECT_TRUE(r.generated);                      // a key file instead
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(hasMessage(r, LogSeverity::Warning, "does not hold a key"));
}

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
TEST(KeyStoreTest, platform_key_store_round_trip)
{
    EnvOverride current("RAGOSHELL_KEYSTORE", nullptr);
    const std::unique_ptr<KeyStore> store = createPlatformKeyStore();
    ASSERT_NE(store, nullptr);
    if (!store->isAvailable()) {
        std::cerr << "[ SKIP ] " << qPrintable(store->name())
                  << " skipped for ad-hoc binary (sign with a certificate or set RAGOSHELL_KEYSTORE=keychain)\n";
        return;
    }
#ifdef Q_OS_WIN
    EXPECT_EQ(store->name(), QStringLiteral("Windows Credential Manager"));
#else
    EXPECT_EQ(store->name(), QStringLiteral("Keychain"));
#endif

    const QString account = QStringLiteral("unit-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString value, error;
    EXPECT_EQ(store->read(account, &value, &error), KeyStore::Result::NotFound);

    const QString secret = QStringLiteral("2305843009213693953 çğıöşü 日本語");
    if (!store->write(account, secret, &error) && error.contains(QLatin1String("-25308"))) {
        // A locked login keychain in a session without a screen (ssh): nothing can be asked of the user.
        std::cerr << "[ SKIP ] the keychain is locked and cannot ask for a password here: " << qPrintable(error) << "\n";
        return;
    }
    ASSERT_TRUE(store->write(account, secret, &error)) << qPrintable(error);
    ASSERT_EQ(store->read(account, &value, &error), KeyStore::Result::Ok) << qPrintable(error);
    EXPECT_EQ(value, secret);

    ASSERT_TRUE(store->write(account, QStringLiteral("changed"), &error));       // replaces
    ASSERT_EQ(store->read(account, &value, &error), KeyStore::Result::Ok);
    EXPECT_EQ(value, QStringLiteral("changed"));

    EXPECT_TRUE(store->remove(account, &error));
    EXPECT_EQ(store->read(account, &value, &error), KeyStore::Result::NotFound);
    EXPECT_TRUE(store->remove(account, &error));                                 // already gone is fine
}
#endif
