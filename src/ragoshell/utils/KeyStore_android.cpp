#include "ragoshell/utils/KeyStore.h"

#ifdef Q_OS_ANDROID

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJniEnvironment>
#include <QJniObject>
#include <QRegularExpression>
#include <QSaveFile>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/utils/AppPaths.h"

namespace RagoShell
{
    namespace
    {
        // The AES key under this alias decrypts the keystore/*.bin files; changing the alias
        // orphans them.
        QByteArray masterAlias() { return QByteArray(BrandInfo::productId).toLower() + "_master_key"; }
        constexpr int kGcmIvBytes = 12;
        constexpr int kGcmTagBits = 128;

        bool javaFailed(QString *error, const char *what)
        {
            QJniEnvironment env;
            if (!env->ExceptionCheck())
                return false;
            env->ExceptionClear();
            if (error)
                *error = QStringLiteral("Android Keystore: %1 failed").arg(QLatin1String(what));
            return true;
        }

        QJniObject javaBytes(const QByteArray &bytes)
        {
            QJniEnvironment env;
            jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
            env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()),
                                    reinterpret_cast<const jbyte *>(bytes.constData()));
            return QJniObject::fromLocalRef(array);
        }

        QByteArray bytesFromJava(const QJniObject &object)
        {
            QJniEnvironment env;
            auto array = static_cast<jbyteArray>(object.object());
            if (!array)
                return {};
            QByteArray bytes(static_cast<qsizetype>(env->GetArrayLength(array)), Qt::Uninitialized);
            env->GetByteArrayRegion(array, 0, env->GetArrayLength(array), reinterpret_cast<jbyte *>(bytes.data()));
            return bytes;
        }

        QJniObject javaStringArray(const char *value)
        {
            QJniEnvironment env;
            jclass stringClass = env->FindClass("java/lang/String");
            jobjectArray array = env->NewObjectArray(1, stringClass, env->NewStringUTF(value));
            return QJniObject::fromLocalRef(array);
        }

        /// The AES key that lives inside the Android Keystore (hardware backed where the device has it);
        /// it never leaves the Keystore, the secrets are only encrypted with it.
        QJniObject masterKey(bool create, QString *error)
        {
            const QJniObject type = QJniObject::fromString(QStringLiteral("AndroidKeyStore"));
            QJniObject store = QJniObject::callStaticObjectMethod(
                "java/security/KeyStore", "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;",
                type.object<jstring>());
            if (javaFailed(error, "KeyStore.getInstance") || !store.isValid())
                return {};
            store.callMethod<void>("load", "(Ljava/security/KeyStore$LoadStoreParameter;)V", nullptr);
            if (javaFailed(error, "KeyStore.load"))
                return {};

            const QJniObject alias = QJniObject::fromString(QString::fromLatin1(masterAlias()));
            const bool present = store.callMethod<jboolean>("containsAlias", "(Ljava/lang/String;)Z",
                                                            alias.object<jstring>());
            if (javaFailed(error, "containsAlias"))
                return {};
            if (!present) {
                if (!create)
                    return {};
                const QJniObject algorithm = QJniObject::fromString(QStringLiteral("AES"));
                QJniObject generator = QJniObject::callStaticObjectMethod(
                    "javax/crypto/KeyGenerator", "getInstance",
                    "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/KeyGenerator;",
                    algorithm.object<jstring>(), type.object<jstring>());
                QJniObject builder("android/security/keystore/KeyGenParameterSpec$Builder",
                                   "(Ljava/lang/String;I)V", alias.object<jstring>(), jint(3));   // encrypt | decrypt
                const char *builderClass = "Landroid/security/keystore/KeyGenParameterSpec$Builder;";
                builder.callObjectMethod("setBlockModes",
                    QByteArray("([Ljava/lang/String;)").append(builderClass).constData(),
                    javaStringArray("GCM").object());
                builder.callObjectMethod("setEncryptionPaddings",
                    QByteArray("([Ljava/lang/String;)").append(builderClass).constData(),
                    javaStringArray("NoPadding").object());
                builder.callObjectMethod("setKeySize", QByteArray("(I)").append(builderClass).constData(), jint(256));
                const QJniObject spec = builder.callObjectMethod(
                    "build", "()Landroid/security/keystore/KeyGenParameterSpec;");
                generator.callMethod<void>("init", "(Ljava/security/spec/AlgorithmParameterSpec;)V", spec.object());
                generator.callObjectMethod("generateKey", "()Ljavax/crypto/SecretKey;");
                if (javaFailed(error, "generating the master key"))
                    return {};
            }
            QJniObject key = store.callObjectMethod("getKey", "(Ljava/lang/String;[C)Ljava/security/Key;",
                                                    alias.object<jstring>(), nullptr);
            if (javaFailed(error, "KeyStore.getKey"))
                return {};
            return key;
        }

        QJniObject newCipher()
        {
            const QJniObject transformation = QJniObject::fromString(QStringLiteral("AES/GCM/NoPadding"));
            return QJniObject::callStaticObjectMethod("javax/crypto/Cipher", "getInstance",
                                                      "(Ljava/lang/String;)Ljavax/crypto/Cipher;",
                                                      transformation.object<jstring>());
        }

        QString filePathFor(const QString &account)
        {
            QString safe = account;
            safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
            return appDataDir() + QStringLiteral("keystore/") + safe + QStringLiteral(".bin");
        }

        /// Secrets are encrypted with the Keystore's master key (AES-256-GCM) and the result (IV + cipher
        /// text) is kept in a file in the app's private folder.
        class AndroidKeyStore final : public KeyStore
        {
        public:
            QString name() const override { return QStringLiteral("Android Keystore"); }

            bool isAvailable() override
            {
                if (_probed)
                    return _available;
                _probed = true;
                QString error;
                _available = masterKey(/*create=*/true, &error).isValid();
                return _available;
            }

            Result read(const QString &account, QString *value, QString *error) override
            {
                QFile file(filePathFor(account));
                if (!file.exists())
                    return Result::NotFound;
                if (!file.open(QIODevice::ReadOnly)) {
                    if (error) *error = file.errorString();
                    return Result::Error;
                }
                const QByteArray blob = file.readAll();
                if (blob.size() <= kGcmIvBytes) {
                    if (error) *error = QStringLiteral("the stored secret is damaged");
                    return Result::Error;
                }
                const QJniObject key = masterKey(/*create=*/false, error);
                if (!key.isValid()) {
                    if (error && error->isEmpty()) *error = QStringLiteral("the master key is gone");
                    return Result::Error;
                }
                QJniObject cipher = newCipher();
                const QJniObject params("javax/crypto/spec/GCMParameterSpec", "(I[B)V", jint(kGcmTagBits),
                                        javaBytes(blob.left(kGcmIvBytes)).object());
                cipher.callMethod<void>("init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V",
                                        jint(2), key.object(), params.object());   // decrypt
                const QJniObject plain = cipher.callObjectMethod("doFinal", "([B)[B",
                                                                 javaBytes(blob.mid(kGcmIvBytes)).object());
                if (javaFailed(error, "decrypting")) {
                    return Result::Error;
                }
                if (value)
                    *value = QString::fromUtf8(bytesFromJava(plain));
                return Result::Ok;
            }

            bool write(const QString &account, const QString &value, QString *error) override
            {
                const QJniObject key = masterKey(/*create=*/true, error);
                if (!key.isValid())
                    return false;
                QJniObject cipher = newCipher();
                cipher.callMethod<void>("init", "(ILjava/security/Key;)V", jint(1), key.object());   // encrypt
                const QByteArray iv = bytesFromJava(cipher.callObjectMethod("getIV", "()[B"));
                const QByteArray encrypted = bytesFromJava(
                    cipher.callObjectMethod("doFinal", "([B)[B", javaBytes(value.toUtf8()).object()));
                if (javaFailed(error, "encrypting") || iv.size() != kGcmIvBytes || encrypted.isEmpty())
                    return false;

                const QString path = filePathFor(account);
                QDir().mkpath(QFileInfo(path).absolutePath());
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly)) {
                    if (error) *error = file.errorString();
                    return false;
                }
                file.write(iv + encrypted);
                if (!file.commit()) {
                    if (error) *error = file.errorString();
                    return false;
                }
                return true;
            }

            bool remove(const QString &account, QString *) override
            {
                QFile::remove(filePathFor(account));
                return true;
            }

        private:
            bool _probed = false;
            bool _available = false;
        };
    }

    std::unique_ptr<KeyStore> createAndroidKeyStore()
    {
        return std::make_unique<AndroidKeyStore>();
    }
}

#endif
