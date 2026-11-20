#include "ragoshell/utils/KeyStore.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "ragoshell/core/cloud/SyncCrypto.h"
#include "ragoshell/core/utils/WasmPersist.h"

namespace RagoShell
{
    namespace
    {
        // Keystore blob: {"kdf":<kdf_params>, "entries":{<account-b64>:
        // {"ct":<ciphertext-b64>, "nonce":<nonce-b64>}}}. AAD binds each entry
        // to its account so entries cannot be swapped between accounts (T6).
        constexpr int kMaxEntries = 256;
        constexpr int kMaxEntryBytes = 64 * 1024;
        constexpr qint64 kMaxBlobBytes = 32 * 1024 * 1024;
        constexpr const char *kAadPrefix = "ragoshell-keystore/v1/";
        // Sealed at first store() and re-opened at every load(): a wrong
        // passphrase yields a different key, the tag fails and the store
        // reports "passphrase does not match" instead of silently poisoning
        // entries under the wrong key (security review finding 1).
        constexpr const char *kSelfCheckAccount = "_selfcheck";
        constexpr const char *kSelfCheckPlaintext = "ragoshell-keystore-canary";

        QString aadFor(const QString &account)
        {
            return QString::fromUtf8(kAadPrefix) + account;
        }

        // The local blob is self-authored: its KDF params must be exactly the
        // shipped constants (salt aside) — parseKdfParams' wider bounds are
        // sized for a hostile sync server and a tampered file could otherwise
        // stall startup with a 1 GiB Argon2 alloc (review finding 7).
        bool kdfMatchesShipped(const QJsonObject &kdf, QString *error)
        {
            SyncCrypto::KdfParams params;
            if (!SyncCrypto::parseKdfParams(kdf, &params, error))
                return false;
            return params.memoryKiB == SyncCrypto::kKdfMemoryKiB
                   && params.iterations == SyncCrypto::kKdfIterations
                   && params.parallelism == SyncCrypto::kKdfParallelism;
        }

        class PassphraseFileKeyStore : public KeyStore
        {
        public:
            PassphraseFileKeyStore(const QString &filePath, const QString &passphrase)
                : _filePath(filePath)
            {
                SyncCrypto::KdfParams params;
                params.salt = SyncCrypto::generateSalt();
                _masterKey = SyncCrypto::deriveMasterKey(passphrase, params);
                _kdfParams = SyncCrypto::buildKdfParams(params);
            }

            PassphraseFileKeyStore(const QString &filePath, QByteArray masterKey,
                                   const QJsonObject &kdfParams)
                : _filePath(filePath), _masterKey(std::move(masterKey)),
                  _kdfParams(kdfParams)
            {
            }

            ~PassphraseFileKeyStore() override { SyncCrypto::cleanse(&_masterKey); }

            QString name() const override { return QStringLiteral("Browser (passphrase)"); }
            bool isAvailable() override { return !_masterKey.isEmpty(); }

            Result read(const QString &account, QString *value, QString *error = nullptr) override
            {
                QJsonObject entries;
                if (!load(&entries, error))
                    return Result::Error;
                const QJsonObject entry = entries.value(accountKey(account)).toObject();
                if (entry.isEmpty())
                    return Result::NotFound;

                QByteArray plaintext;
                QString err;
                if (!SyncCrypto::decryptEnvelope(
                        _masterKey, aadFor(account),
                        QByteArray::fromBase64(entry.value("ct").toString().toUtf8()),
                        QByteArray::fromBase64(entry.value("nonce").toString().toUtf8()),
                        &plaintext, &err)) {
                    if (error)
                        *error = err;
                    return Result::Error; // T5: fail closed, keep the file
                }
                *value = QString::fromUtf8(plaintext);
                SyncCrypto::cleanse(&plaintext);
                return Result::Ok;
            }

            bool write(const QString &account, const QString &value, QString *error = nullptr) override
            {
                if (account.isEmpty() || account == QLatin1String(kSelfCheckAccount)
                    || value.toUtf8().size() > kMaxEntryBytes) {
                    if (error)
                        *error = QStringLiteral("Invalid keystore entry.");
                    return false;
                }
                QJsonObject entries;
                if (!load(&entries, error))
                    return false;
                if (entries.size() >= kMaxEntries
                    && !entries.contains(accountKey(account))) {
                    if (error)
                        *error = QStringLiteral("Keystore entry limit reached.");
                    return false;
                }

                QByteArray ciphertext, nonce;
                QString err;
                if (!SyncCrypto::encryptEnvelope(_masterKey, aadFor(account),
                                                 value.toUtf8(), &ciphertext, &nonce, &err)) {
                    if (error)
                        *error = err;
                    return false;
                }
                entries.insert(accountKey(account),
                               QJsonObject{{"ct", QString::fromUtf8(ciphertext.toBase64())},
                                           {"nonce", QString::fromUtf8(nonce.toBase64())}});
                return store(entries, error);
            }

            bool remove(const QString &account, QString *error = nullptr) override
            {
                if (account == QLatin1String(kSelfCheckAccount)) {
                    if (error)
                        *error = QStringLiteral("Invalid keystore entry.");
                    return false;
                }
                QJsonObject entries;
                if (!load(&entries, error))
                    return false;
                entries.remove(accountKey(account));
                return store(entries, error);
            }

        private:
            static QString accountKey(const QString &account)
            {
                return QString::fromUtf8(account.toUtf8().toBase64());
            }

            bool load(QJsonObject *entries, QString *error)
            {
                *entries = QJsonObject();
                QFile f(_filePath);
                if (!f.exists())
                    return true; // first run: empty store
                if (f.size() > kMaxBlobBytes) {
                    if (error)
                        *error = QStringLiteral("Keystore blob exceeds the size limit.");
                    return false;
                }
                if (!f.open(QIODevice::ReadOnly)) {
                    if (error)
                        *error = f.errorString();
                    return false;
                }
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                const QJsonObject kdf = root.value("kdf").toObject();
                if (kdf != _kdfParams) {
                    // Passphrase derives a key under the file's own params; a
                    // mismatch means tampering, not a wrong passphrase.
                    if (error)
                        *error = QStringLiteral("Keystore passphrase does not match.");
                    return false;
                }
                *entries = root.value("entries").toObject();
                const QJsonObject canary = entries->value(accountKey(QLatin1String(kSelfCheckAccount))).toObject();
                if (!canary.isEmpty()) {
                    QByteArray probe;
                    QString err;
                    if (!SyncCrypto::decryptEnvelope(
                            _masterKey, aadFor(QLatin1String(kSelfCheckAccount)),
                            QByteArray::fromBase64(canary.value("ct").toString().toUtf8()),
                            QByteArray::fromBase64(canary.value("nonce").toString().toUtf8()),
                            &probe, &err)
                        || probe != kSelfCheckPlaintext) {
                        SyncCrypto::cleanse(&probe);
                        if (error)
                            *error = QStringLiteral("Keystore passphrase does not match.");
                        return false;
                    }
                    SyncCrypto::cleanse(&probe);
                }
                return true;
            }

            bool store(QJsonObject entries, QString *error)
            {
                // Seal the canary once so a wrong-passphrase key fails at the
                // next load() instead of corrupting real entries.
                if (!entries.contains(accountKey(QLatin1String(kSelfCheckAccount)))) {
                    QByteArray ciphertext, nonce;
                    QString err;
                    if (SyncCrypto::encryptEnvelope(
                            _masterKey, aadFor(QLatin1String(kSelfCheckAccount)),
                            QByteArray(kSelfCheckPlaintext), &ciphertext, &nonce, &err)) {
                        entries.insert(accountKey(QLatin1String(kSelfCheckAccount)),
                                       QJsonObject{{"ct", QString::fromUtf8(ciphertext.toBase64())},
                                                   {"nonce", QString::fromUtf8(nonce.toBase64())}});
                    }
                }
                QSaveFile f(_filePath);
                if (!f.open(QIODevice::WriteOnly)) {
                    if (error)
                        *error = f.errorString();
                    return false;
                }
                const QJsonObject root{{"kdf", _kdfParams}, {"entries", entries}};
                f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
                if (!f.commit()) {
                    if (error)
                        *error = f.errorString();
                    return false;
                }
                QFile::setPermissions(_filePath,
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                requestPersistSync();
                return true;
            }

            QString _filePath;
            QByteArray _masterKey;
            QJsonObject _kdfParams;
        };
    }

    std::unique_ptr<KeyStore> createPassphraseFileKeyStore(const QString &filePath,
                                                         const QString &passphrase)
    {
        // Existing file? Re-derive with its own persisted salt so the same
        // passphrase reopens the store after a reload.
        QFile f(filePath);
        if (f.exists() && f.size() <= kMaxBlobBytes && f.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            QString err;
            const QJsonObject kdf = root.value("kdf").toObject();
            if (kdfMatchesShipped(kdf, &err)) {
                SyncCrypto::KdfParams params;
                SyncCrypto::parseKdfParams(kdf, &params);
                QByteArray key = SyncCrypto::deriveMasterKey(passphrase, params);
                if (!key.isEmpty()) {
                    auto store = std::make_unique<PassphraseFileKeyStore>(
                        filePath, std::move(key), kdf);
                    // Wrong passphrase → canary tag fails → return nullptr so
                    // the caller falls back to the session store (finding 1).
                    QString probe;
                    if (store->read(QLatin1String(kSelfCheckAccount), &probe)
                        != KeyStore::Result::Error) {
                        return store;
                    }
                }
            }
            return nullptr;
        }
        auto store = std::make_unique<PassphraseFileKeyStore>(filePath, passphrase);
        return store->isAvailable() ? std::move(store) : nullptr;
    }
}
