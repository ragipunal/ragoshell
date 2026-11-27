#include "ragoshell/core/mongodb/MongoWorker.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/transport/GatewayProtocol.h"
#include "ragoshell/core/transport/IMongoTransport.h"
#include "ragoshell/core/transport/InMemoryDemoTransport.h"
#include "ragoshell/core/transport/WebSocketGatewayTransport.h"

#include <algorithm>
#include <exception>

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/domain/MongoShellResult.h"
#include "ragoshell/core/domain/MongoCollectionInfo.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/engine/ScriptEngine.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/mongodb/MongoClient.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/utils/StringOperations.h"

namespace RagoShell
{
    namespace
    {
        std::string canonicalEndpoint(const std::string &addr)
        {
            QString q = QString::fromStdString(addr).trimmed().toLower();
            if (q.isEmpty())
                return std::string();

            QString host = q;
            QString port = QStringLiteral("27017");
            const qsizetype colon = q.lastIndexOf(QLatin1Char(':'));
            if (colon > 0 && !q.mid(colon + 1).contains(QLatin1Char(']'))) {
                host = q.left(colon).trimmed();
                const QString p = q.mid(colon + 1).trimmed();
                if (!p.isEmpty())
                    port = p;
            }
            if (host == QLatin1String("localhost") || host == QLatin1String("[::1]"))
                host = QStringLiteral("127.0.0.1");
            return (host + QLatin1Char(':') + port).toStdString();
        }

        bool replicaSetMatchesAnySeed(const ReplicaSet &setInfo,
                                      const std::vector<std::string> &seeds)
        {
            if (seeds.empty())
                return true;

            std::set<std::string> knownMembers;
            if (!setInfo.primary.empty())
                knownMembers.insert(canonicalEndpoint(setInfo.primary.toString()));
            for (const auto &mh : setInfo.membersAndHealths) {
                if (!mh.first.empty())
                    knownMembers.insert(canonicalEndpoint(mh.first));
            }
            if (knownMembers.empty())
                return true;

            for (const auto &seed : seeds) {
                if (knownMembers.count(canonicalEndpoint(seed)) > 0)
                    return true;
            }
            return false;
        }

        std::string redactUriUserinfo(const QString &raw)
        {
            QString msg = raw;
            static const QRegularExpression uriUserinfo(
                QStringLiteral("mongodb(\\+srv)?://[^/\\s@]+@"));
            msg.replace(uriUserinfo, QStringLiteral("mongodb\\1://***@"));
            return msg.toStdString();
        }
    }

    MongoWorker::MongoWorker(ConnectionSettings *connection, const QString &startupScript, int batchSize,
                             double mongoTimeoutSec, int shellTimeoutSec, QObject *parent) 
        : QObject(parent),
        _connSettings(connection),
        _client(nullptr),
        _scriptEngine(nullptr),
        _startupScript(startupScript),
        _batchSize(batchSize),
        _timerId(-1),
        _mongoTimeoutSec(mongoTimeoutSec),
        _shellTimeoutSec(shellTimeoutSec),
        _isQuiting(0)
    {
        // Whitespace removed from the start and the end of host string
        _connSettings->setServerHost(QString::fromStdString(_connSettings->serverHost()).trimmed().toStdString());
        _thread = new QThread();
        moveToThread(_thread);
        VERIFY(connect( _thread, SIGNAL(finished()), _thread, SLOT(deleteLater()) ));
        VERIFY(connect( _thread, SIGNAL(finished()), this, SLOT(deleteLater()) ));
        _thread->start();
    }

    void MongoWorker::timerEvent(QTimerEvent *event)
    {
        if (_timerId == event->timerId()) {
            keepAlive();
            return;
        }
    }

    void MongoWorker::restartReplicaSetConnection()
    {
        // With mongocxx, credentials are part of the connection URI,
        // so a re-auth is simply a fresh client connection.
        _client.reset();
        getConnection(true);
    }

    void MongoWorker::keepAlive()
    {
        // Never re-enter ScriptEngine or WebSocketGatewayTransport while an operation
        // is suspended inside a nested QEventLoop on this worker thread (W4 review H1).
        if (_inFlightOps > 0 || _scriptRunning)
            return;

        try {
            if (_client)
                pingDatabase(_client.get());

            if (_scriptEngine)
                _scriptEngine->ping();
            else if (_transport)
                (void)_transport->ping();

        } 
        catch(std::exception &ex) {
            sendLog(this, LogEvent::RBM_WARN, 
                "Failed to ping the server. " + std::string(ex.what()));
        }
    }

    uint64_t MongoWorker::beginNewOperation()
    {
        return ++_latestOpGen;
    }

    bool MongoWorker::isOpCancelled(uint64_t opGen) const
    {
        return _isQuiting != 0 || (opGen > 0 && opGen <= _cancelledOpGen.load());
    }

    ScriptEngine *MongoWorker::ensureScriptEngine()
    {
        IMongoTransport *transport = _transport;
        std::string transportErr;
        if (_connSettings->hasTransport() && !transport)
            transport = ensureTransportConnected(&transportErr);

        ScriptEngine *engine = nullptr;
        {
            QMutexLocker lock(&_engineMutex);
            if (!_scriptEngine) {
                auto eng = std::make_unique<ScriptEngine>(_connSettings, _shellTimeoutSec);
                eng->setBatchSize(_batchSize);
                eng->setStartupScript(_startupScript);
                _scriptEngine = std::move(eng);
            }
            if (_connSettings->hasTransport() || transport != nullptr)
                _scriptEngine->setTransport(transport, transportErr);
            engine = _scriptEngine.get();
        }
        if (_timerId == -1) {
            constexpr int PING_INTERVAL_MSEC { 60 * 1000 }; // 60 seconds
            _timerId = startTimer(PING_INTERVAL_MSEC);
        }
        return engine;
    }

    void MongoWorker::init()
    {        
        try {
            ScriptEngine *engine = ensureScriptEngine();
            engine->init("", _connSettings->defaultDatabase());
        } catch (const std::exception &ex) {
            auto const msg { "Failed to initialize MongoWorker. Reason: "};
            sendLog(this, LogEvent::RBM_ERROR, msg + std::string(ex.what()));
            throw std::runtime_error(msg + std::string(ex.what()));
        }
    }

    void MongoWorker::interrupt() {
        try {
            QMutexLocker lock(&_engineMutex);
            _cancelledOpGen.store(_latestOpGen.load());
            if (_isQuiting || !_scriptEngine)
                return;

            _scriptEngine->interrupt();
        } catch(const std::exception &ex) {
            sendLog(this, LogEvent::RBM_ERROR, std::string(ex.what()));
        }
    }

    MongoWorker::~MongoWorker()
    {
        if (_timerId != -1)
            killTimer(_timerId);

        // Tear down ScriptEngine (and any open ShellHost cursors) BEFORE
        // destroying _ownedTransport, _client, or _connSettings (W4 review C2).
        _scriptEngine.reset();
        _transport = nullptr;
        _ownedTransport.reset();
        _client.reset();
        delete _connSettings;
        _connSettings = nullptr;

        // QThread "_thread" and MongoWorker itself will be deleted later
        // (see MongoWorker() constructor)
    }

    void MongoWorker::stopAndDelete()
    {
        _isQuiting = 1;
        {
            QMutexLocker lock(&_engineMutex);
            _cancelledOpGen.store(_latestOpGen.load());
            if (_scriptEngine)
                _scriptEngine->interrupt();
        }
        _thread->quit();
    }

    void MongoWorker::applyPendingPortUpdate()
    {
        if (_inFlightOps > 0 || _scriptRunning || _pendingPort <= 0 || !_connSettings)
            return;
        const int newPort = _pendingPort;
        _pendingPort = 0;
        if (_connSettings->serverPort() == newPort)
            return;
        _connSettings->setServerPort(newPort);
        _client.reset();
        QMutexLocker lock(&_engineMutex);
        _scriptEngine.reset();
    }

    void MongoWorker::updateLocalServerPort(int newPort)
    {
        if (_isQuiting || newPort <= 0)
            return;

        QMetaObject::invokeMethod(this, [this, newPort]() {
            if (_isQuiting || !_connSettings)
                return;
            _pendingPort = newPort;
            applyPendingPortUpdate();
        }, Qt::QueuedConnection);
    }

    void MongoWorker::changeTimeout(int newTimeout)
    {
        QMutexLocker lock(&_engineMutex);
        _shellTimeoutSec = newTimeout;
        if (_scriptEngine)
            _scriptEngine->changeTimeout(newTimeout);
    }

    /**
     * @brief Initiate connection to MongoDB
     */
    bool MongoWorker::handle(EstablishConnectionRequest *event)
    {
        struct InFlightGuard
        {
            MongoWorker *self;
            explicit InFlightGuard(MongoWorker *w) : self(w) { ++self->_inFlightOps; }
            ~InFlightGuard() { --self->_inFlightOps; self->applyPendingPortUpdate(); }
        } opGuard(this);

        QMutexLocker lock(&_firstConnectionMutex);

        if (event->connectionType == ConnectionSecondary) {
            // Secondary connections are dedicated per-shell workers. Skip opening a redundant
            // mongocxx TLS connection and running listDatabases / replSetGetStatus before the
            // shell query starts; MongoClient will lazily connect if C++ CRUD/paging is used.
            std::unique_ptr<ReplicaSet> secondaryRepSet(new ReplicaSet());
            if (_connSettings->isReplicaSet()) {
                auto *rsSettings = _connSettings->replicaSetSettings();
                const std::string rsName = !rsSettings->setNameUserEntered().empty()
                    ? rsSettings->setNameUserEntered()
                    : rsSettings->cachedSetName();
                const HostAndPort primary = !_connSettings->serverHost().empty()
                    ? HostAndPort(_connSettings->serverHost(), _connSettings->serverPort())
                    : HostAndPort();
                secondaryRepSet.reset(new ReplicaSet(rsName, primary, {}));
            }
            auto connInfo = ConnectionInfo(_connSettings->getFullAddress(), {}, 0.0f, "", "", event->uuid);
            reply(event->sender(),
                  new EstablishConnectionResponse(this, connInfo, event->connectionType, *secondaryRepSet.release()));
            return true;
        }

        // Transport mode (WASM demo / gateway): connect through IMongoTransport
        // instead of building a mongocxx client — docs/TransportSeam.md §4,
        // docs/TransportShell.md §2.1.
        if (_connSettings->hasTransport() || _transport) {
            std::string transportErr;
            if (!ensureTransportConnected(&transportErr)) {
                reply(event->sender(), new EstablishConnectionResponse(this,
                      EventError(transportErr, EventError::ErrorCode::Unknown),
                      event->connectionType, ConnectionInfo(event->uuid), ReplicaSet(),
                      EstablishConnectionResponse::MongoConnection));
                sendLog(this, LogEvent::RBM_ERROR, transportErr);
                return false;
            }
            try {
                std::unique_ptr<MongoClient> client(getClient());
                std::vector<std::string> const dbNames = getDatabaseNamesSafe(event);
                const std::string verStr = client->dbVersionStr();
                const float verNum = verStr.empty() ? 0.0f : static_cast<float>(std::atof(verStr.c_str()));
                auto connInfo = ConnectionInfo(_connSettings->getFullAddress(), dbNames, verNum,
                                               verStr, client->getStorageEngineType(), event->uuid);
                reply(event->sender(), new EstablishConnectionResponse(this, connInfo,
                      event->connectionType, ReplicaSet()));
                return true;
            } catch (const std::exception &ex) {
                const std::string safeMsg = redactUriUserinfo(QString::fromUtf8(ex.what()));
                reply(event->sender(), new EstablishConnectionResponse(this,
                      EventError(safeMsg, EventError::ErrorCode::Unknown),
                      event->connectionType, ConnectionInfo(event->uuid), ReplicaSet(),
                      EstablishConnectionResponse::MongoConnection));
                sendLog(this, LogEvent::RBM_ERROR, safeMsg);
                return false;
            }
        }

        std::unique_ptr<ReplicaSet> repSetInfo(new ReplicaSet);
        auto errorCode = EventError::ErrorCode::Unknown;

        try {
            auto const& connAndErrorStr = getConnection(true);
            mongocxx::client *conn = connAndErrorStr.first;           
            
            // --- Connection failed for single server & replica set (no member of the set is reachable)
            if (!conn) 
            {
                auto errorReason = std::string("Connection failure: Unknown error.");
                auto const& connErrorStr = connAndErrorStr.second;

                if (_connSettings->sslSettings()->sslEnabled())
                     errorReason = 
                         "TLS tunnel failure: Network is unreachable or TLS connection rejected by server." + 
                          (connErrorStr.empty() ? "" : " Reason: " + connErrorStr);
                else {  // Non-TLS connections
                    if (_connSettings->isReplicaSet()) {
                        errorReason = "No member of the set is reachable." + 
                                      (connErrorStr.empty() ? "" : " Reason: " + connErrorStr);
                        std::vector<std::pair<std::string, bool>> membersAndHealths;
                        for (auto const& member : _connSettings->replicaSetSettings()->members())
                            membersAndHealths.push_back({ member, false });
                        
                        repSetInfo.reset(new ReplicaSet("", HostAndPort(), membersAndHealths, errorReason));
                    }
                    else    // single server
                        errorReason = "Network is unreachable." + (connErrorStr.empty() ? "" : " Reason: " + connErrorStr);
                }


                reply(event->sender(), new EstablishConnectionResponse(this, EventError(errorReason, errorCode),             
                      event->connectionType, event->uuid, *repSetInfo.release(), 
                      EstablishConnectionResponse::MongoConnection));

                return false;
            }
            
            // --- Single server: Connection successful 
            // --- Replica set:   Connection successful (primary reachable) or 
            //                    Connection failed (primary unreachable with at least one reachable member)
            if (_connSettings->isReplicaSet()) {
                ReplicaSet const& setInfo = getReplicaSetInfo();

                if (!setInfo.setName.empty() &&
                    _connSettings->replicaSetSettings()->setNameUserEntered().empty()) {
                    _connSettings->replicaSetSettings()->setCachedSetName(setInfo.setName);
                }

                // Verify at least one user-entered seed belongs to the discovered replica set
                // (normalizing localhost <-> 127.0.0.1 and supporting single-secondary seeds).
                auto const& members = _connSettings->replicaSetSettings()->members();
                if (!setInfo.primary.empty() && !replicaSetMatchesAnySeed(setInfo, members))
                {
                    std::string const errorStr { 
                        "Different members found under same replica set name \"" +
                         setInfo.setName + "\"." 
                    };
                    repSetInfo.reset(new ReplicaSet(setInfo));
                    errorCode = EventError::ErrorCode::ServerHasDifferentMembers;
                    sendLog(this, LogEvent::RBM_ERROR, errorStr);
                    throw std::runtime_error(errorStr);
                }

                if (setInfo.primary.empty()) {  // No reachable primary
                    // Pass possible reachable secondary(ies) info 
                    repSetInfo.reset(new ReplicaSet(setInfo)); 
                    sendLog(this, LogEvent::RBM_ERROR, setInfo.errorStr);
                    throw std::runtime_error(setInfo.errorStr);
                }
                else {  // Primary is reachable, save setInfo and continue
                    repSetInfo.reset(new ReplicaSet(setInfo));
                }
            }

            // Credentials are part of the connection URI in mongocxx;
            // an auth failure would have thrown during the ping in getConnection().

            std::unique_ptr<MongoClient> client(getClient());
            std::vector<std::string> const dbNames = getDatabaseNamesSafe(event);

            // If we do not have databases, it means that we are unable to
            // execute "listdatabases" command and we have nothing to show.
            if (dbNames.size() == 0)
                throw std::runtime_error("Failed to execute \"listdatabases\" command.");

            const std::string verStr = client->dbVersionStr();
            const float verNum = verStr.empty() ? 0.0f : static_cast<float>(std::atof(verStr.c_str()));
            auto connInfo = ConnectionInfo(_connSettings->getFullAddress(), dbNames, verNum, 
                                           verStr, client->getStorageEngineType(), event->uuid);

            // todo: two ctors for rep.set and single server.
            reply(event->sender(), new EstablishConnectionResponse(this, connInfo, event->connectionType, 
                                                                   *repSetInfo.release()));
            return true;
        } 
        catch(const std::exception &ex) {
            auto errorReason = _connSettings->sslSettings()->sslEnabled() ?
                               EstablishConnectionResponse::ErrorReason::MongoSslConnection : 
                               EstablishConnectionResponse::ErrorReason::MongoAuth;

            reply(event->sender(), new EstablishConnectionResponse(this, EventError(ex.what(), errorCode), 
                  event->connectionType, ConnectionInfo(event->uuid), *repSetInfo.release(), errorReason));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());  // todo: duplicate logging?
        }

        return false;
    }

    void MongoWorker::handle(RefreshReplicaSetFolderRequest *event)
    {

        try {
            ReplicaSet const& replicaSetInfo = getReplicaSetInfo();
            // Primary is unreachable, but there might be reachable secondary(ies)
            if (replicaSetInfo.primary.empty()) {
                reply(
                    event->sender(), 
                    new RefreshReplicaSetFolderResponse(
                        this, replicaSetInfo, event->expanded, EventError(replicaSetInfo.errorStr)
                    )
                );
                sendLog(this, LogEvent::RBM_ERROR, replicaSetInfo.errorStr);
                return;
            }
            else { // Primary is reachable
                reply(event->sender(), 
                    new RefreshReplicaSetFolderResponse(this, replicaSetInfo, event->expanded));
            }
        }
        catch (const std::exception &ex) {
            reply(
                event->sender(),
                new RefreshReplicaSetFolderResponse(
                    this, ReplicaSet(), event->expanded, EventError(ex.what())
                )
            );
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    std::string MongoWorker::getAuthBase() const
    {
        if (_connSettings->hasEnabledPrimaryCredential())
            return _connSettings->primaryCredential()->databaseName();

        return std::string();
    }

    std::vector<std::string> MongoWorker::getDatabaseNamesSafe(EstablishConnectionRequest* event /*= nullptr*/)
    {
        std::set<std::string> dbNames;
        auto const primaryCredential { _connSettings->primaryCredential() };

        try {
            std::unique_ptr<MongoClient> client(getClient());
            std::vector<std::string> dbNamesFetched { client->getDatabaseNames() };
            dbNames = std::set<std::string> { dbNamesFetched.cbegin(), dbNamesFetched.cend() };
        } 
        catch(const std::exception &ex) {
#if defined(__clang__) 
#pragma clang diagnostic push    
#pragma clang diagnostic ignored "-Wlogical-op-parentheses"
#endif
            bool const informUser {
                event != nullptr &&
                event->connectionType == ConnectionType::ConnectionPrimary &&
                _connSettings->credentialCount() > 0 &&
                !primaryCredential->useManuallyVisibleDbs() ||
                primaryCredential->manuallyVisibleDbs().empty()
            };
#if defined(__clang__) 
#pragma clang diagnostic pop
#endif
            std::string const hint {
                "\n\nHint: If this user has access to a specific database, "
                "please use \"Manually specify visible databases\" option in "
                "Connection Settings window -> Authentication tab."
            };
            sendLog(this, LogEvent::RBM_WARN, ex.what() + hint, informUser);
        }

        if (_connSettings->credentialCount() > 0 &&
            primaryCredential->useManuallyVisibleDbs() &&
            !primaryCredential->manuallyVisibleDbs().empty()
        )
        {        
            QString const manuallyVisibleDbs {
                QString::fromStdString(primaryCredential->manuallyVisibleDbs())
            };

            for (auto const& db : manuallyVisibleDbs.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString trimmed = db.trimmed();
                if (!trimmed.isEmpty())
                    dbNames.insert(trimmed.toStdString());
            }
        }

        std::string const authBase = getAuthBase();
        if (!authBase.empty() && authBase != "$external")
            dbNames.insert(authBase);

        std::string const defaultDb = _connSettings->defaultDatabase();
        if (!defaultDb.empty() && defaultDb != "$external")
            dbNames.insert(defaultDb);

        return std::vector<std::string> { dbNames.cbegin(), dbNames.cend() };
    }

    /**
     * @brief Load list of all database names
     */

    std::pair<mongocxx::client*, std::string> MongoWorker::getConnection(bool mayReturnNull /* = false */)
    {
        if (_connSettings && _connSettings->hasTransport()) {
            const std::string msg = "Direct driver connection refused on transport-backed channel.";
            if (mayReturnNull)
                return { nullptr, msg };
            throw std::runtime_error(msg);
        }

        if (_client)
            return { _client.get(), "" };

        try {
            if (_connSettings->isReplicaSet()) {
                auto *rsSettings = _connSettings->replicaSetSettings();
                if (rsSettings->setNameUserEntered().empty() && rsSettings->cachedSetName().empty()) {
                    const std::string discovered = connectAndGetReplicaSetName();
                    if (!discovered.empty())
                        rsSettings->setCachedSetName(discovered);
                }
            }

            _client.reset(new mongocxx::client(
                mongocxx::uri(buildUri()), buildClientOptions()));

            // mongocxx::client connects lazily; verify reachability with a ping.
            pingDatabase(_client.get());
        }
        catch (const std::exception &ex) {
            _client.reset();
            if (mayReturnNull)
                return { nullptr, ex.what() };
            throw;
        }

        return { _client.get(), "" };
    }

    IMongoTransport *MongoWorker::ensureTransportConnected(std::string *outError)
    {
        if (_transport)
            return _transport;
        if (!_connSettings || !_connSettings->hasTransport())
            return nullptr;

        const std::string kind = _connSettings->transportKind();
        const QString token = QString::fromStdString(_connSettings->transportToken());
        std::unique_ptr<IMongoTransport> created;

        if (kind == "memory") {
#if defined(Q_OS_WASM)
            created = std::make_unique<InMemoryDemoTransport>(
                InMemoryDemoTransport::sharedStore());
#else
            // The demo catalog must never masquerade as a real server:
            // a synced/imported "memory" record on desktop would serve
            // canned data under the stored name (8.6 review M3).
            created = nullptr;
#endif
        } else if (kind == "agent" || kind == "hosted"
                   || kind.rfind("ws://", 0) == 0
                   || kind.rfind("wss://", 0) == 0) {
            const QString url =
                kind == "agent"
                    ? QStringLiteral("ws://127.0.0.1:%1")
                          .arg(GatewayProtocol::kDefaultAgentPort)
                    : kind == "hosted"
                        ? CloudClient::deriveBridgeUrlFromPortal(
                              QString::fromUtf8(BrandInfo::portalUrl))
                        : QString::fromStdString(kind);
            const QString h = QUrl(url).host().toLower();
            const bool cleartext = url.startsWith(QStringLiteral("ws://"));
            const bool loopback = h == QLatin1String("localhost")
                                  || h == QLatin1String("127.0.0.1")
                                  || h == QLatin1String("::1")
                                  || h == QLatin1String("[::1]");
            if (cleartext && !loopback) {
                if (outError) {
                    *outError = "Refusing cleartext ws:// gateway to a "
                                "non-loopback host; use wss://";
                }
                return nullptr;
            }
            if (const auto *ssl = _connSettings->sslSettings();
                ssl && (!ssl->caFile().empty() || !ssl->pemKeyFile().empty()
                        || !ssl->pemPassPhrase().empty()
                        || !ssl->crlFile().empty())) {
                if (outError) {
                    *outError = "Custom CA / client-certificate / CRL "
                                "TLS options are not supported over "
                                "gateway channels (agent/self-hosted/hosted)";
                }
                return nullptr;
            }
            created = std::make_unique<WebSocketGatewayTransport>(url, token);
        }

        if (!created) {
            if (outError)
                *outError = "Unsupported transportKind '" + kind + "'";
            return nullptr;
        }

        try {
            const QString payload = kind == "memory"
                ? QStringLiteral("{}")
                : QString::fromUtf8(QJsonDocument(QJsonObject{
                      {QStringLiteral("uri"), QString::fromStdString(buildUri())}
                  }).toJson(QJsonDocument::Compact));
            const auto r = created->connect(payload);
            if (!r.ok) {
                if (outError)
                    *outError = redactUriUserinfo(r.error);
                return nullptr;
            }
            _ownedTransport = std::move(created);
            _transport = _ownedTransport.get();
            return _transport;
        } catch (const std::exception &ex) {
            if (outError)
                *outError = redactUriUserinfo(QString::fromUtf8(ex.what()));
            return nullptr;
        }
    }

    MongoClient *MongoWorker::getClient()
    {
        // Transport mode (WASM demo/gateway): commands travel as EJSON through
        // IMongoTransport instead of the driver's socket path. Fail closed if
        // transport initialization fails so secondary shell workers never
        // bypass the configured channel (docs/TransportShell.md §2.1, T1).
        if (_connSettings && _connSettings->hasTransport()) {
            std::string err;
            IMongoTransport *t = ensureTransportConnected(&err);
            if (!t)
                throw std::runtime_error(err.empty() ? "Transport is not connected." : err);
            return new MongoClient(t);
        }
        if (_transport)
            return new MongoClient(_transport);
        return new MongoClient(getConnection().first);
    }

    void MongoWorker::setTransport(IMongoTransport *transport)
    {
        _transport = transport;
    }

    std::string MongoWorker::buildUri() const
    {
        return buildMongoUri(*_connSettings);
    }

    mongocxx::options::client MongoWorker::buildClientOptions() const
    {
        mongocxx::options::client clientOpts;

        const SslSettings * const sslSettings = _connSettings->sslSettings();
        if (sslSettings->sslEnabled()) {
            mongocxx::options::tls tls;
            tls.allow_invalid_certificates(sslSettings->allowInvalidCertificates());
            if (!sslSettings->allowInvalidCertificates() && !sslSettings->caFile().empty())
                tls.ca_file(sslSettings->caFile());
            if (sslSettings->usePemFile()) {
                tls.pem_file(sslSettings->pemKeyFile());
                if (!sslSettings->pemPassPhrase().empty())
                    tls.pem_password(sslSettings->pemPassPhrase());
            }
            if (sslSettings->useAdvancedOptions() && !sslSettings->crlFile().empty())
                tls.crl_file(sslSettings->crlFile());
            clientOpts.tls_opts(tls);
        }

        return clientOpts;
    }

    ReplicaSet MongoWorker::getReplicaSetInfo() const
    {
        std::string setName;
        HostAndPort primary;
        std::vector<std::pair<std::string, bool>> membersAndHealths;
        std::string errorStr;

        if (!_client) {
            return ReplicaSet(setName, primary, membersAndHealths,
                              "No active connection to the replica set.");
        }

        try {
            namespace basic = bsoncxx::builder::basic;
            auto res = _client->database("admin").run_command(
                basic::make_document(basic::kvp("replSetGetStatus", 1)));

            const auto view = res.view();
            setName = BsonUtils::getField<std::string>(view, "set");

            const auto members = view["members"];
            if (members && members.type() == bsoncxx::type::k_array) {
                for (auto const& m : members.get_array().value) {
                    if (m.type() != bsoncxx::type::k_document)
                        continue;
                    const auto mv = m.get_document().view();
                    const std::string name = BsonUtils::getField<std::string>(mv, "name");
                    const bool healthy = mv["health"] && mv["health"].type() == bsoncxx::type::k_double
                        ? mv["health"].get_double().value > 0
                        : BsonUtils::getField<int>(mv, "health") > 0;
                    const std::string state = BsonUtils::getField<std::string>(mv, "stateStr");
                    membersAndHealths.push_back({ name, healthy });
                    if (state == "PRIMARY")
                        primary = HostAndPort(name);
                }
            }
        }
        catch (const std::exception &ex) {
            // Non-admin users lack clusterMonitor privilege for replSetGetStatus;
            // fall back to unprivileged { ismaster: 1 } to discover setName, primary, and hosts.
            try {
                namespace basic = bsoncxx::builder::basic;
                auto helloRes = _client->database("admin").run_command(
                    basic::make_document(basic::kvp("ismaster", 1)));
                const auto hv = helloRes.view();
                setName = BsonUtils::getField<std::string>(hv, "setName");
                const std::string primStr = BsonUtils::getField<std::string>(hv, "primary");
                if (!primStr.empty())
                    primary = HostAndPort(primStr);

                std::set<std::string> seen;
                auto appendHostsField = [&](const char *fieldName) {
                    const auto arr = hv[fieldName];
                    if (!arr || arr.type() != bsoncxx::type::k_array)
                        return;
                    for (auto const& item : arr.get_array().value) {
                        if (item.type() != bsoncxx::type::k_string)
                            continue;
                        const auto sv = item.get_string().value;
                        const std::string h(sv.data(), sv.size());
                        if (!h.empty() && seen.insert(h).second)
                            membersAndHealths.push_back({ h, true });
                    }
                };
                appendHostsField("hosts");
                appendHostsField("passives");
                appendHostsField("arbiters");

                if (primary.empty())
                    errorStr = ex.what();
            }
            catch (const std::exception &) {
                errorStr = ex.what();
            }
        }

        return ReplicaSet(setName, primary, membersAndHealths, errorStr);
    }

    std::string MongoWorker::connectAndGetReplicaSetName() const
    {
        std::string setName;

        std::string tlsQuery;
        if (_connSettings->sslSettings()->sslEnabled()) {
            tlsQuery = "&tls=true";
            if (_connSettings->sslSettings()->allowInvalidHostnames())
                tlsQuery += "&tlsAllowInvalidHostnames=true";
        }

        // Try connecting to the nodes one by one until getting replica set name.
        for (auto const& node : _connSettings->replicaSetSettings()->membersToHostAndPort())
        {
            try {
                mongocxx::client temp(
                    mongocxx::uri(
                        "mongodb://" + node.toString() +
                        "/?directConnection=true&serverSelectionTimeoutMS=3000" + tlsQuery),
                    buildClientOptions());

                namespace basic = bsoncxx::builder::basic;
                auto res = temp.database("admin").run_command(
                    basic::make_document(basic::kvp("ismaster", 1)));

                const auto elem = res.view()["setName"];
                if (elem && elem.type() == bsoncxx::type::k_string) {
                    const auto sv = elem.get_string().value;
                    setName.assign(sv.data(), sv.size());
                }
                if (!setName.empty()) // We get the information, finish the loop
                    break;
            }
            catch (const std::exception &) {
                // Try the next member
            }
        }

        return setName;
    }

    /**
     * @brief Send event to this MongoWorker
     */
    void MongoWorker::send(Event *event)
    {
        if (_isQuiting)
            return;

        AppRegistry::instance().bus()->send(this, event);
    }

    /**
     * @brief Send reply event to object 'receiver'
     */
    void MongoWorker::reply(QObject *receiver, Event *event)
    {
        if (_isQuiting)
            return;

        AppRegistry::instance().bus()->send(receiver, event);
    }

    void MongoWorker::pingDatabase(mongocxx::client *client) const
    {
        // Building { ping: 1 }
        namespace basic = bsoncxx::builder::basic;
        std::string const authBase = getAuthBase();
        std::string const targetDb = (authBase.empty() || authBase == "$external") ? "admin" : authBase;
        client->database(targetDb)
            .run_command(basic::make_document(basic::kvp("ping", 1)));
    }
}
