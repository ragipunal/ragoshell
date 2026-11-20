#include "ragoshell/core/engine/ShellHost.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <QByteArray>
#include <QRandomGenerator>
#include <QStringList>

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
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

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
            return filter.isUndefined() || filter.isNull() ? bsoncxx::builder::basic::make_document() : bson.toDocument(filter);
        }

        std::chrono::milliseconds millis(const QJSValue &v)
        {
            return std::chrono::milliseconds(static_cast<std::int64_t>(v.toNumber()));
        }

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
        if (!iterator) {
            iterator.emplace(cursor.begin());
            consumed = false;
        } else if (consumed) {
            ++*iterator;
            consumed = false;
        }
        return *iterator != cursor.end();
    }

    bsoncxx::document::value ShellHost::CursorState::next()
    {
        if (!hasNext())
            throw std::runtime_error("the cursor has no more documents");
        consumed = true;
        return bsoncxx::document::value(**iterator);
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

    ShellHost::ErrorInfo ShellHost::describeError(const std::exception &error)
    {
        ErrorInfo info;
        info.message = QString::fromUtf8(error.what());

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
                    info.message = QString::fromUtf8(m.get_string().value.data(),
                                                     static_cast<qsizetype>(m.get_string().value.size()));
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
                *error = QString::fromUtf8(e.what());
            return false;
        }
    }

    bool ShellHost::connect(const ShellConnectOptions &options, QString *error)
    {
        disconnect();
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
        try {
            client().database("admin").run_command(make_document(kvp("ping", 1)));
            return true;
        } catch (const std::exception &e) {
            if (error)
                *error = QString::fromUtf8(e.what());
            return false;
        }
    }

    void ShellHost::disconnect()
    {
        closeAllCursors();
        _client.reset();
    }

    int ShellHost::addCursor(mongocxx::cursor &&cursor)
    {
        const int id = _nextCursorId++;
        _cursors[id] = std::make_unique<CursorState>(std::move(cursor));
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
            const bsoncxx::document::value reply =
                client().database(db.toStdString()).run_command(_bson.toDocument(command));
            return _bson.toJs(reply.view());
        } catch (const std::exception &e) {
            throwError(e);
            return QJSValue();
        }
    }

    int ShellHost::find(const QString &db, const QString &collection, const QJSValue &filter, const QJSValue &options)
    {
        try {
            mongocxx::options::find opts;
            if (has(options, "projection")) opts.projection(_bson.toDocument(get(options, "projection")));
            if (has(options, "sort")) opts.sort(_bson.toDocument(get(options, "sort")));
            if (has(options, "collation")) opts.collation(_bson.toDocument(get(options, "collation")));
            if (has(options, "min")) opts.min(_bson.toDocument(get(options, "min")));
            if (has(options, "max")) opts.max(_bson.toDocument(get(options, "max")));
            if (has(options, "let")) opts.let(_bson.toDocument(get(options, "let")));
            if (has(options, "limit")) opts.limit(static_cast<std::int64_t>(get(options, "limit").toNumber()));
            if (has(options, "skip")) opts.skip(static_cast<std::int64_t>(get(options, "skip").toNumber()));
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

            const bsoncxx::document::value filterDoc = filterOrEmpty(_bson, filter);
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
            mongocxx::pipeline stages;
            for (const bsoncxx::document::value &stage : _bson.toDocuments(pipeline))
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
            mongocxx::options::count opts;
            if (has(options, "limit")) opts.limit(static_cast<std::int64_t>(get(options, "limit").toNumber()));
            if (has(options, "skip")) opts.skip(static_cast<std::int64_t>(get(options, "skip").toNumber()));
            opts.max_time(has(options, "maxTimeMS") ? millis(get(options, "maxTimeMS")) : defaultMaxTime());
            if (has(options, "collation")) opts.collation(_bson.toDocument(get(options, "collation")));
            if (has(options, "hint")) {
                const QJSValue hint = get(options, "hint");
                if (hint.isString())
                    opts.hint(mongocxx::hint(hint.toString().toStdString()));
                else
                    opts.hint(mongocxx::hint(_bson.toDocument(hint)));
            }
            const bsoncxx::document::value filterDoc = filterOrEmpty(_bson, filter);
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
            mongocxx::options::distinct opts;
            opts.max_time(has(options, "maxTimeMS") ? millis(get(options, "maxTimeMS")) : defaultMaxTime());
            if (has(options, "collation")) opts.collation(_bson.toDocument(get(options, "collation")));
            const bsoncxx::document::value filterDoc = filterOrEmpty(_bson, filter);
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
        if (!_client)
            return QStringLiteral("[no connection]");
        QString address = _seedHosts;
        try {
            const bsoncxx::document::value reply =
                client().database("admin").run_command(make_document(kvp("isMaster", 1)));
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
            const bsoncxx::document::value info =
                client().database("admin").run_command(make_document(kvp("buildInfo", 1)));
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
