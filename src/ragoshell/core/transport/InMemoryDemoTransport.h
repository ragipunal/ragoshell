#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

#include "ragoshell/core/transport/IMongoTransport.h"

namespace RagoShell
{
    /**
     * @brief Canned in-memory transport for the Mod A live playground
     * (docs/WasmBuildReport.md §3–4, roadmap 8.1).
     *
     * Serves the synthetic dataset "catalog" (products, customers, orders)
     * without any socket or driver. Supports the demo-shaped subset of
     * commands: find (equality match + limit), aggregate (a $match-only
     * pipeline), count/listCollections/ping/killOp stubs. Anything else
     * returns an EJSON error so the UI path is identical to a real server.
     */
    class InMemoryDemoTransport final : public IMongoTransport
    {
    public:
        InMemoryDemoTransport();
        ~InMemoryDemoTransport() override = default;

        Result connect(const QString &ejsonSettings) override;
        Result runCommand(const QString &database, const QString &ejsonCommand) override;
        Result listDatabases() override;
        Result listCollections(const QString &database) override;
        Result ping() override;
        Result killOp(const QString &ejsonFilter) override;
        void disconnect() override;
        bool isLive() const override { return false; }

        /// The built-in dataset (for tests and the playground page).
        static QJsonObject dataset();

    private:
        Result runFind(const QString &coll, const QJsonObject &cmd);
        Result runAggregate(const QString &coll, const QJsonObject &cmd);
        bool matches(const QJsonObject &doc, const QJsonObject &filter) const;
        static QString toEjson(const QJsonValue &v);

        bool _connected = false;
        QHash<QString, QJsonArray> _collections;   ///< name -> documents
    };
}
