#include "ragoshell/utils/common.h"

#include <QFileInfo>
#include <QSettings>

#include <QSize>
#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/utils/StringOperations.h"

namespace RagoShell
{
    void genericEventErrorHandler(Event *event, const std::string &userFriendlyMessage, EventBus* bus, 
                                  QObject* sender)
    {
        if (!event->isError())
            return;

        std::string errMsg = captilizeFirstChar(event->error().errorMessage());
        LOG_MSG(userFriendlyMessage + " " + errMsg, RagoShell::LogSeverity::Error);

        if (bus && sender)
            bus->publish(new OperationFailedEvent(sender, errMsg, userFriendlyMessage));
        else
            LOG_MSG("Failed to publish OperationFailedEvent.", RagoShell::LogSeverity::Error);
    }

    bool fileExists(const QString &filePath)
    {
        QFileInfo fileInfo(filePath);
        return fileInfo.exists() && fileInfo.isFile();
    }

    template<typename T>
    bool vectorContains(std::vector<T> const& vec, T const& value) {
        return find(vec.cbegin(), vec.cend(), value) != vec.cend();
    }

    QVariant getSetting(QString const& key) {
        const QString brand = QString::fromUtf8(BrandInfo::productName);
        return QSettings(brand, brand).value(key).toSize();
    }

    void saveSetting(QString const& key, QVariant const& value) {
        const QString brand = QString::fromUtf8(BrandInfo::productName);
        QSettings(brand, brand).setValue(key, value);
    }

}   // end of name space RagoShell
