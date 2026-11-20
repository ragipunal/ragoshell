#include "RagoCrypt.h"

#include "ragoshell/core/utils/Logger.h"       
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/utils/KeyStore.h"

namespace RagoShell {

    long long RagoCrypt::_KEY = 0;
    std::vector<RagoCrypt::LogAndSeverity> RagoCrypt::_ragoCryptLogs;
    RagoCrypt::KeyStorage RagoCrypt::_keyStorage;

    void RagoCrypt::initKey() 
    {
        const QString dataDir = configDirectory();
        const bool overridden = appDataDirOverridden();
        const std::unique_ptr<KeyStore> store = createPlatformKeyStore();

        const EncryptionKeyResult result = resolveEncryptionKey(
            store.get(), encryptionKeyAccount(dataDir, overridden), dataDir + appFileName("key"));

        _KEY = result.key;
        _keyStorage.inKeyStore = result.inKeyStore;
        _keyStorage.storeAvailable = result.storeAvailable;
        _keyStorage.storeName = result.storeName;
        for (const auto &message : result.messages)
            _ragoCryptLogs.push_back({ message.text.toStdString(), message.severity });
    }

}
