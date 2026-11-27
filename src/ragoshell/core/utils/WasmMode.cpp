#include "ragoshell/core/utils/WasmMode.h"

#include <QUrl>

#include "ragoshell/core/utils/UpdatePulse.h"

namespace RagoShell::WasmMode
{
    WasmRuntimeMode resolveRuntimeMode(bool isWasmBuild, QStringView rawEnvValue)
    {
        if (!isWasmBuild)
            return WasmRuntimeMode::DesktopOrMobile;

        const QStringView trimmed = rawEnvValue.trimmed();
        if (trimmed.compare(QLatin1String("gateway"), Qt::CaseInsensitive) == 0)
            return WasmRuntimeMode::Gateway;

        return WasmRuntimeMode::Playground;
    }

    QString resolveDownloadPageUrl(QStringView rawProductUrl)
    {
        QString base = rawProductUrl.trimmed().toString();
        while (base.endsWith(QLatin1Char('/')))
            base.chop(1);
        if (base.isEmpty())
            return QString();

        const QUrl parsedBase(base, QUrl::StrictMode);
        if (!parsedBase.isValid()
            || parsedBase.hasQuery()
            || parsedBase.hasFragment()
            || !parsedBase.userInfo().isEmpty()
            || parsedBase.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))) {
            return QString();
        }

        const QString candidate = base + QStringLiteral("/download/");
        return UpdatePulse::isSafeDownloadUrl(candidate) ? candidate : QString();
    }

    QString defaultPlaygroundConnectionName(QStringView productName)
    {
        const QString trimmed = productName.trimmed().toString();
        if (trimmed.isEmpty())
            return QStringLiteral("Playground");
        return trimmed + QStringLiteral(" Playground");
    }

    QString playgroundStarterScript()
    {
        return QStringLiteral(
            "// Interactive MongoDB Playground — edit and press F5:\n"
            "db.getCollection('products').find({ active: true }).sort({ price: -1 })\n"
            "\n"
            "// Select an example below and press F5 to run the selection:\n"
            "// db.getCollection('orders').aggregate([\n"
            "//     { $group: { _id: '$customerId', orders: { $sum: 1 }, revenue: { $sum: '$total' } } },\n"
            "//     { $sort: { revenue: -1 } }\n"
            "// ])\n"
            "//\n"
            "// db.getCollection('products').insertOne({\n"
            "//     sku: 'DK-04', name: 'USB-C Dock', category: 'peripherals',\n"
            "//     price: 119.0, stock: 25, active: true, tags: ['usb', 'dock']\n"
            "// })\n");
    }
}
