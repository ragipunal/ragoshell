#pragma once

#include "SimpleCrypt.h"

#include "ragoshell/core/Enums.h"
#include <utility>
#include <vector>

#include <string>

#include <QString>


namespace RagoShell {
    
    class RagoCrypt {    
        using LogAndSeverity = std::pair<std::string, RagoShell::LogSeverity>;
    public: 
        /// Where the key that encrypts saved passwords lives.
        struct KeyStorage {
            bool inKeyStore = false;      ///< The OS key store holds the key.
            bool storeAvailable = false;  ///< A key store exists on this machine.
            QString storeName;            ///< "Windows Credential Manager", "Keychain", "Secret Service"
        };

        static SimpleCrypt& simpleCrypter() {
            static SimpleCrypt simpleCrypt(_KEY);
            return simpleCrypt;
        }

        static std::string encrypt(std::string passwd) {
            return simpleCrypter().encryptToString(QString::fromStdString(passwd)).toStdString();
        }

        static std::string decrypt(std::string cryptedPasswd) {
            return simpleCrypter().decryptToString(QString::fromStdString(cryptedPasswd)).toStdString();
        }

        // Finds the key: in the OS key store (moving an old key file into it),
        // else in the key file, else makes a new key (see resolveEncryptionKey()).
        static void initKey();

        static std::vector<LogAndSeverity> const& ragoCryptLogs() {
            return _ragoCryptLogs;
        }

        /// Where the key came from in initKey().
        static KeyStorage const& keyStorage() {
            return _keyStorage;
        }

    private:
        static long long _KEY;
        static std::vector<LogAndSeverity> _ragoCryptLogs;
        static KeyStorage _keyStorage;
    };

}
