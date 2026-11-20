#include "ragoshell/utils/KeyStore.h"

#ifdef Q_OS_WASM

#include <QHash>

#include "ragoshell/core/utils/AppPaths.h"

namespace RagoShell
{
    namespace
    {
        // Default store: memory only (docs/WebStorage.md §2). Nothing touches
        // IDBFS, so saved credentials vanish on reload — honest behaviour for
        // the playground; RAGOSHELL_KEYSTORE_PASS upgrades to the encrypted
        // file store when persistence is wanted.
        class SessionKeyStore : public KeyStore
        {
        public:
            QString name() const override { return QStringLiteral("Browser Session"); }
            bool isAvailable() override { return true; }

            Result read(const QString &account, QString *value, QString *error = nullptr) override
            {
                Q_UNUSED(error);
                const auto it = _entries.constFind(account);
                if (it == _entries.constEnd())
                    return Result::NotFound;
                *value = it.value();
                return Result::Ok;
            }

            bool write(const QString &account, const QString &value, QString *error = nullptr) override
            {
                Q_UNUSED(error);
                if (account.isEmpty())
                    return false;
                _entries.insert(account, value);
                return true;
            }

            bool remove(const QString &account, QString *error = nullptr) override
            {
                Q_UNUSED(error);
                return _entries.remove(account) > 0;
            }

        private:
            QHash<QString, QString> _entries;
        };
    }

    std::unique_ptr<KeyStore> createWasmKeyStore(const QString &service)
    {
        // Emscripten env vars are injectable through Module['ENV'] / qtloader
        // config; presence of the passphrase switches to the encrypted file.
        const QString pass = productEnv("KEYSTORE_PASS");
        if (!pass.isEmpty()) {
            auto store = createPassphraseFileKeyStore(
                appDataDir() + QStringLiteral("keystore.json"), pass);
            if (store)
                return store;
        }
        Q_UNUSED(service);
        return std::make_unique<SessionKeyStore>();
    }
}
#endif
