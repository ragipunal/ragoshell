#pragma once

#include <QString>
#include <QStringList>

namespace RagoShell
{
    /**
     * True when the host name or address points into the local network: private
     * IPv4 ranges (10/8, 172.16/12, 192.168/16), link-local, carrier-grade NAT
     * (100.64/10), IPv6 unique-local and link-local addresses, ".local" names and
     * single-label names. Loopback is not included: Android's local network
     * protection (API 37) does not cover it.
     *
     * Accepts "host", "host:port" and "[v6]:port".
     */
    bool isLocalNetworkHost(const QString &hostWithOptionalPort);

    /// True when any of the hosts is a local network host.
    bool anyLocalNetworkHost(const QStringList &hosts);
}
