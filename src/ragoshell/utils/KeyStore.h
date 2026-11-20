#pragma once

#include <memory>
#include <vector>

#include <QString>

#include "ragoshell/core/Enums.h"

namespace RagoShell
{
    /**
     * @brief A place for one small secret that the operating system protects:
     *        Windows Credential Manager, the macOS / iOS Keychain, the Linux
     *        Secret Service (libsecret). The app keeps the key that encrypts
     *        saved passwords here instead of in a file next to the settings.
     *
     * Methods are blocking and may be called from any one thread at a time.
     */
    class KeyStore
    {
    public:
        enum class Result { Ok, NotFound, Error };

        virtual ~KeyStore() = default;

        /// "Windows Credential Manager", "Keychain", "Secret Service".
        virtual QString name() const = 0;
        /// False when there is no usable store on this machine (a Linux session
        /// without a Secret Service, for example).
        virtual bool isAvailable() = 0;

        virtual Result read(const QString &account, QString *value, QString *error = nullptr) = 0;
        virtual bool write(const QString &account, const QString &value, QString *error = nullptr) = 0;
        virtual bool remove(const QString &account, QString *error = nullptr) = 0;
    };

    /// The store of this platform, or nullptr when there is none. RAGOSHELL_KEYSTORE=file
    /// forces "no store" (headless runs, tests, portable setups).
    std::unique_ptr<KeyStore> createPlatformKeyStore();

    /// A file holding AES-256-GCM-sealed entries keyed by an Argon2id-derived
    /// passphrase key (docs/WebStorage.md §2). Platform-neutral — the WASM
    /// build uses it when RAGOSHELL_KEYSTORE_PASS is set, tests exercise it
    /// natively. The caller's passphrase QString is not cleansed.
    std::unique_ptr<KeyStore> createPassphraseFileKeyStore(const QString &filePath,
                                                         const QString &passphrase);

    /// Account name of the encryption key. The default settings folder uses
    /// "ragoshell-key"; a RAGOSHELL_CONFIG_DIR gets its own account, so a
    /// throw-away folder can never replace the key of the real profile.
    QString encryptionKeyAccount(const QString &appDataDir, bool overridden);

    struct EncryptionKeyResult
    {
        struct Message
        {
            QString text;
            LogSeverity severity;
        };

        long long key = 0;
        bool inKeyStore = false;     ///< The key is held by the OS key store.
        bool storeAvailable = false; ///< A key store exists on this machine.
        bool migrated = false;       ///< The key file was moved into the key store.
        bool generated = false;      ///< There was no key; a new one was made.
        QString storeName;           ///< Name of the key store, when there is one.
        std::vector<Message> messages;
    };

    /**
     * Finds (or creates) the encryption key.
     *
     *  - key store has it: use it; a key file with the same key is removed
     *  - only a key file has it: move it into the key store, remove the file
     *    after the stored key was read back
     *  - nothing has it: make a key, store it
     *  - no usable key store: use (or create) the key file and say so
     *  - a store that cannot be read, or an entry that is not a key, is never
     *    written over: the key file is used instead
     *
     * "store" may be nullptr. A key file that is empty or not a number keeps
     * its old meaning (key 0) and is never moved or replaced.
     */
    EncryptionKeyResult resolveEncryptionKey(KeyStore *store, const QString &account,
                                             const QString &keyFilePath);
}
