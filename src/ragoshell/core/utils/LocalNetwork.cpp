#include "ragoshell/core/utils/LocalNetwork.h"

#include <QHostAddress>

namespace RagoShell
{
    namespace
    {
        QString stripPort(const QString &input)
        {
            QString host = input.trimmed();
            if (host.startsWith(QLatin1Char('['))) {
                const int close = host.indexOf(QLatin1Char(']'));
                return close > 0 ? host.mid(1, close - 1) : host.mid(1);
            }
            // A single colon separates a port; more than one means a bare IPv6 address.
            if (host.count(QLatin1Char(':')) == 1)
                host.truncate(host.indexOf(QLatin1Char(':')));
            return host;
        }
    }

    bool isLocalNetworkHost(const QString &hostWithOptionalPort)
    {
        const QString host = stripPort(hostWithOptionalPort);
        if (host.isEmpty())
            return false;

        QHostAddress address;
        if (address.setAddress(host)) {
            if (address.isLoopback())
                return false;
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                const quint32 v4 = address.toIPv4Address();
                const quint8 a = v4 >> 24;
                const quint8 b = (v4 >> 16) & 0xff;
                return a == 10
                    || (a == 172 && b >= 16 && b <= 31)
                    || (a == 192 && b == 168)
                    || (a == 169 && b == 254)
                    || (a == 100 && b >= 64 && b <= 127);
            }
            return address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7)
                || address.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10);
        }

        const QString name = host.toLower();
        if (name == QLatin1String("localhost"))
            return false;
        return name.endsWith(QLatin1String(".local")) || !name.contains(QLatin1Char('.'));
    }

    bool anyLocalNetworkHost(const QStringList &hosts)
    {
        for (const QString &host : hosts) {
            if (isLocalNetworkHost(host))
                return true;
        }
        return false;
    }
}
