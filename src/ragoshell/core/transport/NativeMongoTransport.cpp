#include "NativeMongoTransport.h"

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/uri.hpp>

#include <cmath>

namespace
{
    // Server-side bound for every command the bridge executes
    // (8.6 review M1): all sessions share the gateway's single event loop,
    // so one unbounded op ($function, sleep, long aggregation) would stall
    // every session. The client may tighten this by sending its own
    // maxTimeMS; it may never raise it past this cap.
    constexpr std::int32_t kMaxOpTimeMs = RagoShell::NativeMongoTransport::kMaxOpTimeMs;

    // Rebuilds `command` with exactly one maxTimeMS, the last key, always an
    // int32 in [1, kMaxOpTimeMs]. A smaller valid user value wins; anything
    // else — absent, larger, 0 (MongoDB: "no limit"), negative, fractional,
    // NaN/inf or non-numeric — is replaced by the cap (2026-11-04 review R4:
    // `maxTimeMS: 0` used to pass the old "larger than the cap" test, and a
    // NaN double hit an undefined double->int64 cast). Duplicates are dropped
    // so the server cannot pick a different one than we checked.
    bsoncxx::document::value capMaxTime(const bsoncxx::document::view &command)
    {
        std::int32_t effective = kMaxOpTimeMs;
        for (const auto &el : command) {
            if (el.key() != "maxTimeMS")
                continue;
            double v = -1.0;
            if (el.type() == bsoncxx::type::k_int32)
                v = el.get_int32().value;
            else if (el.type() == bsoncxx::type::k_int64)
                v = static_cast<double>(el.get_int64().value);
            else if (el.type() == bsoncxx::type::k_double)
                v = el.get_double().value;
            // NaN fails every comparison, so it never reaches the cast.
            if (v >= 1.0 && v < effective && std::floor(v) == v)
                effective = static_cast<std::int32_t>(v);
        }
        bsoncxx::builder::basic::document cmd;
        for (const auto &el : command) {
            if (el.key() != "maxTimeMS")
                cmd.append(bsoncxx::builder::basic::kvp(el.key(), el.get_value()));
        }
        cmd.append(bsoncxx::builder::basic::kvp("maxTimeMS", effective));
        return cmd.extract();
    }

    RagoShell::IMongoTransport::Result okResult(const QString &ejson)
    {
        RagoShell::IMongoTransport::Result r;
        r.ok = true;
        r.ejson = ejson;
        return r;
    }

    RagoShell::IMongoTransport::Result errResult(const QString &error, int code = 0,
                                               const QString &codeName = {})
    {
        RagoShell::IMongoTransport::Result r;
        r.ok = false;
        r.error = error;
        r.code = code;
        r.codeName = codeName;
        return r;
    }
}

namespace RagoShell
{

    NativeMongoTransport::NativeMongoTransport() = default;
    NativeMongoTransport::~NativeMongoTransport() = default;

    NativeMongoTransport::Result NativeMongoTransport::connect(const QString &ejsonSettings)
    {
        try {
            // The dialog serializes { uri, timeoutMS? } — mirrors MongoWorker's
            // URI+options split so gateway transports accept the same payload.
            const auto settings = bsoncxx::from_json(ejsonSettings.toStdString());
            const auto uri = settings["uri"].get_string().value;
            const mongocxx::uri parsed{std::string(uri)};
            _client = std::make_unique<mongocxx::client>(parsed);
            // Fail fast instead of lazy discovery on first command.
            _client->database("admin").run_command(
                bsoncxx::builder::basic::make_document(
                    bsoncxx::builder::basic::kvp("ping", 1)));
            return okResult(R"({"ok":1})");
        } catch (const mongocxx::exception &e) {
            _client.reset();
            return errResult(QString::fromUtf8(e.what()));
        } catch (const std::exception &e) {
            _client.reset();
            return errResult(QString::fromUtf8(e.what()));
        }
    }

    NativeMongoTransport::Result NativeMongoTransport::runCommand(const QString &database,
                                                                  const QString &ejsonCommand)
    {
        if (!_client)
            return errResult(QStringLiteral("not connected"));

        try {
            const auto parsed = bsoncxx::from_json(ejsonCommand.toStdString());
            const auto cmd = capMaxTime(parsed.view());
            const auto res = _client->database(database.toStdString()).run_command(cmd.view());
            return okResult(QString::fromStdString(
                bsoncxx::to_json(res.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
        } catch (const mongocxx::exception &e) {
            return errResult(QString::fromUtf8(e.what()), static_cast<int>(e.code().value()),
                             QString::fromStdString(e.code().category().name()));
        } catch (const std::exception &e) {
            return errResult(QString::fromUtf8(e.what()));
        }
    }

    NativeMongoTransport::Result NativeMongoTransport::listDatabases()
    {
        if (!_client)
            return errResult(QStringLiteral("not connected"));

        try {
            bsoncxx::builder::basic::array out;
            for (const auto &db : _client->list_databases())
                out.append(db);
            return okResult(QString::fromStdString(bsoncxx::to_json(
                out.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
        } catch (const std::exception &e) {
            return errResult(QString::fromUtf8(e.what()));
        }
    }

    NativeMongoTransport::Result NativeMongoTransport::listCollections(const QString &database)
    {
        if (!_client)
            return errResult(QStringLiteral("not connected"));

        try {
            bsoncxx::builder::basic::array out;
            for (const auto &coll : _client->database(database.toStdString()).list_collections())
                out.append(coll);
            return okResult(QString::fromStdString(bsoncxx::to_json(
                out.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
        } catch (const std::exception &e) {
            return errResult(QString::fromUtf8(e.what()));
        }
    }

    NativeMongoTransport::Result NativeMongoTransport::ping()
    {
        return runCommand(QStringLiteral("admin"), QStringLiteral(R"({"ping":1})"));
    }

    NativeMongoTransport::Result NativeMongoTransport::killOp(const QString &ejsonFilter)
    {
        try {
            const auto filter = bsoncxx::from_json(ejsonFilter.toStdString());
            bsoncxx::builder::basic::document cmd;
            cmd.append(bsoncxx::builder::basic::kvp("killOp", 1));
            cmd.append(bsoncxx::builder::basic::kvp("op", filter["op"].get_value()));
            return runCommand(QStringLiteral("admin"),
                              QString::fromStdString(bsoncxx::to_json(cmd.view())));
        } catch (const std::exception &e) {
            return errResult(QString::fromUtf8(e.what()));
        }
    }

    QString NativeMongoTransport::withMaxTimeCap(const QString &ejsonCommand)
    {
        try {
            const auto parsed = bsoncxx::from_json(ejsonCommand.toStdString());
            return QString::fromStdString(bsoncxx::to_json(
                capMaxTime(parsed.view()).view(),
                bsoncxx::ExtendedJsonMode::k_canonical));
        } catch (const std::exception &) {
            return {};
        }
    }

    void NativeMongoTransport::disconnect()
    {
        _client.reset();
    }

}
