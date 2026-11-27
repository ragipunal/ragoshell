#include "ragoshell/core/engine/ShellHost.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include <QByteArray>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/error_code.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/server_error_code.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/hint.hpp>
#include <mongocxx/options/aggregate.hpp>
#include <mongocxx/options/client.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/distinct.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/tls.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"

namespace RagoShell
{
    namespace
    {
        namespace basic = bsoncxx::builder::basic;
        using basic::kvp;
        using basic::make_array;
        using basic::make_document;

        bool has(const QJSValue &options, const char *name)
        {
            if (!options.isObject())
                return false;
            const QJSValue v = options.property(QString::fromLatin1(name));
            return !v.isUndefined() && !v.isNull();
        }

        QJSValue get(const QJSValue &options, const char *name)
        {
            return options.property(QString::fromLatin1(name));
        }

        // A filter that is absent (undefined or null) matches everything.
        bsoncxx::document::value filterOrEmpty(const BsonJsConverter &bson, const QJSValue &filter)
        {
            return filter.isUndefined() || filter.isNull()
                ? bsoncxx::builder::basic::make_document()
                : bson.toDocument(filter);
        }

        std::int64_t doubleToInt64(double d, std::int64_t fallback = 0)
        {
            if (!std::isfinite(d))
                return fallback;
            constexpr double kMinI64 = -9223372036854775808.0; // -2^63 (exact in IEEE-754 double)
            constexpr double kMaxI64 = 9223372036854774784.0;  // largest IEEE-754 double <= INT64_MAX
            if (d <= kMinI64)
                return INT64_MIN;
            if (d >= kMaxI64)
                return INT64_MAX;
            return static_cast<std::int64_t>(std::trunc(d));
        }

        QString redactUriUserinfo(const QString &raw)
        {
            QString msg = raw;
            static const QRegularExpression uriUserinfo(
                QStringLiteral("mongodb(\\+srv)?://[^/\\s@]+@"));
            msg.replace(uriUserinfo, QStringLiteral("mongodb\\1://***@"));
            return msg;
        }

        std::chrono::milliseconds millis(const QJSValue &v)
        {
            return std::chrono::milliseconds(doubleToInt64(v.toNumber(), 0));
        }

        std::int64_t extractCursorId(bsoncxx::document::view cursorObj)
        {
            const auto idEl = cursorObj["id"];
            if (!idEl)
                return 0;
            if (idEl.type() == bsoncxx::type::k_int64)
                return idEl.get_int64().value;
            if (idEl.type() == bsoncxx::type::k_int32)
                return idEl.get_int32().value;
            if (idEl.type() == bsoncxx::type::k_double)
                return doubleToInt64(idEl.get_double().value, 0);
            return 0;
        }

        std::vector<bsoncxx::document::value> extractBatch(bsoncxx::document::view cursorObj,
                                                           const char *batchKey)
        {
            std::vector<bsoncxx::document::value> out;
            const auto b = cursorObj[batchKey];
            if (!b || b.type() != bsoncxx::type::k_array)
                return out;
            for (const auto &el : b.get_array().value) {
                if (el.type() == bsoncxx::type::k_document)
                    out.emplace_back(el.get_document().value);
            }
            return out;
        }

        class TransportServerException final : public std::exception
        {
        public:
            explicit TransportServerException(const IMongoTransport::Result &res)
                : _message(res.error.isEmpty() ? std::string("transport command failed")
                                               : res.error.toStdString()),
                  _code(res.code),
                  _codeName(res.codeName)
            {
            }

            const char *what() const noexcept override { return _message.c_str(); }
            int code() const noexcept { return _code; }
            const QString &codeName() const noexcept { return _codeName; }

        private:
            std::string _message;
            int _code = 0;
            QString _codeName;
        };

        // `uri` with the given options set (an option the URI already has is replaced).
        std::string withUriOptions(const std::string &uri, const std::vector<std::pair<std::string, std::string>> &options)
        {
            const QString text = QString::fromStdString(uri);
            const qsizetype q = text.indexOf(QLatin1Char('?'));
            QString base = q >= 0 ? text.left(q) : text;
            QStringList parts = q >= 0 ? text.mid(q + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts) : QStringList();

            for (const auto &option : options) {
                const QString key = QString::fromStdString(option.first);
                for (qsizetype i = parts.size() - 1; i >= 0; --i) {
                    if (parts.at(i).section(QLatin1Char('='), 0, 0).compare(key, Qt::CaseInsensitive) == 0)
                        parts.removeAt(i);
                }
                parts << key + QLatin1Char('=') + QString::fromStdString(option.second);
            }
            // A URI with options needs a path ("host:port/?...").
            const qsizetype schemeEnd = base.indexOf(QLatin1String("://"));
            if (base.indexOf(QLatin1Char('/'), schemeEnd >= 0 ? schemeEnd + 3 : 0) < 0)
                base += QLatin1Char('/');
            return (base + QLatin1Char('?') + parts.join(QLatin1Char('&'))).toStdString();
        }
    }

    // ---- cursors ------------------------------------------------------------------------------

    bool ShellHost::CursorState::hasNext()
    {
        if (isTransport) {
            while (batchIndex >= batch.size() && remoteCursorId != 0 && transport != nullptr) {
                basic::document cmd;
                cmd.append(kvp("getMore", remoteCursorId),
                           kvp("collection", collection.toStdString()));
                if (batchSize > 0)
                    cmd.append(kvp("batchSize", batchSize));
                if (maxTimeMs > 0)
                    cmd.append(kvp("maxTimeMS", maxTimeMs));
                const auto res = transport->runCommand(
                    db, QString::fromStdString(
                            bsoncxx::to_json(cmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
                if (!res.ok) {
                    remoteCursorId = 0;
                    throw TransportServerException(res);
                }
                const auto reply = bsoncxx::from_json(res.ejson.toStdString());
                const auto cursorEl = reply.view()["cursor"];
                if (!cursorEl || cursorEl.type() != bsoncxx::type::k_document) {
                    remoteCursorId = 0;
                    batch.clear();
                    batchIndex = 0;
                    break;
                }
                const auto cv = cursorEl.get_document().value;
                remoteCursorId = extractCursorId(cv);
                batch = extractBatch(cv, "nextBatch");
                batchIndex = 0;
            }
            return batchIndex < batch.size();
        }

        if (!iterator) {
            iterator.emplace(cursor->begin());
            consumed = false;
        } else if (consumed) {
            ++*iterator;
            consumed = false;
        }
        return *iterator != cursor->end();
    }

    bsoncxx::document::value ShellHost::CursorState::next()
    {
        if (!hasNext())
            throw std::runtime_error("the cursor has no more documents");
        if (isTransport)
            return std::move(batch[batchIndex++]);
        consumed = true;
        return bsoncxx::document::value(**iterator);
    }

    void ShellHost::CursorState::closeRemote() noexcept
    {
        if (!isTransport || remoteCursorId == 0 || transport == nullptr)
            return;
        const std::int64_t toKill = remoteCursorId;
        remoteCursorId = 0;
        try {
            const auto cmd = make_document(
                kvp("killCursors", collection.toStdString()),
                kvp("cursors", make_array(toKill)));
            transport->runCommand(
                db, QString::fromStdString(
                        bsoncxx::to_json(cmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
        } catch (...) {
            // best effort on close
        }
    }

    ShellHost::CursorState::~CursorState()
    {
        closeRemote();
    }

    // ---- the host -----------------------------------------------------------------------------

    ShellHost::ShellHost(QJSEngine *engine, QObject *parent) :
        QObject(parent),
        _engine(engine),
        _bson(engine)
    {
    }

    ShellHost::~ShellHost()
    {
        disconnect();
    }

    void ShellHost::install()
    {
        // The engine must not delete the host when it collects its JavaScript wrapper.
        QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
        _engine->globalObject().setProperty(QStringLiteral("__host"), _engine->newQObject(this));
    }

    mongocxx::client &ShellHost::client()
    {
        if (!_client)
            throw std::runtime_error("not connected to a MongoDB server");
        return *_client;
    }

    bsoncxx::document::value ShellHost::runTransportCommand(const QString &db,
                                                            bsoncxx::document::view cmd)
    {
        if (!_transport)
            throw std::runtime_error("not connected to a MongoDB server");
        const auto res = _transport->runCommand(
            db, QString::fromStdString(
                    bsoncxx::to_json(cmd, bsoncxx::ExtendedJsonMode::k_canonical)));
        if (!res.ok)
            throw TransportServerException(res);
        return bsoncxx::from_json(res.ejson.toStdString());
    }

    ShellHost::ErrorInfo ShellHost::describeError(const std::exception &error)
    {
        ErrorInfo info;
        info.message = redactUriUserinfo(QString::fromUtf8(error.what()));

        if (const auto *te = dynamic_cast<const TransportServerException *>(&error)) {
            info.name = QStringLiteral("MongoServerError");
            if (te->code() != 0) {
                info.hasCode = true;
                info.code = te->code();
            }
            info.codeName = te->codeName();
            return info;
        }

        if (const auto *op = dynamic_cast<const mongocxx::operation_exception *>(&error)) {
            info.name = QStringLiteral("MongoServerError");
            if (op->raw_server_error()) {
                const bsoncxx::document::view raw = op->raw_server_error()->view();
                if (const auto c = raw["code"]) {
                    if (c.type() == bsoncxx::type::k_int32) {
                        info.hasCode = true;
                        info.code = c.get_int32().value;
                    } else if (c.type() == bsoncxx::type::k_int64) {
                        info.hasCode = true;
                        info.code = static_cast<double>(c.get_int64().value);
                    }
                }
                if (const auto n = raw["codeName"]; n && n.type() == bsoncxx::type::k_string)
                    info.codeName = QString::fromUtf8(n.get_string().value.data(),
                                                      static_cast<qsizetype>(n.get_string().value.size()));
                if (const auto m = raw["errmsg"]; m && m.type() == bsoncxx::type::k_string)
                    info.message = redactUriUserinfo(
                        QString::fromUtf8(m.get_string().value.data(),
                                          static_cast<qsizetype>(m.get_string().value.size())));
            }
        }
        if (const auto *ex = dynamic_cast<const mongocxx::exception *>(&error)) {
            // The driver appends the category's text to a server error ("msg: generic server error").
            if (ex->code().category() == mongocxx::server_error_category()) {
                info.name = QStringLiteral("MongoServerError");
                const QString suffix = QStringLiteral(": ") + QString::fromStdString(ex->code().message());
                if (info.message.endsWith(suffix))
                    info.message.chop(suffix.size());
            }
            if (!info.hasCode) {
                info.hasCode = true;
                info.code = ex->code().value();
            }
        }
        return info;
    }

    void ShellHost::throwError(const std::exception &error)
    {
        const ErrorInfo info = describeError(error);
        QJSValue jsError = _engine->newErrorObject(QJSValue::GenericError, info.message);
        if (info.name != QLatin1String("Error"))
            jsError.setProperty(QStringLiteral("name"), info.name);
        if (info.hasCode)
            jsError.setProperty(QStringLiteral("code"), info.code);
        if (!info.codeName.isEmpty())
            jsError.setProperty(QStringLiteral("codeName"), info.codeName);
        _engine->throwError(jsError);
    }

    namespace
    {
        // The name the driver reports to the server (client metadata), so that currentOp can tell the
        // operations of this shell from everything else; the killer uses its own name.
        std::string newAppName(const char *role)
        {
            return clientAppName(std::string(role) + "-" + bsoncxx::oid().to_string().substr(14));
        }
    }

    bool ShellHost::openClient(const ShellConnectOptions &options, const std::string &appName,
                               std::unique_ptr<mongocxx::client> *opened, QString *error)
    {
        try {
            std::vector<std::pair<std::string, std::string>> uriOptions;
            // Without these a single-threaded client tries once and waits out the 10 s TCP connect
            // timeout of an unreachable host, whatever serverSelectionTimeoutMS says.
            uriOptions.emplace_back("serverSelectionTimeoutMS", std::to_string(options.serverSelectionTimeoutMs));
            uriOptions.emplace_back("serverSelectionTryOnce", "false");
            uriOptions.emplace_back("connectTimeoutMS", std::to_string(std::min(options.serverSelectionTimeoutMs, 10000)));
            if (options.directConnection)
                uriOptions.emplace_back("directConnection", "true");
            // A call the server never answers ends with the socket timeout, a little after the shell's own
            // time limit would have stopped the script.
            if (options.operationTimeoutMs > 0)
                uriOptions.emplace_back("socketTimeoutMS", std::to_string(std::max(30000, 2 * options.operationTimeoutMs)));
            if (!appName.empty())
                uriOptions.emplace_back("appName", appName);
            const std::string uri = withUriOptions(options.uri.toStdString(), uriOptions);

            mongocxx::options::client clientOptions;
            if (options.tls) {
                mongocxx::options::tls tls;
                tls.allow_invalid_certificates(options.tlsAllowInvalidCertificates);
                if (!options.tlsAllowInvalidCertificates && !options.tlsCAFile.isEmpty())
                    tls.ca_file(options.tlsCAFile.toStdString());
                if (!options.tlsCertificateKeyFile.isEmpty()) {
                    tls.pem_file(options.tlsCertificateKeyFile.toStdString());
                    if (!options.tlsCertificateKeyFilePassword.isEmpty())
                        tls.pem_password(options.tlsCertificateKeyFilePassword.toStdString());
                }
                if (!options.crlFile.isEmpty())
                    tls.crl_file(options.crlFile.toStdString());
                clientOptions.tls_opts(tls);
            }

            auto created = std::make_unique<mongocxx::client>(mongocxx::uri(uri), clientOptions);
            // mongocxx connects lazily: a ping makes a wrong host, port or password fail here.
            created->database("admin").run_command(make_document(kvp("ping", 1)));
            *opened = std::move(created);
            return true;
        } catch (const std::exception &e) {
            if (error)
                *error = redactUriUserinfo(QString::fromUtf8(e.what()));
            return false;
        }
    }

    bool ShellHost::connect(const ShellConnectOptions &options, QString *error)
    {
        disconnect();
        if (options.transport != nullptr) {
            const auto pingRes = options.transport->ping();
            if (!pingRes.ok) {
                if (error)
                    *error = pingRes.error.isEmpty() ? QStringLiteral("transport ping failed")
                                                     : redactUriUserinfo(pingRes.error);
                return false;
            }
            adoptTransport(options.transport, options);
            return true;
        }
        const std::string appName = newAppName("shell");
        std::unique_ptr<mongocxx::client> opened;
        if (!openClient(options, appName, &opened, error))
            return false;
        adopt(std::move(opened), appName, options);
        return true;
    }

    std::string ShellHost::newShellAppName()
    {
        return newAppName("shell");
    }

    void ShellHost::adopt(std::unique_ptr<mongocxx::client> client, const std::string &appName,
                          const ShellConnectOptions &options)
    {
        disconnect();
        _client = std::move(client);
        _transport = nullptr;
        _appName = appName;
        _connectOptions = options;
        _operationTimeoutMs = options.operationTimeoutMs;

        // mongodb://[user:password@]host1:port,host2:port/db?options -> host1:port,host2:port
        QString rest = options.uri;
        const qsizetype scheme = rest.indexOf(QLatin1String("://"));
        if (scheme >= 0)
            rest = rest.mid(scheme + 3);
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        if (slash >= 0)
            rest = rest.left(slash);
        const qsizetype at = rest.lastIndexOf(QLatin1Char('@'));
        _seedHosts = at >= 0 ? rest.mid(at + 1) : rest;
        _serverAddress.clear();
    }

    void ShellHost::adoptTransport(IMongoTransport *transport, const ShellConnectOptions &options)
    {
        disconnect();
        _transport = transport;
        _appName.clear();
        _connectOptions = options;
        _operationTimeoutMs = options.operationTimeoutMs;
        if (!options.transportAddress.isEmpty()) {
            _seedHosts = options.transportAddress;
        } else {
            QString rest = options.uri;
            const qsizetype scheme = rest.indexOf(QLatin1String("://"));
            if (scheme >= 0)
                rest = rest.mid(scheme + 3);
            const qsizetype slash = rest.indexOf(QLatin1Char('/'));
            if (slash >= 0)
                rest = rest.left(slash);
            const qsizetype at = rest.lastIndexOf(QLatin1Char('@'));
            _seedHosts = at >= 0 ? rest.mid(at + 1) : rest;
        }
        if (_seedHosts.isEmpty())
            _seedHosts = QStringLiteral("demo");
        _serverAddress.clear();
    }

    ShellHost::KillTarget ShellHost::killTarget() const
    {
        KillTarget target;
        target.options = _connectOptions;
        target.appName = _appName;
        return target;
    }

    void ShellHost::killOperations(const KillTarget &target, const std::atomic<bool> &stillRunning)
    {
        if (target.appName.empty())
            return;
        try {
            ShellConnectOptions options = target.options;
            options.serverSelectionTimeoutMs = 3000;
            options.operationTimeoutMs = 0;
            std::unique_ptr<mongocxx::client> killer;
            if (!openClient(options, newAppName("killer"), &killer, nullptr))
                return;
            auto admin = killer->database("admin");

            // An operation that has not reached the server yet is not in currentOp: look a few times
            // while the script is still running.
            for (int attempt = 0; attempt < 10; ++attempt) {
                bsoncxx::stdx::optional<bsoncxx::document::value> reply;
                try {
                    reply = admin.run_command(make_document(kvp("currentOp", 1), kvp("appName", target.appName),
                                                            kvp("active", true)));
                } catch (const std::exception &) {
                    // Without the privilege to see all operations, the user's own ones.
                    try {
                        reply = admin.run_command(make_document(kvp("currentOp", 1), kvp("$ownOps", true),
                                                                kvp("appName", target.appName), kvp("active", true)));
                    } catch (const std::exception &) {
                        return;
                    }
                }
                const auto inprog = reply->view()["inprog"];
                if (inprog && inprog.type() == bsoncxx::type::k_array) {
                    for (const auto &entry : inprog.get_array().value) {
                        if (entry.type() != bsoncxx::type::k_document)
                            continue;
                        const auto opid = entry.get_document().value["opid"];
                        if (!opid)
                            continue;
                        try {
                            admin.run_command(make_document(kvp("killOp", 1), kvp("op", opid.get_value())));
                        } catch (const std::exception &) {
                            // already finished, or not ours to kill
                        }
                    }
                }
                if (!stillRunning.load())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                if (!stillRunning.load())
                    break;
            }
        } catch (const std::exception &) {
            // best effort
        }
    }

    bool ShellHost::ping(QString *error)
    {
        if (_transport) {
            const auto res = _transport->ping();
            if (!res.ok && error)
                *error = redactUriUserinfo(res.error);
            return res.ok;
        }
        try {
            client().database("admin").run_command(make_document(kvp("ping", 1)));
            return true;
        } catch (const std::exception &e) {
            if (error)
                *error = redactUriUserinfo(QString::fromUtf8(e.what()));
            return false;
        }
    }

    void ShellHost::disconnect()
    {
        closeAllCursors();
        _client.reset();
        _transport = nullptr;
    }

    int ShellHost::addCursor(mongocxx::cursor &&cursor)
    {
        const int id = _nextCursorId++;
        _cursors[id] = std::make_unique<CursorState>(std::move(cursor));
        return id;
    }

    int ShellHost::openTransportCursor(const QString &db, const QString &collection,
                                       bsoncxx::document::view reply, int batchSize,
                                       std::int32_t maxTimeMs)
    {
        const auto cursorEl = reply["cursor"];
        if (!cursorEl || cursorEl.type() != bsoncxx::type::k_document)
            throw std::runtime_error("command reply is missing cursor document");
        const auto cv = cursorEl.get_document().value;
        auto state = std::make_unique<CursorState>();
        state->isTransport = true;
        state->transport = _transport;
        state->db = db;
        state->collection = collection;
        if (const auto nsEl = cv["ns"]; nsEl && nsEl.type() == bsoncxx::type::k_string) {
            const QString ns = QString::fromUtf8(nsEl.get_string().value.data(),
                                                 static_cast<qsizetype>(nsEl.get_string().value.size()));
            const qsizetype dot = ns.indexOf(QLatin1Char('.'));
            if (dot >= 0 && dot + 1 < ns.size())
                state->collection = ns.mid(dot + 1);
        }
        state->batch = extractBatch(cv, "firstBatch");
        state->remoteCursorId = extractCursorId(cv);
        state->batchSize = batchSize;
        state->maxTimeMs = maxTimeMs;
        const int id = _nextCursorId++;
        _cursors[id] = std::move(state);
        return id;
    }

    ShellHost::CursorState *ShellHost::cursorState(int cursorId)
    {
        const auto it = _cursors.find(cursorId);
        if (it == _cursors.end())
            throw std::runtime_error("unknown or closed cursor");
        return it->second.get();
    }

    std::vector<MongoDocumentPtr> ShellHost::takeDocuments(int cursorId, int maxCount, bool *exhausted)
    {
        std::vector<MongoDocumentPtr> documents;
        CursorState *state = cursorState(cursorId);
        while ((maxCount <= 0 || static_cast<int>(documents.size()) < maxCount) && state->hasNext())
            documents.push_back(MongoDocumentPtr(new MongoDocument(state->next())));
        if (exhausted)
            *exhausted = !state->hasNext();
        return documents;
    }

    // ---- JavaScript API -----------------------------------------------------------------------

    QJSValue ShellHost::runCommand(const QString &db, const QJSValue &command)
    {
        try {
            const bsoncxx::document::value cmdDoc = _bson.toDocument(command);
            const bsoncxx::document::value reply = _transport
                ? runTransportCommand(db, cmdDoc.view())
                : client().database(db.toStdString()).run_command(cmdDoc.view());
            return _bson.toJs(reply.view());
        } catch (const std::exception &e) {
            throwError(e);
            return QJSValue();
        }
    }

    int ShellHost::find(const QString &db, const QString &collection, const QJSValue &filter, const QJSValue &options)
    {
        try {
            const bsoncxx::document::value filterDoc = filterOrEmpty(_bson, filter);
            if (_transport) {
                basic::document cmd;
                cmd.append(kvp("find", collection.toStdString()),
                           kvp("filter", filterDoc.view()));
                if (has(options, "projection"))
                    cmd.append(kvp("projection", _bson.toDocument(get(options, "projection"))));
                if (has(options, "sort"))
                    cmd.append(kvp("sort", _bson.toDocument(get(options, "sort"))));
                if (has(options, "collation"))
                    cmd.append(kvp("collation", _bson.toDocument(get(options, "collation"))));
                if (has(options, "min"))
                    cmd.append(kvp("min", _bson.toDocument(get(options, "min"))));
                if (has(options, "max"))
                    cmd.append(kvp("max", _bson.toDocument(get(options, "max"))));
                if (has(options, "let"))
                    cmd.append(kvp("let", _bson.toDocument(get(options, "let"))));
                if (has(options, "limit")) {
                    const double limNum = get(options, "limit").toNumber();
                    if (std::isfinite(limNum)) {
                        const std::int64_t lim = doubleToInt64(limNum, 0);
                        if (lim > 0) {
                            cmd.append(kvp("limit", lim));
                        } else if (lim < 0) {
                            const std::int64_t absLim = (lim == INT64_MIN) ? INT64_MAX : -lim;
                            cmd.append(kvp("limit", absLim), kvp("singleBatch", true));
                        }
                    }
                }
                if (has(options, "skip")) {
                    const std::int64_t sk = doubleToInt64(get(options, "skip").toNumber(), 0);
                    if (sk > 0)
                        cmd.append(kvp("skip", sk));
                }
                int batchSize = 0;
                if (has(options, "batchSize")) {
                    batchSize = get(options, "batchSize").toInt();
                    if (batchSize > 0)
                        cmd.append(kvp("batchSize", batchSize));
                }
                const auto maxMs = has(options, "maxTimeMS")
                    ? millis(get(options, "maxTimeMS")) : defaultMaxTime();
                const auto effectiveMaxMs = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(maxMs.count(), 0, INT32_MAX));
                if (effectiveMaxMs > 0)
                    cmd.append(kvp("maxTimeMS", effectiveMaxMs));
                if (has(options, "allowDiskUse"))
                    cmd.append(kvp("allowDiskUse", get(options, "allowDiskUse").toBool()));
                if (has(options, "noCursorTimeout"))
                    cmd.append(kvp("noCursorTimeout", get(options, "noCursorTimeout").toBool()));
                if (has(options, "returnKey"))
                    cmd.append(kvp("returnKey", get(options, "returnKey").toBool()));
                if (has(options, "showRecordId"))
                    cmd.append(kvp("showRecordId", get(options, "showRecordId").toBool()));
                if (has(options, "comment"))
                    cmd.append(kvp("comment", _bson.toValue(get(options, "comment"))));
                if (has(options, "hint")) {
                    const QJSValue hint = get(options, "hint");
                    if (hint.isString())
                        cmd.append(kvp("hint", hint.toString().toStdString()));
                    else
                        cmd.append(kvp("hint", _bson.toDocument(hint)));
                }
                const auto reply = runTransportCommand(db, cmd.view());
                return openTransportCursor(db, collection, reply.view(), batchSize, effectiveMaxMs);
            }

            mongocxx::options::find opts;
            if (has(options, "projection")) opts.projection(_bson.toDocument(get(options, "projection")));
            if (has(options, "sort")) opts.sort(_bson.toDocument(get(options, "sort")));
            if (has(options, "collation")) opts.collation(_bson.toDocument(get(options, "collation")));
            if (has(options, "min")) opts.min(_bson.toDocument(get(options, "min")));
            if (has(options, "max")) opts.max(_bson.toDocument(get(options, "max")));
            if (has(options, "let")) opts.let(_bson.toDocument(get(options, "let")));
            if (has(options, "limit")) {
                const double limNum = get(options, "limit").toNumber();
                if (std::isfinite(limNum))
                    opts.limit(doubleToInt64(limNum, 0));
            }
            if (has(options, "skip")) {
                const std::int64_t sk = doubleToInt64(get(options, "skip").toNumber(), 0);
                if (sk > 0)
                    opts.skip(sk);
            }
            if (has(options, "batchSize")) opts.batch_size(get(options, "batchSize").toInt());
            opts.max_time(has(options, "maxTimeMS") ? millis(get(options, "maxTimeMS")) : defaultMaxTime());
            if (has(options, "allowDiskUse")) opts.allow_disk_use(get(options, "allowDiskUse").toBool());
            if (has(options, "noCursorTimeout")) opts.no_cursor_timeout(get(options, "noCursorTimeout").toBool());
            if (has(options, "returnKey")) opts.return_key(get(options, "returnKey").toBool());
            if (has(options, "showRecordId")) opts.show_record_id(get(options, "showRecordId").toBool());
            if (has(options, "comment")) opts.comment(get(options, "comment").toString().toStdString());
            if (has(options, "hint")) {
                const QJSValue hint = get(options, "hint");
                if (hint.isString())
                    opts.hint(mongocxx::hint(hint.toString().toStdString()));
                else
                    opts.hint(mongocxx::hint(_bson.toDocument(hint)));
            }

            mongocxx::cursor cursor =
                client().database(db.toStdString()).collection(collection.toStdString()).find(filterDoc.view(), opts);
            return addCursor(std::move(cursor));
        } catch (const std::exception &e) {
            throwError(e);
            return -1;
        }
    }

    int ShellHost::aggregate(const QString &db, const QString &collection, const QJSValue &pipeline,
                             const QJSValue &options)
    {
        try {
            const std::vector<bsoncxx::document::value> stageDocs = _bson.toDocuments(pipeline);
            if (_transport) {
                basic::array pipeArr;
                for (const auto &stage : stageDocs)
                    pipeArr.append(stage.view());
                basic::document cursorOpt;
                int batchSize = 0;
                if (has(options, "batchSize")) {
                    batchSize = get(options, "batchSize").toInt();
                    if (batchSize > 0)
                        cursorOpt.append(kvp("batchSize", batchSize));
                }
                basic::document cmd;
                cmd.append(kvp("aggregate", collection.toStdString()),
                           kvp("pipeline", pipeArr.extract()),
                           kvp("cursor", cursorOpt.extract()));
                if (has(options, "allowDiskUse"))
                    cmd.append(kvp("allowDiskUse", get(options, "allowDiskUse").toBool()));
                const auto maxMs = has(options, "maxTimeMS")
                    ? millis(get(options, "maxTimeMS")) : defaultMaxTime();
                const auto effectiveMaxMs = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(maxMs.count(), 0, INT32_MAX));
                if (effectiveMaxMs > 0)
                    cmd.append(kvp("maxTimeMS", effectiveMaxMs));
                if (has(options, "collation"))
                    cmd.append(kvp("collation", _bson.toDocument(get(options, "collation"))));
                if (has(options, "let"))
                    cmd.append(kvp("let", _bson.toDocument(get(options, "let"))));
                if (has(options, "comment"))
                    cmd.append(kvp("comment", _bson.toValue(get(options, "comment"))));
                if (has(options, "hint")) {
                    const QJSValue hint = get(options, "hint");
                    if (hint.isString())
                        cmd.append(kvp("hint", hint.toString().toStdString()));
                    else
                        cmd.append(kvp("hint", _bson.toDocument(hint)));
                }
                const auto reply = runTransportCommand(db, cmd.view());
                return openTransportCursor(db, collection, reply.view(), batchSize, effectiveMaxMs);
            }

            mongocxx::pipeline stages;
            for (const bsoncxx::document::value &stage : stageDocs)
                stages.append_stage(stage.view());

            mongocxx::options::aggregate opts;
            if (has(options, "allowDiskUse")) opts.allow_disk_use(get(options, "allowDiskUse").toBool());
            if (has(options, "batchSize")) opts.batch_size(get(options, "batchSize").toInt());
            opts.max_time(has(options, "maxTimeMS") ? millis(get(options, "maxTimeMS")) : defaultMaxTime());
            if (has(options, "collation")) opts.collation(_bson.toDocument(get(options, "collation")));
            if (has(options, "let")) opts.let(_bson.toDocument(get(options, "let")));
            if (has(options, "comment")) opts.comment(_bson.toValue(get(options, "comment")));
            if (has(options, "hint")) {
                const QJSValue hint = get(options, "hint");
                if (hint.isString())
                    opts.hint(mongocxx::hint(hint.toString().toStdString()));
                else
                    opts.hint(mongocxx::hint(_bson.toDocument(hint)));
            }

            mongocxx::cursor cursor =
                client().database(db.toStdString()).collection(collection.toStdString()).aggregate(stages, opts);
            return addCursor(std::move(cursor));
        } catch (const std::exception &e) {
            throwError(e);
            return -1;
        }
    }

    QJSValue ShellHost::cursorNext(int cursorId, int count)
    {
        try {
            CursorState *state = cursorState(cursorId);
            QJSValue result = _engine->newArray();
            quint32 index = 0;
            while ((count <= 0 || static_cast<int>(index) < count) && state->hasNext())
                result.setProperty(index++, _bson.toJs(state->next().view()));
            return result;
        } catch (const std::exception &e) {
            throwError(e);
            return QJSValue();
        }
    }

    bool ShellHost::cursorHasNext(int cursorId)
    {
        try {
            return cursorState(cursorId)->hasNext();
        } catch (const std::exception &e) {
            throwError(e);
            return false;
        }
    }

    void ShellHost::cursorClose(int cursorId)
    {
        _cursors.erase(cursorId);
    }

    void ShellHost::closeAllCursors()
    {
        _cursors.clear();
    }

    double ShellHost::countDocuments(const QString &db, const QString &collection, const QJSValue &filter,
                                     const QJSValue &options)
    {
        try {
            const bsoncxx::document::value filterDoc = filterOrEmpty(_bson, filter);
            if (_transport) {
                basic::array pipe;
                pipe.append(make_document(kvp("$match", filterDoc.view())));
                if (has(options, "skip")) {
                    const std::int64_t sk = doubleToInt64(get(options, "skip").toNumber(), 0);
                    if (sk > 0)
                        pipe.append(make_document(kvp("$skip", sk)));
                }
                if (has(options, "limit")) {
                    const double limNum = get(options, "limit").toNumber();
                    if (std::isfinite(limNum)) {
                        const std::int64_t lim = doubleToInt64(limNum, 0);
                        if (lim > 0)
                            pipe.append(make_document(kvp("$limit", lim)));
                    }
                }
                pipe.append(make_document(
                    kvp("$group", make_document(kvp("_id", 1), kvp("n", make_document(kvp("$sum", 1)))))));

                basic::document cmd;
                cmd.append(kvp("aggregate", collection.toStdString()),
                           kvp("pipeline", pipe.extract()),
                           kvp("cursor", make_document()));
                const auto maxMs = has(options, "maxTimeMS")
                    ? millis(get(options, "maxTimeMS")) : defaultMaxTime();
                const auto effectiveMaxMs = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(maxMs.count(), 0, INT32_MAX));
                if (effectiveMaxMs > 0)
                    cmd.append(kvp("maxTimeMS", effectiveMaxMs));
                if (has(options, "collation"))
                    cmd.append(kvp("collation", _bson.toDocument(get(options, "collation"))));
                if (has(options, "hint")) {
                    const QJSValue hint = get(options, "hint");
                    if (hint.isString())
                        cmd.append(kvp("hint", hint.toString().toStdString()));
                    else
                        cmd.append(kvp("hint", _bson.toDocument(hint)));
                }
                const auto reply = runTransportCommand(db, cmd.view());
                const int cid = openTransportCursor(db, collection, reply.view(), 0, effectiveMaxMs);
                double n = 0.0;
                try {
                    CursorState *st = cursorState(cid);
                    if (st->hasNext()) {
                        const auto doc = st->next();
                        if (const auto nEl = doc.view()["n"]) {
                            if (nEl.type() == bsoncxx::type::k_int32)
                                n = nEl.get_int32().value;
                            else if (nEl.type() == bsoncxx::type::k_int64)
                                n = static_cast<double>(nEl.get_int64().value);
                            else if (nEl.type() == bsoncxx::type::k_double)
                                n = nEl.get_double().value;
                        }
                    }
                } catch (...) {
                    cursorClose(cid);
                    throw;
                }
                cursorClose(cid);
                return n;
            }

            mongocxx::options::count opts;
            if (has(options, "limit")) {
                const double limNum = get(options, "limit").toNumber();
                if (std::isfinite(limNum)) {
                    const std::int64_t lim = doubleToInt64(limNum, 0);
                    if (lim > 0)
                        opts.limit(lim);
                }
            }
            if (has(options, "skip")) {
                const std::int64_t sk = doubleToInt64(get(options, "skip").toNumber(), 0);
                if (sk > 0)
                    opts.skip(sk);
            }
            opts.max_time(has(options, "maxTimeMS") ? millis(get(options, "maxTimeMS")) : defaultMaxTime());
            if (has(options, "collation")) opts.collation(_bson.toDocument(get(options, "collation")));
            if (has(options, "hint")) {
                const QJSValue hint = get(options, "hint");
                if (hint.isString())
                    opts.hint(mongocxx::hint(hint.toString().toStdString()));
                else
                    opts.hint(mongocxx::hint(_bson.toDocument(hint)));
            }
            return static_cast<double>(client().database(db.toStdString())
                                           .collection(collection.toStdString())
                                           .count_documents(filterDoc.view(), opts));
        } catch (const std::exception &e) {
            throwError(e);
            return -1;
        }
    }

    double ShellHost::estimatedDocumentCount(const QString &db, const QString &collection)
    {
        try {
            if (_transport) {
                basic::document cmd;
                cmd.append(kvp("count", collection.toStdString()));
                const auto maxMs = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(defaultMaxTime().count(), 0, INT32_MAX));
                if (maxMs > 0)
                    cmd.append(kvp("maxTimeMS", maxMs));
                const auto reply = runTransportCommand(db, cmd.view());
                if (const auto nEl = reply.view()["n"]) {
                    if (nEl.type() == bsoncxx::type::k_int32)
                        return nEl.get_int32().value;
                    if (nEl.type() == bsoncxx::type::k_int64)
                        return static_cast<double>(nEl.get_int64().value);
                    if (nEl.type() == bsoncxx::type::k_double)
                        return nEl.get_double().value;
                }
                return 0.0;
            }
            return static_cast<double>(
                client().database(db.toStdString()).collection(collection.toStdString()).estimated_document_count());
        } catch (const std::exception &e) {
            throwError(e);
            return -1;
        }
    }

    QJSValue ShellHost::distinct(const QString &db, const QString &collection, const QString &field,
                                 const QJSValue &filter, const QJSValue &options)
    {
        try {
            const bsoncxx::document::value filterDoc = filterOrEmpty(_bson, filter);
            if (_transport) {
                basic::document cmd;
                cmd.append(kvp("distinct", collection.toStdString()),
                           kvp("key", field.toStdString()),
                           kvp("query", filterDoc.view()));
                const auto maxMs = has(options, "maxTimeMS")
                    ? millis(get(options, "maxTimeMS")) : defaultMaxTime();
                const auto effectiveMaxMs = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(maxMs.count(), 0, INT32_MAX));
                if (effectiveMaxMs > 0)
                    cmd.append(kvp("maxTimeMS", effectiveMaxMs));
                if (has(options, "collation"))
                    cmd.append(kvp("collation", _bson.toDocument(get(options, "collation"))));
                if (has(options, "comment"))
                    cmd.append(kvp("comment", _bson.toValue(get(options, "comment"))));
                const auto reply = runTransportCommand(db, cmd.view());
                if (const auto values = reply.view()["values"];
                    values && values.type() == bsoncxx::type::k_array)
                    return _bson.toJs(values.get_array().value);
                return _engine->newArray();
            }

            mongocxx::options::distinct opts;
            opts.max_time(has(options, "maxTimeMS") ? millis(get(options, "maxTimeMS")) : defaultMaxTime());
            if (has(options, "collation")) opts.collation(_bson.toDocument(get(options, "collation")));
            mongocxx::cursor cursor = client().database(db.toStdString())
                                          .collection(collection.toStdString())
                                          .distinct(field.toStdString(), filterDoc.view(), opts);
            for (const bsoncxx::document::view reply : cursor) {
                const auto values = reply["values"];
                if (values && values.type() == bsoncxx::type::k_array)
                    return _bson.toJs(values.get_array().value);
            }
            return _engine->newArray();
        } catch (const std::exception &e) {
            throwError(e);
            return QJSValue();
        }
    }

    QJSValue ShellHost::listCollections(const QString &db, const QJSValue &filter)
    {
        try {
            const bsoncxx::document::value filterDoc = filterOrEmpty(_bson, filter);
            if (_transport) {
                const auto reply = runTransportCommand(
                    db, make_document(kvp("listCollections", 1), kvp("filter", filterDoc.view())).view());
                const int cid = openTransportCursor(db, QStringLiteral("$cmd.listCollections"),
                                                    reply.view(), 0, 0);
                QJSValue result = _engine->newArray();
                quint32 index = 0;
                try {
                    CursorState *st = cursorState(cid);
                    while (st->hasNext())
                        result.setProperty(index++, _bson.toJs(st->next().view()));
                } catch (...) {
                    cursorClose(cid);
                    throw;
                }
                cursorClose(cid);
                return result;
            }

            mongocxx::cursor cursor = client().database(db.toStdString()).list_collections(filterDoc.view());
            QJSValue result = _engine->newArray();
            quint32 index = 0;
            for (const bsoncxx::document::view doc : cursor)
                result.setProperty(index++, _bson.toJs(doc));
            return result;
        } catch (const std::exception &e) {
            throwError(e);
            return QJSValue();
        }
    }

    QJSValue ShellHost::listIndexes(const QString &db, const QString &collection)
    {
        try {
            if (_transport) {
                const auto reply = runTransportCommand(
                    db, make_document(kvp("listIndexes", collection.toStdString())).view());
                const int cid = openTransportCursor(db, collection, reply.view(), 0, 0);
                QJSValue result = _engine->newArray();
                quint32 index = 0;
                try {
                    CursorState *st = cursorState(cid);
                    while (st->hasNext())
                        result.setProperty(index++, _bson.toJs(st->next().view()));
                } catch (...) {
                    cursorClose(cid);
                    throw;
                }
                cursorClose(cid);
                return result;
            }

            mongocxx::cursor cursor =
                client().database(db.toStdString()).collection(collection.toStdString()).list_indexes();
            QJSValue result = _engine->newArray();
            quint32 index = 0;
            for (const bsoncxx::document::view doc : cursor)
                result.setProperty(index++, _bson.toJs(doc));
            return result;
        } catch (const std::exception &e) {
            throwError(e);
            return QJSValue();
        }
    }

    QString ShellHost::newObjectId() const
    {
        return QString::fromStdString(bsoncxx::oid().to_string());
    }

    QString ShellHost::toEjson(const QJSValue &value, bool relaxed)
    {
        try {
            const bsoncxx::types::bson_value::value converted = _bson.toValue(value);
            if (converted.view().type() == bsoncxx::type::k_document)
                return QString::fromStdString(BsonJsConverter::toJson(converted.view().get_document().value, relaxed));

            // Not a document: wrap it, write the wrapper and cut the wrapper away again.
            const bsoncxx::document::value holder = make_document(kvp("v", converted.view()));
            const std::string json = BsonJsConverter::toJson(holder.view(), relaxed);
            const std::string prefix = "{ \"v\" : ";
            const std::string suffix = " }";
            if (json.size() >= prefix.size() + suffix.size() && json.compare(0, prefix.size(), prefix) == 0)
                return QString::fromStdString(json.substr(prefix.size(), json.size() - prefix.size() - suffix.size()));
            return QString::fromStdString(json);
        } catch (const std::exception &e) {
            throwError(e);
            return QString();
        }
    }

    QString ShellHost::serverAddress()
    {
        if (!_serverAddress.isEmpty())
            return _serverAddress;
        if (!_client && !_transport)
            return QStringLiteral("[no connection]");
        QString address = _seedHosts;
        try {
            const bsoncxx::document::value reply = _transport
                ? runTransportCommand(QStringLiteral("admin"), make_document(kvp("isMaster", 1)).view())
                : client().database("admin").run_command(make_document(kvp("isMaster", 1)));
            const auto primary = reply.view()["primary"];
            if (primary && primary.type() == bsoncxx::type::k_string)
                address = QString::fromUtf8(primary.get_string().value.data(),
                                            static_cast<qsizetype>(primary.get_string().value.size()));
        } catch (const std::exception &) {
            // keep the seed list
        }
        _serverAddress = address.isEmpty() ? QStringLiteral("[no connection]") : address;
        return _serverAddress;
    }

    QString ShellHost::randomHex(int bytes) const
    {
        QByteArray data(bytes < 0 ? 0 : bytes, Qt::Uninitialized);
        QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(data.data()), data.size() / 4);
        for (int i = (data.size() / 4) * 4; i < data.size(); ++i)
            data[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
        return QString::fromLatin1(data.toHex());
    }

    QString ShellHost::hexToBase64(const QString &hex) const
    {
        return QString::fromLatin1(QByteArray::fromHex(hex.toLatin1()).toBase64());
    }

    QString ShellHost::serverVersion()
    {
        try {
            const bsoncxx::document::value info = _transport
                ? runTransportCommand(QStringLiteral("admin"), make_document(kvp("buildInfo", 1)).view())
                : client().database("admin").run_command(make_document(kvp("buildInfo", 1)));
            const auto version = info.view()["version"];
            if (version && version.type() == bsoncxx::type::k_string)
                return QString::fromUtf8(version.get_string().value.data(),
                                         static_cast<qsizetype>(version.get_string().value.size()));
        } catch (const std::exception &) {
            // A user without the buildInfo privilege still has a working connection.
        }
        return QString();
    }
}
