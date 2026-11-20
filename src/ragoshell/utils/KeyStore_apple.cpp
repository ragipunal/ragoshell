#include "ragoshell/utils/KeyStore.h"

#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <QtGlobal>

#include "ragoshell/core/utils/AppPaths.h"

namespace RagoShell
{
    namespace
    {
        // Owns a CoreFoundation object.
        template <typename T>
        class Cf
        {
        public:
            explicit Cf(T ref = nullptr) : _ref(ref) {}
            Cf(Cf &&other) noexcept : _ref(other._ref) { other._ref = nullptr; }
            Cf(const Cf &) = delete;
            Cf &operator=(const Cf &) = delete;
            ~Cf() { if (_ref) CFRelease(_ref); }
            T get() const { return _ref; }
            T *out() { return &_ref; }
        private:
            T _ref;
        };

        Cf<CFStringRef> cfString(const QString &s)
        {
            const QByteArray utf8 = s.toUtf8();
            return Cf<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault,
                                                           reinterpret_cast<const UInt8 *>(utf8.constData()),
                                                           utf8.size(), kCFStringEncodingUTF8, false));
        }

        // Query for the generic password item of an account.
        Cf<CFMutableDictionaryRef> keychainQuery(const QString &serviceName, const QString &account)
        {
            Cf<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            Cf<CFStringRef> service = cfString(serviceName);
            Cf<CFStringRef> acct = cfString(account);
            CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
            CFDictionarySetValue(query.get(), kSecAttrService, service.get());
            CFDictionarySetValue(query.get(), kSecAttrAccount, acct.get());
            return query;
        }

        QString osStatus(OSStatus status)
        {
            Cf<CFStringRef> text(SecCopyErrorMessageString(status, nullptr));
            if (text.get()) {
                char buffer[256];
                if (CFStringGetCString(text.get(), buffer, sizeof(buffer), kCFStringEncodingUTF8))
                    return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(buffer)).arg(status);
            }
            return QStringLiteral("Keychain error %1").arg(status);
        }

        /// The login keychain (macOS) or the app's keychain (iOS), generic password items of one service.
        class AppleKeyStore final : public KeyStore
        {
        public:
            explicit AppleKeyStore(const QString &service) : _service(service) {}

            QString name() const override { return QStringLiteral("Keychain"); }
            bool isAvailable() override
            {
#if defined(Q_OS_MACOS)
                if (productEnv("KEYSTORE").compare(QLatin1String("keychain"), Qt::CaseInsensitive) == 0)
                    return true;

                // Ad-hoc / linker-signed macOS binaries have no certificate or Team ID, so legacy
                // login.keychain ACLs bind to the exact SHA-256 CDHash of the executable — triggering
                // repeated modal login-keychain password prompts on every rebuild or ad-hoc update.
                // Only use macOS Keychain automatically when the process carries a real certificate
                // signature (Apple Development, Developer ID, or Mac App Store).
                static const bool signedWithCertificate = [] {
                    Cf<SecCodeRef> selfCode;
                    if (SecCodeCopySelf(kSecCSDefaultFlags, selfCode.out()) != errSecSuccess || !selfCode.get())
                        return false;
                    Cf<SecStaticCodeRef> staticCode;
                    if (SecCodeCopyStaticCode(selfCode.get(), kSecCSDefaultFlags, staticCode.out()) != errSecSuccess
                        || !staticCode.get())
                        return false;
                    Cf<CFDictionaryRef> info;
                    if (SecCodeCopySigningInformation(staticCode.get(), kSecCSSigningInformation, info.out()) != errSecSuccess
                        || !info.get())
                        return false;

                    SInt32 flags = 0;
                    if (const auto cfFlags = static_cast<CFNumberRef>(CFDictionaryGetValue(info.get(), kSecCodeInfoFlags)))
                        CFNumberGetValue(cfFlags, kCFNumberSInt32Type, &flags);
                    if ((flags & kSecCodeSignatureAdhoc) != 0)
                        return false;

                    const auto certs = static_cast<CFArrayRef>(CFDictionaryGetValue(info.get(), kSecCodeInfoCertificates));
                    return certs != nullptr && CFArrayGetCount(certs) > 0;
                }();
                return signedWithCertificate;
#else
                return true;
#endif
            }

            Result read(const QString &account, QString *value, QString *error) override
            {
                Cf<CFMutableDictionaryRef> query = itemQuery(account);
                CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
                CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
                Cf<CFTypeRef> data;
                const OSStatus status = SecItemCopyMatching(query.get(), data.out());
                if (status == errSecItemNotFound)
                    return Result::NotFound;
                if (status != errSecSuccess) {
                    if (error)
                        *error = osStatus(status);
                    return Result::Error;
                }
                if (value) {
                    const CFDataRef bytes = static_cast<CFDataRef>(data.get());
                    *value = QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(bytes)),
                                               static_cast<qsizetype>(CFDataGetLength(bytes)));
                }
                return Result::Ok;
            }

            bool write(const QString &account, const QString &value, QString *error) override
            {
                const QByteArray utf8 = value.toUtf8();
                Cf<CFDataRef> data(CFDataCreate(kCFAllocatorDefault,
                                                reinterpret_cast<const UInt8 *>(utf8.constData()), utf8.size()));

                // Replace the value of an existing item, else add one.
                Cf<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(
                    kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
                CFDictionarySetValue(update.get(), kSecValueData, data.get());
                OSStatus status = SecItemUpdate(itemQuery(account).get(), update.get());
                if (status == errSecItemNotFound) {
                    Cf<CFMutableDictionaryRef> add = itemQuery(account);
                    CFDictionarySetValue(add.get(), kSecValueData, data.get());
                    status = SecItemAdd(add.get(), nullptr);
                }
                if (status != errSecSuccess) {
                    if (error)
                        *error = osStatus(status);
                    return false;
                }
                return true;
            }

            bool remove(const QString &account, QString *error) override
            {
                const OSStatus status = SecItemDelete(itemQuery(account).get());
                if (status != errSecSuccess && status != errSecItemNotFound) {
                    if (error)
                        *error = osStatus(status);
                    return false;
                }
                return true;
            }

        private:
            Cf<CFMutableDictionaryRef> itemQuery(const QString &account) const
            {
                return keychainQuery(_service, account);
            }

            QString _service;
        };
    }

    std::unique_ptr<KeyStore> createAppleKeyStore(const QString &service)
    {
        return std::make_unique<AppleKeyStore>(service);
    }
}

#endif
