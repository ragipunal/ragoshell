#pragma once

#include <QString>
#include <QStringList>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/utils/LanguageCatalog.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell::QmlBackendDetail
{
    /// Names inside translated status text keep their own direction in
    /// right-to-left sentences (Unicode isolates).
    inline QString iso(const QString &name) { return LanguageCatalog::isolate(name); }

    /// The machines the app opens network connections to for this connection (the SSH host when
    /// a tunnel is used, otherwise the server or the replica set seeds).
    inline QStringList networkHosts(const ConnectionSettings &conn)
    {
        if (conn.sshSettings() && conn.sshSettings()->enabled())
            return {QtUtils::toQString(conn.sshSettings()->host())};
        QStringList hosts;
        if (conn.isReplicaSet() && conn.replicaSetSettings()) {
            for (const std::string &member : conn.replicaSetSettings()->members())
                hosts << QtUtils::toQString(member);
        } else {
            hosts << QtUtils::toQString(conn.serverHost());
        }
        return hosts;
    }
}
