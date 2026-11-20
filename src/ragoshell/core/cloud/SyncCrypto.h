#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace RagoShell
{
    /**
     * @brief Cryptographic envelope layer for zero-knowledge cloud sync (docs/SyncClient.md).
     *
     * Argon2id (libargon2) derives the 32-byte master key from the user passphrase; HKDF-SHA256
     * derives a per-collection AEAD key; AES-256-GCM (OpenSSL EVP) seals the canonical JSON
     * payload. The server only ever sees {ciphertext, nonce, kdf_params}.
     */
    namespace SyncCrypto
    {
        constexpr int kMasterKeyBytes = 32;
        constexpr int kSaltBytes = 16;
        constexpr int kNonceBytes = 12;      // standard AES-256-GCM nonce
        constexpr int kGcmTagBytes = 16;
        constexpr int kSchemaVersion = 1;

        // Argon2id parameters mirrored into non-secret kdf_params (SyncProtocol.md §1.1).
        constexpr uint32_t kKdfMemoryKiB = 65536;
        constexpr uint32_t kKdfIterations = 3;
        constexpr uint32_t kKdfParallelism = 4;
        constexpr int kMaxKdfParamsBytes = 4096;

        struct KdfParams
        {
            uint32_t memoryKiB = kKdfMemoryKiB;
            uint32_t iterations = kKdfIterations;
            uint32_t parallelism = kKdfParallelism;
            QByteArray salt;                 // kSaltBytes after validation
        };

        /// Fresh CSPRNG material (OpenSSL RAND_bytes).
        QByteArray randomBytes(int len);
        inline QByteArray generateSalt() { return randomBytes(kSaltBytes); }
        inline QByteArray generateNonce() { return randomBytes(kNonceBytes); }
        /// Device-key bytes for passphrase-less mode (stored in KeyStore).
        inline QByteArray generateDeviceKey() { return randomBytes(kMasterKeyBytes); }

        /// Builds the public kdf_params JSON object (never contains key material).
        QJsonObject buildKdfParams(const KdfParams &params);
        /// Strict validation of a kdf_params object received/stored; false on any deviation.
        bool parseKdfParams(const QJsonObject &obj, KdfParams *out, QString *error = nullptr);

        /// Argon2id(passphrase, params) -> 32-byte master key; empty on failure.
        QByteArray deriveMasterKey(const QString &passphrase, const KdfParams &params);

        /// HKDF-SHA256(masterKey, salt="", info="ragoshell-sync/v1/<collection>") -> 32-byte key.
        QByteArray deriveCollectionKey(const QByteArray &masterKey, const QString &collection);

        /// AES-256-GCM seal; AAD binds ciphertext to the collection. Empty ciphertext on failure.
        bool encryptEnvelope(const QByteArray &collectionKey,
                             const QString &collection,
                             const QByteArray &plaintext,
                             QByteArray *ciphertextOut,
                             QByteArray *nonceOut,
                             QString *error = nullptr);

        /// AES-256-GCM open with tag verification; false on any auth/format failure.
        bool decryptEnvelope(const QByteArray &collectionKey,
                             const QString &collection,
                             const QByteArray &ciphertext,
                             const QByteArray &nonce,
                             QByteArray *plaintextOut,
                             QString *error = nullptr);

        /// Explicit memory wipe for buffers that held key material or plaintext.
        void cleanse(QByteArray *buf);
    }
}
