#include "ragoshell/core/settings/SshSettings.h"

#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/utils/RagoCrypt.h"

namespace RagoShell
{
    namespace
    {
        void wipeInPlace(std::string &secret)
        {
            if (secret.empty())
                return;
            volatile char *ptr = &secret[0];
            for (size_t i = 0; i < secret.size(); ++i)
                ptr[i] = '\0';
            secret.clear();
        }
    }

    SshSettings::SshSettings() :
        _port(22),
        _authMethod("publickey"),
        _enabled(false),
        _askPassword(false),
        _logLevel(1) {

    }

    SshSettings::~SshSettings() {
        wipeSecrets();
    }

    void SshSettings::wipeSecrets() {
        wipeInPlace(_userPassword);
        wipeInPlace(_passphrase);
        wipeInPlace(_askedPassword);
    }

    SshSettings *SshSettings::clone() const {
        SshSettings *cloned = new SshSettings(*this);
        return cloned;
    }

    QVariant SshSettings::toVariant() const {
        QVariantMap map;
        map.insert("host", QtUtils::toQString(host()));
        map.insert("port", port());
        map.insert("userName", QtUtils::toQString(userName()));
        map.insert("userPasswordEncrypted", userPassword().empty() ? "" : 
                                            QtUtils::toQString(RagoCrypt::encrypt(userPassword())));
        map.insert("privateKeyFile", QtUtils::toQString(privateKeyFile()));
        map.insert("publicKeyFile", QtUtils::toQString(publicKeyFile()));
        map.insert("passphraseEncrypted", passphrase().empty() ? "" : 
                                          QtUtils::toQString(RagoCrypt::encrypt(passphrase())));
        map.insert("method", QtUtils::toQString(authMethod()));
        map.insert("enabled", enabled());
        map.insert("askPassword", askPassword());
        return map;
    }

    void SshSettings::fromVariant(const QVariantMap &map) {
        setHost(QtUtils::toStdString(map.value("host").toString()));
        setPort(map.value("port").toInt());
        setUserName(QtUtils::toStdString(map.value("userName").toString()));

        // From version Rago 1.3 "userPasswordEncrypted" is used instead of "userPassword" in config. file
        if (map.contains("userPassword")) // Rago 1.2 and below
            setUserPassword((map.value("userPassword").toString().toStdString()));
        else if (map.contains("userPasswordEncrypted")) // From Rago 1.3
            setUserPassword(RagoCrypt::decrypt((map.value("userPasswordEncrypted").toString().toStdString())));

        setPrivateKeyFile(QtUtils::toStdString(map.value("privateKeyFile").toString()));
        setPublicKeyFile(QtUtils::toStdString(map.value("publicKeyFile").toString()));

        // From version Rago 1.3 "passphraseEncrypted" is used instead of "passphrase" in config. file
        if (map.contains("passphrase")) // Rago 1.2 and below
            setPassphrase((map.value("passphrase").toString().toStdString()));
        else if (map.contains("passphraseEncrypted")) // From Rago 1.3
            setPassphrase(RagoCrypt::decrypt((map.value("passphraseEncrypted").toString().toStdString())));

        setAuthMethod(QtUtils::toStdString(map.value("method").toString()));
        setEnabled(map.value("enabled").toBool());
        setAskPassword(map.value("askPassword").toBool());
    }
}
