#pragma once

#include "ragoshell-app/ServerTreeModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <cmath>
#include <functional>

#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/domain/MongoDatabase.h"
#include "ragoshell/core/domain/MongoCollection.h"
#include "ragoshell/core/mongodb/ReplicaSet.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaCollector.h"

namespace RagoShell::ServerTreeDetail
{
        inline QString formatShortMongoVersion(MongoServer *server, const QString &exactVer)
        {
            if (!exactVer.trimmed().isEmpty()) {
                const QStringList parts = exactVer.trimmed().split(QLatin1Char('.'));
                if (parts.size() >= 2)
                    return QStringLiteral("v%1.%2").arg(parts[0], parts[1]);
                return QStringLiteral("v%1").arg(exactVer.trimmed());
            }
            if (server && server->version() > 0.0f) {
                return QStringLiteral("v%1").arg(
                    QString::number(static_cast<double>(server->version()), 'f', 1));
            }
            return QString();
        }

        inline void applyServerBadgeAndToolTip(ServerTreeModel::Node *s)
        {
            if (!s || !s->server || !s->server->connectionRecord())
                return;

            MongoServer *server = s->server;
            ConnectionSettings *conn = server->connectionRecord();
            s->isReplicaSet = conn->isReplicaSet();
            const bool isMongos = (server->getStorageEngineType() == "mongos");

            const QString shortVer = formatShortMongoVersion(server, s->serverVersionStr);
            const QString fullVer = !s->serverVersionStr.trimmed().isEmpty()
                ? s->serverVersionStr.trimmed()
                : (server->version() > 0.0f
                       ? QString::number(std::round(static_cast<double>(server->version()) * 10.0) / 10.0, 'f', 1)
                       : QCoreApplication::translate("ServerTreeModel", "Unknown"));
            const QString engine = server->getStorageEngineType().empty()
                ? QStringLiteral("wiredTiger")
                : QtUtils::toQString(server->getStorageEngineType());

            if (!s->online) {
                s->badgeText = QStringLiteral("OFFLINE");
                s->badgeKind = QStringLiteral("danger");
            } else if (isMongos) {
                s->badgeText = shortVer.isEmpty()
                    ? QStringLiteral("MONGOS")
                    : QStringLiteral("MONGOS · %1").arg(shortVer);
                s->badgeKind = QStringLiteral("accent");
            } else if (s->isReplicaSet) {
                s->badgeText = shortVer.isEmpty()
                    ? QStringLiteral("RS")
                    : QStringLiteral("RS · %1").arg(shortVer);
                s->badgeKind = QStringLiteral("accent");
            } else if (conn->sslSettings() && conn->sslSettings()->sslEnabled()) {
                s->badgeText = shortVer.isEmpty()
                    ? QStringLiteral("TLS")
                    : QStringLiteral("TLS · %1").arg(shortVer);
                s->badgeKind = QStringLiteral("info");
            } else if (conn->sshSettings() && conn->sshSettings()->enabled()) {
                s->badgeText = shortVer.isEmpty()
                    ? QStringLiteral("SSH")
                    : QStringLiteral("SSH · %1").arg(shortVer);
                s->badgeKind = QStringLiteral("info");
            } else if (!shortVer.isEmpty()) {
                s->badgeText = shortVer;
                s->badgeKind = QStringLiteral("info");
            } else {
                s->badgeText.clear();
                s->badgeKind.clear();
            }

            QString rsName;
            if (s->isReplicaSet && server->replicaSetInfo() && !server->replicaSetInfo()->setName.empty())
                rsName = QtUtils::toQString(server->replicaSetInfo()->setName);

            QStringList tip;
            tip << QCoreApplication::translate("ServerTreeModel", "Server:   %1").arg(QtUtils::toQString(conn->getReadableName()));
            tip << QCoreApplication::translate("ServerTreeModel", "MongoDB:  v%1").arg(fullVer);
            tip << QCoreApplication::translate("ServerTreeModel", "Engine:   %1").arg(engine);
            tip << QCoreApplication::translate("ServerTreeModel", "Topology: %1").arg(
                isMongos ? QCoreApplication::translate("ServerTreeModel", "Sharded Cluster (mongos)")
                         : (s->isReplicaSet
                                ? (rsName.isEmpty()
                                       ? QCoreApplication::translate("ServerTreeModel", "Replica Set")
                                       : QCoreApplication::translate("ServerTreeModel", "Replica Set (%1)").arg(rsName))
                                : QCoreApplication::translate("ServerTreeModel", "Standalone / Direct")));
            s->toolTip = tip.join(QLatin1Char('\n'));
        }
}
