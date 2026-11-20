#include "ragoshell/core/transport/InMemoryDemoTransport.h"

#include <QJsonDocument>

namespace RagoShell
{
    namespace
    {
        IMongoTransport::Result okResult(const QJsonValue &v)
        {
            IMongoTransport::Result r;
            r.ok = true;
            r.ejson = QString::fromUtf8(
                v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)
                            : QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
            return r;
        }

        // Canonical-EJSON wrappers arrive from bsoncxx::to_json; unwrap to
        // plain JSON scalars for comparison (docs/TransportSeam.md §5).
        QJsonValue unwrapEjson(const QJsonValue &v)
        {
            if (!v.isObject())
                return v;
            const QJsonObject o = v.toObject();
            if (o.size() != 1)
                return v;
            const QString k = o.begin().key();
            if (k == QLatin1String("$numberInt") || k == QLatin1String("$numberLong") ||
                k == QLatin1String("$numberDouble"))
                return o.begin().value().toString().toDouble();
            if (k == QLatin1String("$oid"))
                return o.begin().value().toString();
            return v;
        }

        IMongoTransport::Result errResult(const QString &msg, int code = 59)
        {
            IMongoTransport::Result r;
            r.error = msg;
            r.code = code;
            r.codeName = QStringLiteral("CommandNotFound");
            return r;
        }

        QJsonObject wrapCursor(const QJsonArray &docs)
        {
            return QJsonObject{{"cursor", QJsonObject{{"firstBatch", docs}, {"id", 0}, {"ns", "catalog"}}},
                               {"ok", 1.0}};
        }
    }

    QJsonObject InMemoryDemoTransport::dataset()
    {
        QJsonArray products{
            QJsonObject{{"_id", 1}, {"sku", "KB-01"}, {"name", "Mechanical Keyboard"},
                        {"price", 89.90}, {"stock", 34}, {"tags", QJsonArray{"input", "usb"}}},
            QJsonObject{{"_id", 2}, {"sku", "MS-07"}, {"name", "Wireless Mouse"},
                        {"price", 39.50}, {"stock", 120}, {"tags", QJsonArray{"input", "wireless"}}},
            QJsonObject{{"_id", 3}, {"sku", "MN-27"}, {"name", "27\" Monitor"},
                        {"price", 249.00}, {"stock", 12}, {"tags", QJsonArray{"display"}}},
        };
        QJsonArray customers{
            QJsonObject{{"_id", "c1"}, {"name", "Ada Lovelace"}, {"tier", "gold"}},
            QJsonObject{{"_id", "c2"}, {"name", "Grace Hopper"}, {"tier", "silver"}},
            QJsonObject{{"_id", "c3"}, {"name", "Edsger Dijkstra"}, {"tier", "gold"}},
        };
        QJsonArray orders{
            QJsonObject{{"_id", 101}, {"customerId", "c1"}, {"sku", "KB-01"}, {"qty", 1},
                        {"total", 89.90}},
            QJsonObject{{"_id", 102}, {"customerId", "c2"}, {"sku", "MS-07"}, {"qty", 2},
                        {"total", 79.00}},
            QJsonObject{{"_id", 103}, {"customerId", "c1"}, {"sku", "MN-27"}, {"qty", 1},
                        {"total", 249.00}},
        };
        return QJsonObject{{"products", products}, {"customers", customers}, {"orders", orders}};
    }

    InMemoryDemoTransport::InMemoryDemoTransport()
    {
        const QJsonObject ds = dataset();
        for (auto it = ds.begin(); it != ds.end(); ++it)
            _collections.insert(it.key(), it.value().toArray());
    }

    IMongoTransport::Result InMemoryDemoTransport::connect(const QString &)
    {
        _connected = true;
        return okResult(QJsonObject{{"ok", 1.0}});
    }

    IMongoTransport::Result InMemoryDemoTransport::runCommand(const QString &database,
                                                              const QString &ejsonCommand)
    {
        if (!_connected)
            return errResult(QStringLiteral("not connected"));
        if (database != QLatin1String("catalog") && database != QLatin1String("admin"))
            return errResult(QStringLiteral("demo dataset is 'catalog'"));
        const QJsonObject cmd = QJsonDocument::fromJson(ejsonCommand.toUtf8()).object();
        if (cmd.isEmpty())
            return errResult(QStringLiteral("unreadable command"), 9);

        if (cmd.contains(QStringLiteral("ping")))
            return ping();
        if (cmd.contains(QStringLiteral("listCollections")))
            return listCollections(QStringLiteral("catalog"));
        if (cmd.contains(QStringLiteral("listDatabases")))
            return listDatabases();
        if (cmd.contains(QStringLiteral("serverStatus")))
            return okResult(QJsonObject{{"host", "demo"}, {"version", "in-memory"},
                                        {"ok", 1.0}});
        if (cmd.contains(QStringLiteral("killOp")))
            return okResult(QJsonObject{{"info", "demo: nothing running"}, {"ok", 1.0}});

        const QString find = cmd.value(QStringLiteral("find")).toString();
        if (!find.isEmpty())
            return runFind(find, cmd);
        const QString agg = cmd.value(QStringLiteral("aggregate")).toString();
        if (!agg.isEmpty())
            return runAggregate(agg, cmd);
        const QString count = cmd.value(QStringLiteral("count")).toString();
        if (!count.isEmpty()) {
            const Result r = runFind(count, cmd);
            if (!r.ok)
                return r;
            const QJsonArray docs = QJsonDocument::fromJson(r.ejson.toUtf8())
                                        .object()
                                        .value("cursor").toObject()
                                        .value("firstBatch").toArray();
            return okResult(QJsonObject{{"n", docs.size()}, {"ok", 1.0}});
        }
        return errResult(QStringLiteral("unsupported command in demo transport"));
    }

    IMongoTransport::Result InMemoryDemoTransport::runFind(const QString &coll,
                                                         const QJsonObject &cmd)
    {
        const QJsonArray docs = _collections.value(coll);
        if (docs.isEmpty() && !_collections.contains(coll))
            return errResult(QStringLiteral("unknown collection %1").arg(coll), 26);
        const QJsonObject filter = cmd.value(QStringLiteral("filter")).toObject();
        const int limit = int(unwrapEjson(cmd.value(QStringLiteral("limit"))).toDouble(0));
        QJsonArray out;
        for (const QJsonValue &v : docs) {
            if (matches(v.toObject(), filter))
                out.append(v);
            if (limit > 0 && out.size() >= limit)
                break;
        }
        return okResult(wrapCursor(out));
    }

    IMongoTransport::Result InMemoryDemoTransport::runAggregate(const QString &coll,
                                                              const QJsonObject &cmd)
    {
        const QJsonArray docs = _collections.value(coll);
        if (docs.isEmpty() && !_collections.contains(coll))
            return errResult(QStringLiteral("unknown collection %1").arg(coll), 26);
        const QJsonArray pipeline = cmd.value(QStringLiteral("pipeline")).toArray();
        QJsonArray out = docs;
        for (const QJsonValue &stage : pipeline) {
            const QJsonObject s = stage.toObject();
            if (const QJsonObject m = s.value(QStringLiteral("$match")).toObject(); !m.isEmpty()) {
                QJsonArray filtered;
                for (const QJsonValue &v : out)
                    if (matches(v.toObject(), m))
                        filtered.append(v);
                out = filtered;
            } else if (const QJsonObject l = s.value(QStringLiteral("$limit")).toObject();
                       !l.isEmpty()) {
                // $limit arrives as a scalar, not an object — handled below.
            } else if (s.contains(QStringLiteral("$count"))) {
                out = QJsonArray{QJsonObject{{s.value("$count").toString(), out.size()}}};
            } else if (s.contains(QStringLiteral("$sort")) || s.contains(QStringLiteral("$project"))) {
                return errResult(QStringLiteral("$sort/$project not supported in demo transport"));
            }
        }
        return okResult(wrapCursor(out));
    }

    bool InMemoryDemoTransport::matches(const QJsonObject &doc, const QJsonObject &filter) const
    {
        for (auto it = filter.begin(); it != filter.end(); ++it) {
            const QJsonValue fv = unwrapEjson(it.value());
            const QJsonValue dv = unwrapEjson(doc.value(it.key()));
            if (fv.isObject()) {
                // $gt/$gte/$lt/$lte/$in comparisons on numbers/strings.
                const QJsonObject fobj = fv.toObject();
                bool pass = true;
                for (auto op = fobj.begin(); op != fobj.end(); ++op) {
                    const QString k = op.key();
                    const QJsonValue ov = unwrapEjson(op.value());
                    const double d = dv.isDouble() ? dv.toDouble() : 0.0;
                    const double o = ov.isDouble() ? ov.toDouble() : 0.0;
                    const bool numeric = dv.isDouble() && ov.isDouble();
                    const QString ds = dv.isString() ? dv.toString() : QString();
                    const QString os = ov.isString() ? ov.toString() : QString();
                    if (k == QLatin1String("$gt"))
                        pass = pass && (numeric ? d > o : ds > os);
                    else if (k == QLatin1String("$gte"))
                        pass = pass && (numeric ? d >= o : ds >= os);
                    else if (k == QLatin1String("$lt"))
                        pass = pass && (numeric ? d < o : ds < os);
                    else if (k == QLatin1String("$lte"))
                        pass = pass && (numeric ? d <= o : ds <= os);
                    else if (k == QLatin1String("$in"))
                        pass = pass && ov.toArray().contains(dv);
                    else if (k == QLatin1String("$eq"))
                        pass = pass && dv == ov;
                    else if (k == QLatin1String("$ne"))
                        pass = pass && dv != ov;
                    else
                        return false;
                }
                if (!pass)
                    return false;
            } else if (dv != fv) {
                return false;
            }
        }
        return true;
    }

    QString InMemoryDemoTransport::toEjson(const QJsonValue &v)
    {
        return QString::fromUtf8(
            v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)
                        : QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }

    IMongoTransport::Result InMemoryDemoTransport::listDatabases()
    {
        return okResult(QJsonArray{QJsonObject{{"name", "catalog"},
                                               {"sizeOnDisk", 4096},
                                               {"empty", false}}});
    }

    IMongoTransport::Result InMemoryDemoTransport::listCollections(const QString &)
    {
        QJsonArray out;
        for (auto it = _collections.begin(); it != _collections.end(); ++it)
            out.append(QJsonObject{{"name", it.key()}, {"type", "collection"}});
        return okResult(wrapCursor(out));
    }

    IMongoTransport::Result InMemoryDemoTransport::ping()
    {
        return okResult(QJsonObject{{"ok", 1.0}});
    }

    IMongoTransport::Result InMemoryDemoTransport::killOp(const QString &)
    {
        return okResult(QJsonObject{{"info", "demo: nothing running"}, {"ok", 1.0}});
    }

    void InMemoryDemoTransport::disconnect()
    {
        _connected = false;
    }
}
