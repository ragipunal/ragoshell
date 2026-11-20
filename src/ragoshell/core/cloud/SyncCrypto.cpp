#include "ragoshell/core/cloud/SyncCrypto.h"

#include <argon2.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <QJsonDocument>

#include <memory>

namespace RagoShell
{
namespace SyncCrypto
{
namespace
{
    QByteArray aadFor(const QString &collection)
    {
        return QStringLiteral("ragoshell-sync-v1:%1").arg(collection).toUtf8();
    }
}

QByteArray randomBytes(int len)
{
    if (len <= 0 || len > (1 << 20))
        return {};
    QByteArray out(len, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), len) != 1)
        return {};
    return out;
}

QJsonObject buildKdfParams(const KdfParams &params)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("alg"), QStringLiteral("argon2id"));
    obj.insert(QStringLiteral("v"), 1);
    obj.insert(QStringLiteral("m_cost_kib"), static_cast<qint64>(params.memoryKiB));
    obj.insert(QStringLiteral("t_cost"), static_cast<qint64>(params.iterations));
    obj.insert(QStringLiteral("parallelism"), static_cast<qint64>(params.parallelism));
    obj.insert(QStringLiteral("salt"), QString::fromLatin1(params.salt.toBase64()));
    obj.insert(QStringLiteral("cipher"), QStringLiteral("aes-256-gcm"));
    obj.insert(QStringLiteral("schema_version"), kSchemaVersion);
    return obj;
}

bool parseKdfParams(const QJsonObject &obj, KdfParams *out, QString *error)
{
    auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    if (obj.value(QStringLiteral("alg")).toString() != QLatin1String("argon2id"))
        return fail(QStringLiteral("kdf_alg_unsupported"));
    if (obj.value(QStringLiteral("v")).toInt() != 1)
        return fail(QStringLiteral("kdf_version_unsupported"));
    if (obj.value(QStringLiteral("cipher")).toString() != QLatin1String("aes-256-gcm"))
        return fail(QStringLiteral("cipher_unsupported"));
    if (obj.value(QStringLiteral("schema_version")).toInt() != kSchemaVersion)
        return fail(QStringLiteral("schema_version_unsupported"));

    const qint64 m = obj.value(QStringLiteral("m_cost_kib")).toInteger(-1);
    const qint64 t = obj.value(QStringLiteral("t_cost")).toInteger(-1);
    const qint64 p = obj.value(QStringLiteral("parallelism")).toInteger(-1);
    // Inbound params must be at least as strong as the shipped defaults — a hostile
    // server may not weaken the KDF on an attach/pull path — while still bounded so
    // a hostile/corrupt envelope cannot request absurd allocations.
    if (m < kKdfMemoryKiB || m > (1 << 20))
        return fail(QStringLiteral("kdf_memory_out_of_range"));
    if (t < kKdfIterations || t > 32)
        return fail(QStringLiteral("kdf_iterations_out_of_range"));
    if (p < kKdfParallelism || p > 16)
        return fail(QStringLiteral("kdf_parallelism_out_of_range"));

    const QByteArray salt = QByteArray::fromBase64(
        obj.value(QStringLiteral("salt")).toString().toLatin1());
    if (salt.size() != kSaltBytes)
        return fail(QStringLiteral("kdf_salt_invalid"));

    if (out)
    {
        out->memoryKiB = static_cast<uint32_t>(m);
        out->iterations = static_cast<uint32_t>(t);
        out->parallelism = static_cast<uint32_t>(p);
        out->salt = salt;
    }
    return true;
}

QByteArray deriveMasterKey(const QString &passphrase, const KdfParams &params)
{
    if (params.salt.size() != kSaltBytes)
        return {};
    QByteArray pwd = passphrase.toUtf8();
    QByteArray key(kMasterKeyBytes, '\0');
    const int rc = argon2_hash(
        params.iterations, params.memoryKiB, params.parallelism,
        pwd.constData(), static_cast<size_t>(pwd.size()),
        params.salt.constData(), static_cast<size_t>(params.salt.size()),
        key.data(), static_cast<size_t>(key.size()),
        nullptr, 0,
        Argon2_id, ARGON2_VERSION_NUMBER);
    OPENSSL_cleanse(pwd.data(), static_cast<size_t>(pwd.size()));
    if (rc != ARGON2_OK)
    {
        cleanse(&key);
        return {};
    }
    return key;
}

QByteArray deriveCollectionKey(const QByteArray &masterKey, const QString &collection)
{
    if (masterKey.size() != kMasterKeyBytes)
        return {};
    const QByteArray info = QStringLiteral("ragoshell-sync/v1/%1").arg(collection).toUtf8();

    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0)
        return {};
    QByteArray key(kMasterKeyBytes, '\0');
    size_t outLen = static_cast<size_t>(key.size());
    const int ok =
        EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) > 0
        && EVP_PKEY_CTX_set1_hkdf_key(ctx.get(),
                                      reinterpret_cast<const unsigned char *>(masterKey.constData()),
                                      masterKey.size()) > 0
        && EVP_PKEY_CTX_add1_hkdf_info(ctx.get(),
                                       reinterpret_cast<const unsigned char *>(info.constData()),
                                       info.size()) > 0
        && EVP_PKEY_derive(ctx.get(),
                           reinterpret_cast<unsigned char *>(key.data()),
                           &outLen) > 0
        && outLen == static_cast<size_t>(kMasterKeyBytes);
    if (!ok)
    {
        cleanse(&key);
        return {};
    }
    return key;
}

bool encryptEnvelope(const QByteArray &collectionKey,
                     const QString &collection,
                     const QByteArray &plaintext,
                     QByteArray *ciphertextOut,
                     QByteArray *nonceOut,
                     QString *error)
{
    auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    if (collectionKey.size() != kMasterKeyBytes)
        return fail(QStringLiteral("bad_key_length"));

    const QByteArray nonce = generateNonce();
    const QByteArray aad = aadFor(collection);
    QByteArray ciphertext(plaintext.size() + kGcmTagBytes, '\0');

    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx)
        return fail(QStringLiteral("evp_ctx"));
    int len = 0;
    int total = 0;
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1
        || EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                            reinterpret_cast<const unsigned char *>(collectionKey.constData()),
                            reinterpret_cast<const unsigned char *>(nonce.constData())) != 1
        || EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                             reinterpret_cast<const unsigned char *>(aad.constData()),
                             aad.size()) != 1
        || EVP_EncryptUpdate(ctx.get(),
                             reinterpret_cast<unsigned char *>(ciphertext.data()), &len,
                             reinterpret_cast<const unsigned char *>(plaintext.constData()),
                             plaintext.size()) != 1)
        return fail(QStringLiteral("gcm_encrypt"));
    total += len;
    if (EVP_EncryptFinal_ex(ctx.get(),
                            reinterpret_cast<unsigned char *>(ciphertext.data()) + total,
                            &len) != 1)
        return fail(QStringLiteral("gcm_final"));
    total += len;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kGcmTagBytes,
                            ciphertext.data() + total) != 1)
        return fail(QStringLiteral("gcm_tag"));
    ciphertext.resize(total + kGcmTagBytes);

    if (ciphertextOut) *ciphertextOut = ciphertext;
    if (nonceOut) *nonceOut = nonce;
    return true;
}

bool decryptEnvelope(const QByteArray &collectionKey,
                     const QString &collection,
                     const QByteArray &ciphertext,
                     const QByteArray &nonce,
                     QByteArray *plaintextOut,
                     QString *error)
{
    auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    if (collectionKey.size() != kMasterKeyBytes)
        return fail(QStringLiteral("bad_key_length"));
    if (nonce.size() != kNonceBytes)
        return fail(QStringLiteral("bad_nonce_length"));
    if (ciphertext.size() < kGcmTagBytes)
        return fail(QStringLiteral("ciphertext_too_short"));

    const QByteArray aad = aadFor(collection);
    const int bodyLen = ciphertext.size() - kGcmTagBytes;
    QByteArray plaintext(bodyLen, '\0');
    QByteArray tag = ciphertext.right(kGcmTagBytes);

    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx)
        return fail(QStringLiteral("evp_ctx"));
    int len = 0;
    int total = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1
        || EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                            reinterpret_cast<const unsigned char *>(collectionKey.constData()),
                            reinterpret_cast<const unsigned char *>(nonce.constData())) != 1
        || EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                             reinterpret_cast<const unsigned char *>(aad.constData()),
                             aad.size()) != 1
        || EVP_DecryptUpdate(ctx.get(),
                             reinterpret_cast<unsigned char *>(plaintext.data()), &len,
                             reinterpret_cast<const unsigned char *>(ciphertext.constData()),
                             bodyLen) != 1)
        return fail(QStringLiteral("gcm_decrypt"));
    total += len;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kGcmTagBytes,
                            tag.data()) != 1
        || EVP_DecryptFinal_ex(ctx.get(),
                               reinterpret_cast<unsigned char *>(plaintext.data()) + total,
                               &len) != 1)
    {
        cleanse(&plaintext);
        return fail(QStringLiteral("gcm_auth_failed"));
    }
    total += len;
    plaintext.resize(total);

    if (plaintextOut) *plaintextOut = plaintext;
    return true;
}

void cleanse(QByteArray *buf)
{
    if (buf && !buf->isEmpty())
        OPENSSL_cleanse(buf->data(), static_cast<size_t>(buf->size()));
}

} // namespace SyncCrypto
} // namespace RagoShell
