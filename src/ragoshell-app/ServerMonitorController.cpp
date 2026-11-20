#include "ragoshell-app/ServerMonitorController.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <chrono>
#include <cmath>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/options/client.hpp>
#include <mongocxx/options/tls.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/Enums.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{
    namespace
    {
        constexpr int kMaxHistoryPoints = 60;

        mongocxx::options::client buildMonitorClientOptions(const ConnectionSettings &conn)
        {
            mongocxx::options::client opts;
            const SslSettings *ssl = conn.sslSettings();
            if (ssl && ssl->sslEnabled()) {
                mongocxx::options::tls tls;
                tls.allow_invalid_certificates(ssl->allowInvalidCertificates());
                if (!ssl->allowInvalidCertificates() && !ssl->caFile().empty())
                    tls.ca_file(ssl->caFile());
                if (ssl->usePemFile() && !ssl->pemKeyFile().empty()) {
                    tls.pem_file(ssl->pemKeyFile());
                    if (!ssl->pemPassPhrase().empty())
                        tls.pem_password(ssl->pemPassPhrase());
                }
                if (ssl->useAdvancedOptions() && !ssl->crlFile().empty())
                    tls.crl_file(ssl->crlFile());
                opts.tls_opts(tls);
            }
            return opts;
        }

        qint64 bsonInt64(bsoncxx::document::view doc, const char *key, qint64 def = 0)
        {
            const auto el = doc[key];
            if (!el)
                return def;
            switch (el.type()) {
            case bsoncxx::type::k_int32:  return static_cast<qint64>(el.get_int32().value);
            case bsoncxx::type::k_int64:  return static_cast<qint64>(el.get_int64().value);
            case bsoncxx::type::k_double: return static_cast<qint64>(std::llround(el.get_double().value));
            default: return def;
            }
        }

        double bsonDouble(bsoncxx::document::view doc, const char *key, double def = 0.0)
        {
            const auto el = doc[key];
            if (!el)
                return def;
            switch (el.type()) {
            case bsoncxx::type::k_int32:  return static_cast<double>(el.get_int32().value);
            case bsoncxx::type::k_int64:  return static_cast<double>(el.get_int64().value);
            case bsoncxx::type::k_double: return el.get_double().value;
            default: return def;
            }
        }

        QString bsonStr(bsoncxx::document::view doc, const char *key)
        {
            const auto el = doc[key];
            if (!el || el.type() != bsoncxx::type::k_string)
                return QString();
            const auto sv = el.get_string().value;
            return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
        }

        bool bsonBool(bsoncxx::document::view doc, const char *key, bool def = false)
        {
            const auto el = doc[key];
            if (!el)
                return def;
            if (el.type() == bsoncxx::type::k_bool)
                return el.get_bool().value;
            if (el.type() == bsoncxx::type::k_int32)
                return el.get_int32().value != 0;
            if (el.type() == bsoncxx::type::k_double)
                return el.get_double().value != 0.0;
            return def;
        }

        QString formatDurationText(double ms)
        {
            if (ms < 1.0)
                return QStringLiteral("<1 ms");
            if (ms < 1000.0)
                return QStringLiteral("%1 ms").arg(ms, 0, 'f', ms < 10.0 ? 1 : 0);
            const double secs = ms / 1000.0;
            if (secs < 60.0)
                return QStringLiteral("%1 s").arg(secs, 0, 'f', 2);
            const qint64 whole = static_cast<qint64>(secs);
            return QStringLiteral("%1m %2s").arg(whole / 60).arg(whole % 60);
        }

        struct PollSnapshot
        {
            QString fatalError;
            QString statusNote;
            bool hasStatus = false;

            QString host;
            QString version;
            QString storageEngine;
            QString role;
            QString replSetName;
            qint64 uptimeSec = 0;
            qint64 sampleMs = 0;

            qint64 opInsert = 0;
            qint64 opQuery = 0;
            qint64 opUpdate = 0;
            qint64 opDelete = 0;
            qint64 opGetmore = 0;
            qint64 opCommand = 0;

            int connCurrent = 0;
            int connAvailable = 0;
            qint64 connTotalCreated = 0;
            int activeClients = 0;

            qint64 netBytesIn = 0;
            qint64 netBytesOut = 0;
            qint64 netNumRequests = 0;

            double residentMb = 0.0;
            double virtualMb = 0.0;
            double wtUsedMb = 0.0;
            double wtMaxMb = 0.0;
            double wtDirtyMb = 0.0;

            QVariantList ops;
        };
    } // namespace

    ServerMonitorController::ServerMonitorController(
        const QString &serverUuid,
        const QString &title,
        std::unique_ptr<ConnectionSettings> connSettings,
        QObject *parent)
        : QObject(parent),
          _serverUuid(serverUuid),
          _title(title),
          _connSettings(std::move(connSettings))
    {
        if (_connSettings)
            _host = QString::fromStdString(_connSettings->getFullAddress());

        _workerThread = std::make_unique<std::thread>(&ServerMonitorController::workerLoop, this);
    }

    ServerMonitorController::~ServerMonitorController()
    {
        {
            std::lock_guard<std::mutex> lk(_wakeMutex);
            _stopFlag.store(true);
        }
        _wakeCv.notify_all();
        if (_workerThread && _workerThread->joinable())
            _workerThread->join();
    }

    void ServerMonitorController::setRefreshIntervalMs(int ms)
    {
        const int clamped = ms <= 0 ? 0 : std::max(500, ms);
        if (_refreshIntervalMs.load() == clamped)
            return;
        _refreshIntervalMs.store(clamped);
        emit configChanged();
        if (clamped > 0)
            refreshNow();
    }

    void ServerMonitorController::updateLocalPortIfMatching(int oldPort, int newPort)
    {
        if (oldPort <= 0 || newPort <= 0 || oldPort == newPort)
            return;

        {
            std::lock_guard<std::mutex> lk(_wakeMutex);
            if (!_connSettings ||
                _connSettings->serverHost() != "127.0.0.1" ||
                _connSettings->serverPort() != oldPort) {
                return;
            }
            _connSettings->setServerPort(newPort);
            _resetClient.store(true);
            _forcePoll.store(true);
        }
        _wakeCv.notify_all();
    }

    void ServerMonitorController::setActive(bool on)
    {
        if (_active.load() == on)
            return;
        {
            std::lock_guard<std::mutex> lk(_wakeMutex);
            _active.store(on);
        }
        emit activeChanged();
        if (on) {
            // Rates over the paused gap would be meaningless; restart the delta.
            _prevCounters.valid = false;
            refreshNow();
        }
    }

    void ServerMonitorController::setActiveOnly(bool v)
    {
        if (_activeOnly == v)
            return;
        _activeOnly = v;
        _includeIdleOps.store(!v);
        emit configChanged();
        applyFilteredOps();
        refreshNow();
    }

    void ServerMonitorController::setHideSystemOps(bool v)
    {
        if (_hideSystemOps == v)
            return;
        _hideSystemOps = v;
        emit configChanged();
        applyFilteredOps();
    }

    void ServerMonitorController::setMinDurationMs(int ms)
    {
        const int clamped = std::max(0, ms);
        if (_minDurationMs == clamped)
            return;
        _minDurationMs = clamped;
        emit configChanged();
        applyFilteredOps();
    }

    void ServerMonitorController::setSearchFilter(const QString &q)
    {
        if (_searchFilter == q)
            return;
        _searchFilter = q;
        emit configChanged();
        applyFilteredOps();
    }

    void ServerMonitorController::refreshNow()
    {
        {
            std::lock_guard<std::mutex> lk(_wakeMutex);
            _forcePoll.store(true);
        }
        _wakeCv.notify_one();
    }

    void ServerMonitorController::clearHistory()
    {
        _historySamples.clear();
        _prevCounters = RawCounters{};
        emit metricsUpdated();
    }

    void ServerMonitorController::killOperation(const QString &opidStr)
    {
        const QString trimmed = opidStr.trimmed();
        if (trimmed.isEmpty())
            return;
        {
            std::lock_guard<std::mutex> lk(_wakeMutex);
            _pendingKillOpId = trimmed;
            _forcePoll.store(true);
        }
        _wakeCv.notify_one();
    }

    void ServerMonitorController::copyToClipboard(const QString &text) const
    {
        if (QClipboard *cb = QGuiApplication::clipboard())
            cb->setText(text);
    }

    QString ServerMonitorController::formatUptime(qint64 secs)
    {
        if (secs <= 0)
            return QStringLiteral("0s");
        const qint64 days = secs / 86400;
        secs %= 86400;
        const qint64 hours = secs / 3600;
        secs %= 3600;
        const qint64 mins = secs / 60;
        const qint64 remSecs = secs % 60;

        QStringList parts;
        if (days > 0)
            parts << QStringLiteral("%1d").arg(days);
        if (hours > 0 || days > 0)
            parts << QStringLiteral("%1h").arg(hours);
        if (mins > 0 || hours > 0 || days > 0)
            parts << QStringLiteral("%1m").arg(mins);
        parts << QStringLiteral("%1s").arg(remSecs);
        return parts.join(QLatin1Char(' '));
    }

    void ServerMonitorController::applyFilteredOps()
    {
        QVariantList out;
        const QString needle = _searchFilter.trimmed().toLower();

        for (const QVariant &item : _rawOps) {
            const QVariantMap row = item.toMap();
            if (_activeOnly && !row.value(QStringLiteral("active")).toBool())
                continue;
            if (_hideSystemOps && row.value(QStringLiteral("isSystem")).toBool())
                continue;
            const double durMs = row.value(QStringLiteral("durationMs")).toDouble();
            if (_minDurationMs > 0 && durMs < static_cast<double>(_minDurationMs))
                continue;
            if (!needle.isEmpty()) {
                const bool match =
                    row.value(QStringLiteral("opid")).toString().toLower().contains(needle) ||
                    row.value(QStringLiteral("op")).toString().toLower().contains(needle) ||
                    row.value(QStringLiteral("ns")).toString().toLower().contains(needle) ||
                    row.value(QStringLiteral("client")).toString().toLower().contains(needle) ||
                    row.value(QStringLiteral("planSummary")).toString().toLower().contains(needle) ||
                    row.value(QStringLiteral("commandSummary")).toString().toLower().contains(needle);
                if (!match)
                    continue;
            }
            out.append(row);
        }

        _filteredOps = out;
        emit operationsUpdated();
    }

    void ServerMonitorController::workerLoop()
    {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

        QPointer<ServerMonitorController> self(this);
        std::unique_ptr<mongocxx::client> client;

        while (!_stopFlag.load()) {
            QString killTarget;
            {
                std::lock_guard<std::mutex> lk(_wakeMutex);
                killTarget = _pendingKillOpId;
                _pendingKillOpId.clear();
                _forcePoll.store(false);
            }

            if (self) {
                QMetaObject::invokeMethod(self, [self]() {
                    if (!self || self->_busy)
                        return;
                    self->_busy = true;
                    emit self->busyChanged();
                }, Qt::QueuedConnection);
            }

            PollSnapshot snap;
            snap.sampleMs = QDateTime::currentMSecsSinceEpoch();
            QStringList notes;

            try {
                if (_resetClient.exchange(false))
                    client.reset();

                if (!client) {
                    std::string uriStr;
                    mongocxx::options::client clientOpts;
                    {
                        std::lock_guard<std::mutex> lk(_wakeMutex);
                        if (!_connSettings)
                            throw std::runtime_error("Connection settings are unavailable.");
                        // socketTimeoutMS bounds a hung serverStatus/currentOp so
                        // shutdown (join in the destructor) cannot block forever.
                        uriStr = buildMongoUri(*_connSettings) + "&socketTimeoutMS=15000";
                        clientOpts = buildMonitorClientOptions(*_connSettings);
                    }
                    mongocxx::uri uri(uriStr);
                    client = std::make_unique<mongocxx::client>(uri, std::move(clientOpts));
                }

                auto adminDb = client->database("admin");

                // 1. Optional killOp action requested by user
                if (!killTarget.isEmpty()) {
                    try {
                        bsoncxx::builder::basic::document killCmd;
                        killCmd.append(kvp("killOp", 1));
                        bool numericOk = false;
                        const qint64 numOp = killTarget.toLongLong(&numericOk);
                        if (numericOk && !killTarget.contains(QLatin1Char(':'))) {
                            if (numOp >= INT32_MIN && numOp <= INT32_MAX)
                                killCmd.append(kvp("op", static_cast<std::int32_t>(numOp)));
                            else
                                killCmd.append(kvp("op", static_cast<std::int64_t>(numOp)));
                        } else {
                            killCmd.append(kvp("op", killTarget.toStdString()));
                        }

                        const auto killRes = adminDb.run_command(killCmd.extract());
                        const QString infoMsg = bsonStr(killRes.view(), "info");
                        if (self) {
                            QMetaObject::invokeMethod(self, [self, killTarget, infoMsg]() {
                                if (!self)
                                    return;
                                emit self->operationKilled(
                                    true,
                                    infoMsg.isEmpty()
                                        ? tr("Operation %1 terminated.").arg(killTarget)
                                        : infoMsg);
                            }, Qt::QueuedConnection);
                        }
                    } catch (const std::exception &kex) {
                        const QString errMsg = QString::fromUtf8(kex.what());
                        if (self) {
                            QMetaObject::invokeMethod(self, [self, errMsg]() {
                                if (!self)
                                    return;
                                emit self->operationKilled(false, errMsg);
                            }, Qt::QueuedConnection);
                        }
                    }
                }

                // 2. Poll serverStatus
                try {
                    const auto ssDoc = adminDb.run_command(make_document(kvp("serverStatus", 1)));
                    const auto ss = ssDoc.view();
                    snap.hasStatus = true;
                    snap.host = bsonStr(ss, "host");
                    snap.version = bsonStr(ss, "version");
                    snap.uptimeSec = bsonInt64(ss, "uptime");

                    if (const auto se = ss["storageEngine"]; se && se.type() == bsoncxx::type::k_document)
                        snap.storageEngine = bsonStr(se.get_document().view(), "name");

                    if (bsonStr(ss, "process") == QLatin1String("mongos")) {
                        snap.role = QStringLiteral("MONGOS");
                        if (snap.storageEngine.isEmpty())
                            snap.storageEngine = QStringLiteral("mongos");
                    } else if (const auto repl = ss["repl"]; repl && repl.type() == bsoncxx::type::k_document) {
                        const auto rv = repl.get_document().view();
                        snap.replSetName = bsonStr(rv, "setName");
                        if (bsonBool(rv, "isWritablePrimary") || bsonBool(rv, "ismaster"))
                            snap.role = QStringLiteral("PRIMARY");
                        else if (bsonBool(rv, "secondary"))
                            snap.role = QStringLiteral("SECONDARY");
                        else if (bsonBool(rv, "arbiterOnly"))
                            snap.role = QStringLiteral("ARBITER");
                        else
                            snap.role = QStringLiteral("REPLICA");
                    } else {
                        snap.role = QStringLiteral("Standalone");
                    }

                    if (const auto opc = ss["opcounters"]; opc && opc.type() == bsoncxx::type::k_document) {
                        const auto ov = opc.get_document().view();
                        snap.opInsert  = bsonInt64(ov, "insert");
                        snap.opQuery   = bsonInt64(ov, "query");
                        snap.opUpdate  = bsonInt64(ov, "update");
                        snap.opDelete  = bsonInt64(ov, "delete");
                        snap.opGetmore = bsonInt64(ov, "getmore");
                        snap.opCommand = bsonInt64(ov, "command");
                    }

                    if (const auto conn = ss["connections"]; conn && conn.type() == bsoncxx::type::k_document) {
                        const auto cv = conn.get_document().view();
                        snap.connCurrent      = static_cast<int>(bsonInt64(cv, "current"));
                        snap.connAvailable    = static_cast<int>(bsonInt64(cv, "available"));
                        snap.connTotalCreated = bsonInt64(cv, "totalCreated");
                    }

                    if (const auto gl = ss["globalLock"]; gl && gl.type() == bsoncxx::type::k_document) {
                        const auto gv = gl.get_document().view();
                        if (const auto ac = gv["activeClients"]; ac && ac.type() == bsoncxx::type::k_document)
                            snap.activeClients = static_cast<int>(bsonInt64(ac.get_document().view(), "total"));
                    }

                    if (const auto net = ss["network"]; net && net.type() == bsoncxx::type::k_document) {
                        const auto nv = net.get_document().view();
                        snap.netBytesIn     = bsonInt64(nv, "bytesIn");
                        snap.netBytesOut    = bsonInt64(nv, "bytesOut");
                        snap.netNumRequests = bsonInt64(nv, "numRequests");
                    }

                    if (const auto mem = ss["mem"]; mem && mem.type() == bsoncxx::type::k_document) {
                        const auto mv = mem.get_document().view();
                        snap.residentMb = bsonDouble(mv, "resident");
                        snap.virtualMb  = bsonDouble(mv, "virtual");
                    }

                    if (const auto wt = ss["wiredTiger"]; wt && wt.type() == bsoncxx::type::k_document) {
                        const auto wtv = wt.get_document().view();
                        if (const auto cache = wtv["cache"]; cache && cache.type() == bsoncxx::type::k_document) {
                            const auto cv = cache.get_document().view();
                            constexpr double kBytesPerMb = 1024.0 * 1024.0;
                            snap.wtUsedMb  = bsonDouble(cv, "bytes currently in the cache") / kBytesPerMb;
                            snap.wtMaxMb   = bsonDouble(cv, "maximum bytes configured") / kBytesPerMb;
                            snap.wtDirtyMb = bsonDouble(cv, "tracked dirty bytes in the cache") / kBytesPerMb;
                        }
                    }
                } catch (const std::exception &ssex) {
                    notes << QStringLiteral("serverStatus: %1").arg(QString::fromUtf8(ssex.what()));
                }

                // 3. Poll currentOp (with automatic $ownOps fallback for non-admin users)
                auto parseInprog = [&](bsoncxx::document::view replyView) {
                    const auto inprogEl = replyView["inprog"];
                    if (!inprogEl || inprogEl.type() != bsoncxx::type::k_array)
                        return;

                    for (const auto &elem : inprogEl.get_array().value) {
                        if (elem.type() != bsoncxx::type::k_document)
                            continue;
                        const auto opDoc = elem.get_document().view();

                        QString opidText;
                        if (const auto idEl = opDoc["opid"]) {
                            if (idEl.type() == bsoncxx::type::k_int32)
                                opidText = QString::number(idEl.get_int32().value);
                            else if (idEl.type() == bsoncxx::type::k_int64)
                                opidText = QString::number(idEl.get_int64().value);
                            else if (idEl.type() == bsoncxx::type::k_string) {
                                const auto sv = idEl.get_string().value;
                                opidText = QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
                            }
                        }

                        const QString opType = bsonStr(opDoc, "op");
                        const QString ns = bsonStr(opDoc, "ns");
                        const QString desc = bsonStr(opDoc, "desc");
                        QString clientStr = bsonStr(opDoc, "client");
                        if (clientStr.isEmpty())
                            clientStr = bsonStr(opDoc, "client_s");
                        const QString appName = bsonStr(opDoc, "appName");
                        if (!appName.isEmpty())
                            clientStr = clientStr.isEmpty() ? appName : (clientStr + QStringLiteral(" · ") + appName);

                        const bool active = bsonBool(opDoc, "active", true);
                        const bool waitingForLock = bsonBool(opDoc, "waitingForLock", false);
                        const QString planSummary = bsonStr(opDoc, "planSummary");

                        double durationMs = 0.0;
                        if (opDoc["microsecs_running"])
                            durationMs = bsonDouble(opDoc, "microsecs_running") / 1000.0;
                        else if (opDoc["secs_running"])
                            durationMs = bsonDouble(opDoc, "secs_running") * 1000.0;

                        QString cmdSummary;
                        if (const auto cmdEl = opDoc["command"]; cmdEl && cmdEl.type() == bsoncxx::type::k_document) {
                            cmdSummary = QString::fromUtf8(
                                bsoncxx::to_json(cmdEl.get_document().view(), bsoncxx::ExtendedJsonMode::k_relaxed));
                        } else if (const auto qEl = opDoc["query"]; qEl && qEl.type() == bsoncxx::type::k_document) {
                            cmdSummary = QString::fromUtf8(
                                bsoncxx::to_json(qEl.get_document().view(), bsoncxx::ExtendedJsonMode::k_relaxed));
                        }

                        // Skip our own transient currentOp / serverStatus polling command
                        if (cmdSummary.contains(QLatin1String("\"currentOp\"")) ||
                            cmdSummary.contains(QLatin1String("\"serverStatus\""))) {
                            continue;
                        }

                        if (cmdSummary.size() > 260)
                            cmdSummary = cmdSummary.left(257) + QStringLiteral("…");

                        const bool isSystem =
                            ns.isEmpty() ||
                            ns.startsWith(QLatin1String("local.")) ||
                            ns.startsWith(QLatin1String("config.")) ||
                            ns == QLatin1String("admin.$cmd") ||
                            opType == QLatin1String("none") ||
                            (clientStr.isEmpty() && desc.startsWith(QLatin1String("Repl")));

                        const QString rawJson = QtUtils::toQString(
                            BsonUtils::jsonString(opDoc, TenGen, 1, DefaultEncoding, Utc));

                        snap.ops.append(QVariantMap{
                            {QStringLiteral("opid"),           opidText},
                            {QStringLiteral("op"),             opType.isEmpty() ? QStringLiteral("op") : opType},
                            {QStringLiteral("ns"),             ns.isEmpty() ? QStringLiteral("—") : ns},
                            {QStringLiteral("client"),         clientStr.isEmpty() ? desc : clientStr},
                            {QStringLiteral("desc"),           desc},
                            {QStringLiteral("active"),         active},
                            {QStringLiteral("waitingForLock"), waitingForLock},
                            {QStringLiteral("planSummary"),    planSummary},
                            {QStringLiteral("durationMs"),     durationMs},
                            {QStringLiteral("durationText"),   formatDurationText(durationMs)},
                            {QStringLiteral("commandSummary"), cmdSummary},
                            {QStringLiteral("isSystem"),       isSystem},
                            {QStringLiteral("rawJson"),        rawJson}
                        });
                    }
                };

                try {
                    bsoncxx::builder::basic::document copCmd;
                    copCmd.append(kvp("currentOp", 1));
                    if (_includeIdleOps.load())
                        copCmd.append(kvp("$all", true));
                    const auto copDoc = adminDb.run_command(copCmd.extract());
                    parseInprog(copDoc.view());
                } catch (const std::exception &) {
                    try {
                        bsoncxx::builder::basic::document ownCmd;
                        ownCmd.append(kvp("currentOp", 1));
                        ownCmd.append(kvp("$ownOps", true));
                        if (_includeIdleOps.load())
                            ownCmd.append(kvp("$all", true));
                        const auto ownDoc = adminDb.run_command(ownCmd.extract());
                        parseInprog(ownDoc.view());
                        notes << tr("Showing own operations ($ownOps) — grant 'clusterMonitor' to view all client operations.");
                    } catch (const std::exception &copEx) {
                        notes << QStringLiteral("currentOp: %1").arg(QString::fromUtf8(copEx.what()));
                    }
                }

                snap.statusNote = notes.join(QStringLiteral("  |  "));
            } catch (const std::exception &ex) {
                snap.fatalError = QString::fromUtf8(ex.what());
                client.reset();
            }

            if (self && !_stopFlag.load()) {
                QMetaObject::invokeMethod(self, [self, snap = std::move(snap)]() mutable {
                    if (!self)
                        return;
                    self->_busy = false;
                    emit self->busyChanged();

                    self->_lastUpdated = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
                    self->_errorText = snap.fatalError;
                    self->_statusNote = snap.statusNote;

                    if (snap.hasStatus) {
                        if (!snap.host.isEmpty())
                            self->_host = snap.host;
                        self->_version = snap.version;
                        self->_storageEngine = snap.storageEngine;
                        self->_role = snap.role;
                        self->_replSetName = snap.replSetName;
                        self->_uptimeSec = snap.uptimeSec;
                        self->_uptimeText = formatUptime(snap.uptimeSec);

                        self->_currentConnections = snap.connCurrent;
                        self->_availableConnections = snap.connAvailable;
                        self->_totalCreatedConnections = snap.connTotalCreated;
                        self->_activeClients = snap.activeClients;

                        self->_residentMb = snap.residentMb;
                        self->_virtualMb = snap.virtualMb;
                        self->_wtCacheUsedMb = snap.wtUsedMb;
                        self->_wtCacheMaxMb = snap.wtMaxMb;
                        self->_wtCacheDirtyMb = snap.wtDirtyMb;

                        if (self->_prevCounters.valid && snap.sampleMs > self->_prevCounters.sampleMs) {
                            const double dtSec = static_cast<double>(snap.sampleMs - self->_prevCounters.sampleMs) / 1000.0;
                            auto rate = [dtSec](qint64 cur, qint64 prev) -> double {
                                if (dtSec <= 0.05 || cur < prev)
                                    return 0.0;
                                return static_cast<double>(cur - prev) / dtSec;
                            };

                            self->_opsInsertRate  = rate(snap.opInsert,  self->_prevCounters.insert);
                            self->_opsQueryRate   = rate(snap.opQuery,   self->_prevCounters.query);
                            self->_opsUpdateRate  = rate(snap.opUpdate,  self->_prevCounters.update);
                            self->_opsDeleteRate  = rate(snap.opDelete,  self->_prevCounters.del);
                            self->_opsGetmoreRate = rate(snap.opGetmore, self->_prevCounters.getmore);
                            self->_opsCommandRate = rate(snap.opCommand, self->_prevCounters.command);
                            self->_opsTotalRate   = self->_opsInsertRate + self->_opsQueryRate +
                                                    self->_opsUpdateRate + self->_opsDeleteRate +
                                                    self->_opsGetmoreRate + self->_opsCommandRate;

                            self->_netInKbps  = rate(snap.netBytesIn,  self->_prevCounters.bytesIn) / 1024.0;
                            self->_netOutKbps = rate(snap.netBytesOut, self->_prevCounters.bytesOut) / 1024.0;
                            self->_netRequestsRate = rate(snap.netNumRequests, self->_prevCounters.numRequests);

                            self->_historySamples.append(QVariantMap{
                                {QStringLiteral("ts"),          self->_lastUpdated},
                                {QStringLiteral("totalOps"),    self->_opsTotalRate},
                                {QStringLiteral("query"),       self->_opsQueryRate},
                                {QStringLiteral("insert"),      self->_opsInsertRate},
                                {QStringLiteral("update"),      self->_opsUpdateRate},
                                {QStringLiteral("del"),         self->_opsDeleteRate},
                                {QStringLiteral("getmore"),     self->_opsGetmoreRate},
                                {QStringLiteral("command"),     self->_opsCommandRate},
                                {QStringLiteral("netInKbps"),   self->_netInKbps},
                                {QStringLiteral("netOutKbps"),  self->_netOutKbps},
                                {QStringLiteral("connections"), self->_currentConnections}
                            });
                            while (self->_historySamples.size() > kMaxHistoryPoints)
                                self->_historySamples.removeFirst();
                        }

                        self->_prevCounters.valid       = true;
                        self->_prevCounters.sampleMs    = snap.sampleMs;
                        self->_prevCounters.insert      = snap.opInsert;
                        self->_prevCounters.query       = snap.opQuery;
                        self->_prevCounters.update      = snap.opUpdate;
                        self->_prevCounters.del         = snap.opDelete;
                        self->_prevCounters.getmore     = snap.opGetmore;
                        self->_prevCounters.command     = snap.opCommand;
                        self->_prevCounters.bytesIn     = snap.netBytesIn;
                        self->_prevCounters.bytesOut    = snap.netBytesOut;
                        self->_prevCounters.numRequests = snap.netNumRequests;
                    }

                    self->_rawOps = std::move(snap.ops);
                    self->applyFilteredOps();
                    emit self->metricsUpdated();
                }, Qt::QueuedConnection);
            }

            // Sleep until next poll interval or explicit wakeup/stop
            std::unique_lock<std::mutex> lk(_wakeMutex);
            if (_stopFlag.load())
                break;
            if (_forcePoll.load() || !_pendingKillOpId.isEmpty())
                continue;

            const int intervalMs = _refreshIntervalMs.load();
            if (intervalMs <= 0 || !_active.load()) {
                // Paused or off screen: sleep until resumed, made visible,
                // or an explicit refresh / kill / stop arrives.
                _wakeCv.wait(lk, [this]() {
                    return _stopFlag.load() || _forcePoll.load() || !_pendingKillOpId.isEmpty() ||
                           (_active.load() && _refreshIntervalMs.load() > 0);
                });
            } else {
                _wakeCv.wait_for(lk, std::chrono::milliseconds(intervalMs), [this]() {
                    return _stopFlag.load() || _forcePoll.load() || !_pendingKillOpId.isEmpty();
                });
            }
        }
    }
}
