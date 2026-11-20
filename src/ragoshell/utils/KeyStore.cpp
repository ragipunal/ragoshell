#include "ragoshell/utils/KeyStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTextStream>

#include "ragoshell/core/utils/WasmPersist.h"

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/utils/AppPaths.h"

namespace RagoShell
{
#if defined(Q_OS_WIN)
    std::unique_ptr<KeyStore> createWindowsKeyStore(const QString &service);
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    std::unique_ptr<KeyStore> createAppleKeyStore(const QString &service);
#elif defined(Q_OS_ANDROID)
    std::unique_ptr<KeyStore> createAndroidKeyStore();
#elif defined(Q_OS_LINUX)
    std::unique_ptr<KeyStore> createLibsecretKeyStore(const QString &service, const QByteArray &schemaName);
#elif defined(Q_OS_WASM)
    std::unique_ptr<KeyStore> createWasmKeyStore(const QString &service);
#endif

    namespace
    {
        std::unique_ptr<KeyStore> createStore(const char *service)
        {
            if (productEnv("KEYSTORE").compare(QLatin1String("file"), Qt::CaseInsensitive) == 0)
                return nullptr;
            const QString name = QString::fromUtf8(service);
#if defined(Q_OS_WIN)
            return createWindowsKeyStore(name);
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
            return createAppleKeyStore(name);
#elif defined(Q_OS_ANDROID)
            // One private folder of the app, no service name.
            Q_UNUSED(name);
            return createAndroidKeyStore();
#elif defined(Q_OS_LINUX)
            return createLibsecretKeyStore(name, "org." + name.toLower().toUtf8() + ".Secret");
#elif defined(Q_OS_WASM)
            return createWasmKeyStore(name);
#else
            Q_UNUSED(name);
            return nullptr;
#endif
        }

        QString accountFor(const QString &prefix, const QString &appDataDir, bool overridden)
        {
            if (!overridden)
                return prefix + QStringLiteral("-key");
            const QByteArray hash = QCryptographicHash::hash(
                QDir::cleanPath(appDataDir).toUtf8(), QCryptographicHash::Sha1);
            return prefix + QStringLiteral("-key-") + QString::fromLatin1(hash.toHex().left(12));
        }
    }

    std::unique_ptr<KeyStore> createPlatformKeyStore()
    {
        return createStore(BrandInfo::productName);
    }

    QString encryptionKeyAccount(const QString &appDataDir, bool overridden)
    {
        return accountFor(QString::fromLatin1(BrandInfo::productId).toLower(), appDataDir, overridden);
    }

    namespace
    {
        // Same range as the old generator ([2^61, 2^62)), so stored keys keep their format,
        // but all 61 free bits come from the OS CSPRNG: seeding mt19937_64 with one 32-bit
        // random_device value left only 2^32 possible keys (docs/SyncClient.md §10 R8).
        // 2^64 is a multiple of 2^61, so the modulo has no bias.
        long long generateKey()
        {
            constexpr quint64 kLow = quint64(1) << 61;
            return static_cast<long long>(kLow + QRandomGenerator::system()->generate64() % kLow);
        }

        bool parseKey(const QString &text, long long *key)
        {
            bool ok = false;
            const long long value = text.trimmed().toLongLong(&ok);
            if (!ok || value == 0)
                return false;
            *key = value;
            return true;
        }

        bool writeKeyFile(const QString &path, long long key)
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
                return false;
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            QTextStream out(&file);
            out << QString::number(key);
            out.flush();
            const bool ok = file.error() == QFileDevice::NoError;
            if (ok)
                requestPersistSync();
            return ok;
        }

        // Writes the key, reads it back and compares. On any failure the entry is removed
        // again, so a half-written value is never mistaken for a key on the next start.
        bool storeVerified(KeyStore *store, const QString &account, long long key, QString *error)
        {
            QString back;
            long long backKey = 0;
            if (store->write(account, QString::number(key), error)
                && store->read(account, &back, error) == KeyStore::Result::Ok
                && parseKey(back, &backKey) && backKey == key)
                return true;
            if (error && error->isEmpty())
                *error = QStringLiteral("the stored key reads back differently");
            store->remove(account, nullptr);
            return false;
        }
    }

    EncryptionKeyResult resolveEncryptionKey(KeyStore *store, const QString &account,
                                             const QString &keyFilePath)
    {
        using Message = EncryptionKeyResult::Message;
        EncryptionKeyResult r;
        auto say = [&r](LogSeverity severity, const QString &text) {
            r.messages.push_back(Message{ text, severity });
        };

        if (store) {
            r.storeName = store->name();
            r.storeAvailable = store->isAvailable();
        }

        // The key file, if any. An unreadable or non-numeric one keeps the old
        // behaviour (key 0) and is left alone.
        bool haveFile = false;
        long long fileKey = 0;
        bool fileKeyValid = false;
        auto loadFile = [&]() {
            const QFileInfo fileInfo(keyFilePath);
            haveFile = fileInfo.exists() && fileInfo.isFile();
            fileKey = 0;
            fileKeyValid = false;
            if (!haveFile)
                return;
            QFile file(keyFilePath);
            if (!file.open(QIODevice::ReadOnly)) {
                say(LogSeverity::Error, QStringLiteral("RagoCrypt: Failed to open key file: %1").arg(keyFilePath));
                return;
            }
            const QString text = QTextStream(&file).readAll();
            if (text.isEmpty())
                say(LogSeverity::Error, QStringLiteral("RagoCrypt: Key file is empty: %1").arg(keyFilePath));
            fileKeyValid = parseKey(text, &fileKey);
        };
        loadFile();

        if (r.storeAvailable) {
            QString stored, error;
            long long storedKey = 0;
            const KeyStore::Result read = store->read(account, &stored, &error);
            const bool storedValid = read == KeyStore::Result::Ok && parseKey(stored, &storedKey);
            if (read == KeyStore::Result::Ok && !storedValid)
                error = QStringLiteral("the entry does not hold a key");

            if (storedValid) {
                r.key = storedKey;
                r.inKeyStore = true;
                if (haveFile && fileKeyValid && fileKey == storedKey) {
                    if (QFile::remove(keyFilePath)) {
                        requestPersistSync();
                        say(LogSeverity::Info, QStringLiteral("RagoCrypt: Removed the old key file %1; the key is in %2")
                                                   .arg(keyFilePath, r.storeName));
                    } else
                        say(LogSeverity::Warning, QStringLiteral("RagoCrypt: Could not remove the old key file %1")
                                                      .arg(keyFilePath));
                } else if (haveFile && fileKeyValid) {
                    say(LogSeverity::Warning, QStringLiteral("RagoCrypt: %1 holds a different key than %2; using the one in %1 and keeping the file")
                                                  .arg(r.storeName, keyFilePath));
                }
                return r;
            }

            if (read == KeyStore::Result::NotFound) {
                // Move the file's key, or make a new key, into the store; keep the file
                // until the stored key has been read back.
                const bool fromFile = haveFile && fileKeyValid;
                if (haveFile && !fileKeyValid) {
                    // An unusable key file: the old behaviour (key 0), nothing is moved.
                    r.key = 0;
                    say(LogSeverity::Warning, QStringLiteral("RagoCrypt: The key file %1 does not hold a key; not moved to %2")
                                                  .arg(keyFilePath, r.storeName));
                    return r;
                }
                const long long key = fromFile ? fileKey : generateKey();
                if (storeVerified(store, account, key, &error)) {
                    r.key = key;
                    r.inKeyStore = true;
                    if (fromFile) {
                        r.migrated = true;
                        if (QFile::remove(keyFilePath)) {
                            requestPersistSync();
                            say(LogSeverity::Info, QStringLiteral("RagoCrypt: Moved the key from %1 into %2")
                                                       .arg(keyFilePath, r.storeName));
                        }
                        else
                            say(LogSeverity::Warning, QStringLiteral("RagoCrypt: Moved the key into %1 but could not remove %2")
                                                          .arg(r.storeName, keyFilePath));
                    } else {
                        r.generated = true;
                        say(LogSeverity::Info, QStringLiteral("RagoCrypt: No key found, made a new one in %1").arg(r.storeName));
                    }
                    return r;
                }
                say(LogSeverity::Warning, QStringLiteral("RagoCrypt: Could not keep the key in %1 (%2); using a key file")
                                              .arg(r.storeName, error));
                r.storeAvailable = false;     // fall through to the key file
            } else {
                say(LogSeverity::Warning, QStringLiteral("RagoCrypt: Could not read the key from %1 (%2); using a key file")
                                              .arg(r.storeName, error));
                r.storeAvailable = false;
            }
        } else {
            say(LogSeverity::Warning,
                store ? QStringLiteral("RagoCrypt: %1 is not available; the key stays in a file").arg(r.storeName)
                      : QStringLiteral("RagoCrypt: No key store on this machine; the key stays in a file"));
        }

        // Key file.
        if (haveFile) {
            r.key = fileKeyValid ? fileKey : 0;
            return r;
        }
        say(LogSeverity::Warning, QStringLiteral("RagoCrypt: No key found, generating a new key and saving it into file"));
        r.key = generateKey();
        r.generated = true;
        if (!writeKeyFile(keyFilePath, r.key))
            say(LogSeverity::Error, QStringLiteral("RagoCrypt: Failed to save the key into file: %1").arg(keyFilePath));
        return r;
    }
}
