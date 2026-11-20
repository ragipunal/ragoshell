#include "ragoshell/utils/KeyStore.h"

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)   // Android has its own (KeyStore_android.cpp)

#include <QLibrary>

namespace RagoShell
{
    namespace
    {
        // The parts of libsecret and GLib used here, declared by hand and loaded
        // at run time: nothing to install to build, and a machine without
        // libsecret simply has no key store.
        struct GError
        {
            unsigned int domain;
            int code;
            char *message;
        };

        struct SecretSchemaAttribute
        {
            const char *name;
            int type;                 // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
        };

        struct SecretSchema
        {
            const char *name;
            int flags;                // SECRET_SCHEMA_NONE = 0
            SecretSchemaAttribute attributes[32];
            int reserved;
            void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7;
        };

        using StoreFn = int (*)(const SecretSchema *, const char *collection, const char *label,
                                const char *password, void *cancellable, GError **error, ...);
        using LookupFn = char *(*)(const SecretSchema *, void *cancellable, GError **error, ...);
        using ClearFn = int (*)(const SecretSchema *, void *cancellable, GError **error, ...);
        using FreeFn = void (*)(char *);
        using ErrorFreeFn = void (*)(GError *);

        /// Secret Service (GNOME Keyring, KWallet's compatibility service, ...) through libsecret.
        /// Items carry the schema name and the attributes service=<service>, account=<account>.
        class LibsecretKeyStore final : public KeyStore
        {
        public:
            LibsecretKeyStore(const QString &service, const QByteArray &schemaName)
                : _service(service.toUtf8()), _schemaName(schemaName)
            {
                _schema = SecretSchema{ _schemaName.constData(), 0,
                                        { { "service", 0 }, { "account", 0 }, { nullptr, 0 } },
                                        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
            }

            QString name() const override { return QStringLiteral("Secret Service"); }

            bool isAvailable() override
            {
                if (_probed)
                    return _available;
                _probed = true;

                _secret.setFileNameAndVersion(QStringLiteral("secret-1"), 0);
                _glib.setFileNameAndVersion(QStringLiteral("glib-2.0"), 0);
                if (!_secret.load() || !_glib.load())
                    return false;
                _store = reinterpret_cast<StoreFn>(_secret.resolve("secret_password_store_sync"));
                _lookup = reinterpret_cast<LookupFn>(_secret.resolve("secret_password_lookup_sync"));
                _clear = reinterpret_cast<ClearFn>(_secret.resolve("secret_password_clear_sync"));
                _free = reinterpret_cast<FreeFn>(_secret.resolve("secret_password_free"));
                _errorFree = reinterpret_cast<ErrorFreeFn>(_glib.resolve("g_error_free"));
                if (!_store || !_lookup || !_clear || !_free || !_errorFree)
                    return false;

                // A lookup finds nothing without an error when a service answers; an error
                // means there is no service (no D-Bus session, no keyring daemon).
                GError *error = nullptr;
                char *found = _lookup(&_schema, nullptr, &error, "service", _service.constData(), "account",
                                      "availability-probe", nullptr);
                if (found)
                    _free(found);
                if (error) {
                    _errorFree(error);
                    return false;
                }
                _available = true;
                return true;
            }

            Result read(const QString &account, QString *value, QString *error) override
            {
                if (!isAvailable())
                    return Result::Error;
                GError *gerror = nullptr;
                char *secret = _lookup(&_schema, nullptr, &gerror, "service", _service.constData(), "account",
                                       account.toUtf8().constData(), nullptr);
                if (gerror) {
                    takeError(gerror, error);
                    return Result::Error;
                }
                if (!secret)
                    return Result::NotFound;
                if (value)
                    *value = QString::fromUtf8(secret);
                _free(secret);
                return Result::Ok;
            }

            bool write(const QString &account, const QString &value, QString *error) override
            {
                if (!isAvailable())
                    return false;
                GError *gerror = nullptr;
                const QByteArray label = _service + " encryption key";
                const int ok = _store(&_schema, "default", label.constData(),
                                      value.toUtf8().constData(), nullptr, &gerror,
                                      "service", _service.constData(), "account",
                                      account.toUtf8().constData(), nullptr);
                if (gerror) {
                    takeError(gerror, error);
                    return false;
                }
                return ok != 0;
            }

            bool remove(const QString &account, QString *error) override
            {
                if (!isAvailable())
                    return false;
                GError *gerror = nullptr;
                _clear(&_schema, nullptr, &gerror, "service", _service.constData(), "account",
                       account.toUtf8().constData(), nullptr);
                if (gerror) {
                    takeError(gerror, error);
                    return false;
                }
                return true;
            }

        private:
            void takeError(GError *gerror, QString *error)
            {
                if (error && gerror->message)
                    *error = QString::fromUtf8(gerror->message);
                _errorFree(gerror);
            }

            QByteArray _service;
            QByteArray _schemaName;
            SecretSchema _schema{};
            QLibrary _secret;
            QLibrary _glib;
            StoreFn _store = nullptr;
            LookupFn _lookup = nullptr;
            ClearFn _clear = nullptr;
            FreeFn _free = nullptr;
            ErrorFreeFn _errorFree = nullptr;
            bool _probed = false;
            bool _available = false;
        };
    }

    std::unique_ptr<KeyStore> createLibsecretKeyStore(const QString &service, const QByteArray &schemaName)
    {
        return std::make_unique<LibsecretKeyStore>(service, schemaName);
    }
}

#endif
