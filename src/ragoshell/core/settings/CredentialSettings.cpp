#include "ragoshell/core/settings/CredentialSettings.h"

#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/utils/RagoCrypt.h"

namespace RagoShell
{
    CredentialSettings::CredentialSettings() :
        _userName(),
        _userPassword(),
        _databaseName(),
        _mechanism(),
        _enabled(false),
        _useManuallyVisibleDbs(false),
        _manuallyVisibleDbs()
    {

    }
    CredentialSettings::CredentialSettings(const QVariantMap &map) :
        _userName(QtUtils::toStdString(map.value("userName").toString())),
        _databaseName(QtUtils::toStdString(map.value("databaseName").toString())),
        _mechanism(QtUtils::toStdString(map.value("mechanism").toString())),
        _useManuallyVisibleDbs(map.value("useManuallyVisibleDbs").toBool()),
        _manuallyVisibleDbs(QtUtils::toStdString(map.value("manuallyVisibleDbs").toString())),
        _enabled(map.value("enabled").toBool())
    {
        // From version Rago 1.3 "userPasswordEncrypted" is used instead of "userPassword" in config. file
        if(map.contains("userPassword")) // Rago 1.2 and below
            _userPassword = map.value("userPassword").toString().toStdString();
        else if(map.contains("userPasswordEncrypted")) // From Rago 1.3
            _userPassword = RagoCrypt::decrypt(map.value("userPasswordEncrypted").toString().toStdString());
    }

    /**
     * @brief Clones credential settings.
     */
    CredentialSettings *CredentialSettings::clone() const
    {
        CredentialSettings *cloned = new CredentialSettings(*this);
        return cloned;
    }

    QVariant CredentialSettings::toVariant() const
    {
        QVariantMap map;
        map.insert("userName", QtUtils::toQString(userName()));
        map.insert("userPasswordEncrypted", userPassword().empty() ? "" :
                                            QtUtils::toQString(RagoCrypt::encrypt(userPassword())));
        map.insert("databaseName", QtUtils::toQString(databaseName()));
        map.insert("mechanism", QtUtils::toQString(mechanism()));
        map.insert("useManuallyVisibleDbs", _useManuallyVisibleDbs);
        map.insert("manuallyVisibleDbs", QtUtils::toQString(_manuallyVisibleDbs));
        map.insert("enabled", enabled());
        return map;
    }
}