#include "ragoshell/utils/KeyStore.h"

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <string>

namespace RagoShell
{
    namespace
    {
        QString winError(DWORD code)
        {
            return QStringLiteral("Windows error %1").arg(code);
        }

        /// Generic credentials of the current user in Windows Credential Manager
        /// (Control Panel > Credential Manager > Windows Credentials), "<service>/<account>".
        class WindowsKeyStore final : public KeyStore
        {
        public:
            explicit WindowsKeyStore(const QString &service) : _service(service) {}

            QString name() const override { return QStringLiteral("Windows Credential Manager"); }
            bool isAvailable() override { return true; }

            Result read(const QString &account, QString *value, QString *error) override
            {
                const std::wstring target = targetName(account);
                PCREDENTIALW cred = nullptr;
                if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
                    const DWORD code = GetLastError();
                    if (code == ERROR_NOT_FOUND)
                        return Result::NotFound;
                    if (error)
                        *error = winError(code);
                    return Result::Error;
                }
                if (value)
                    *value = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                               static_cast<qsizetype>(cred->CredentialBlobSize));
                CredFree(cred);
                return Result::Ok;
            }

            bool write(const QString &account, const QString &value, QString *error) override
            {
                const std::wstring target = targetName(account);
                std::wstring user = _service.toStdWString();
                QByteArray blob = value.toUtf8();

                CREDENTIALW cred = {};
                cred.Type = CRED_TYPE_GENERIC;
                cred.TargetName = const_cast<LPWSTR>(target.c_str());
                cred.UserName = const_cast<LPWSTR>(user.c_str());
                cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
                cred.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
                cred.Persist = CRED_PERSIST_LOCAL_MACHINE;   // not roamed to other machines
                if (!CredWriteW(&cred, 0)) {
                    if (error)
                        *error = winError(GetLastError());
                    return false;
                }
                return true;
            }

            bool remove(const QString &account, QString *error) override
            {
                const std::wstring target = targetName(account);
                if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
                    const DWORD code = GetLastError();
                    if (code == ERROR_NOT_FOUND)
                        return true;
                    if (error)
                        *error = winError(code);
                    return false;
                }
                return true;
            }

        private:
            std::wstring targetName(const QString &account) const
            {
                return (_service + QLatin1Char('/') + account).toStdWString();
            }

            QString _service;
        };
    }

    std::unique_ptr<KeyStore> createWindowsKeyStore(const QString &service)
    {
        return std::make_unique<WindowsKeyStore>(service);
    }
}

#endif
