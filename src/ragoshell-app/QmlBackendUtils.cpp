#include "ragoshell-app/QmlBackend.h"

#include <cmath>
#include <functional>
#include <memory>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QPointer>
#include <QFile>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QTextStream>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/EventError.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/domain/HostAndPort.h"
#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/domain/MongoDatabase.h"
#include "ragoshell/core/domain/MongoCollection.h"
#include "ragoshell/core/domain/MongoCollectionInfo.h"
#include "ragoshell/core/domain/MongoNamespace.h"
#include "ragoshell/core/domain/MongoShell.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/mongodb/MongoWorker.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/mongodb/ReplicaSet.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AppLog.h"
#include "ragoshell/core/utils/AiQueryAssistant.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/HostKeyPrompter.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"
#include "ragoshell/core/utils/LanguageCatalog.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/CollectionTransfer.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell-app/CloudService.h"
#include "ragoshell-app/ServerTreeModel.h"
#include "ragoshell-app/ServerMonitorController.h"
#include "ragoshell-app/ShellController.h"
#include "ragoshell-app/BackgroundMonitor.h"
#include "ragoshell-app/LocalNetworkPermission.h"
#include "ragoshell/core/utils/LocalNetwork.h"
#include "ragoshell-app/LogModel.h"
#include "ragoshell-app/ScriptDraftModel.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell-app/QmlBackendInternal.h"

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

#ifdef Q_OS_IOS
namespace RagoShell
{
    bool iosShareFile(const QString &filePath);
}
#endif

namespace RagoShell
{
    using namespace QmlBackendDetail;

    double QmlBackend::serverVersion(const QString &uuid) const
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return 0.0;
        const QString exact = _serverVersionStrings.value(server).trimmed();
        if (!exact.isEmpty()) {
            const QStringList parts = exact.split(QLatin1Char('.'));
            if (parts.size() >= 2) {
                bool ok = false;
                const double parsed = (parts[0] + QLatin1Char('.') + parts[1]).toDouble(&ok);
                if (ok && parsed > 0.0)
                    return parsed;
            }
        }
        return std::round(static_cast<double>(server->version()) * 100.0) / 100.0;
    }

    QString QmlBackend::serverVersionString(const QString &uuid) const
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return QString();
        const QString exact = _serverVersionStrings.value(server);
        if (!exact.isEmpty())
            return exact;
        return server->version() > 0.0f
            ? QString::number(std::round(static_cast<double>(server->version()) * 10.0) / 10.0, 'f', 1)
            : QString();
    }

    QString QmlBackend::serverAddress(const QString &uuid) const
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord())
            return QString();
        return QtUtils::toQString(server->connectionRecord()->getReadableName());
    }

    bool QmlBackend::serverIsMongos(const QString &uuid) const
    {
        MongoServer *server = findServerByUuid(uuid);
        return server && server->getStorageEngineType() == "mongos";
    }

    QString QmlBackend::validateJson(const QString &jsonText, bool singleDocument) const
    {
        QString err;
        if (singleDocument) {
            bsoncxx::document::value doc = bsoncxx::from_json("{}");
            if (!MongoJsonUtils::parseSingleDocument(jsonText, doc, &err))
                return err;
            return QString();
        }

        std::vector<bsoncxx::document::value> docs;
        if (!MongoJsonUtils::parseDocuments(jsonText, docs, &err))
            return err;
        return QString();
    }

    QString QmlBackend::formatJson(const QString &jsonText) const
    {
        QString formatted;
        QString err;
        if (MongoJsonUtils::formatJsonDocuments(jsonText, formatted, err))
            return formatted;
        return QString();
    }

    QVariantList QmlBackend::keySequences(int standardKey, const QStringList &extra) const
    {
        QVariantList result;
        QList<QKeySequence> seen;
        for (const QKeySequence &binding :
             QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(standardKey))) {
            seen.append(binding);
            result.append(binding.toString(QKeySequence::PortableText));
        }
        for (const QString &text : extra) {
            const QKeySequence sequence(text, QKeySequence::PortableText);
            if (!sequence.isEmpty() && !seen.contains(sequence)) {
                seen.append(sequence);
                result.append(text);
            }
        }
        return result;
    }

    QVariant QmlBackend::uiState(const QString &key) const
    {
        if (!_persistUiState)
            return {};
        const QString brand = QString::fromUtf8(BrandInfo::productName);
        QSettings settings(brand, brand);
        const QVariant value = settings.value(QStringLiteral("QmlLayout/") + key);
        // The registry keeps booleans as text.
        if (value.typeId() == QMetaType::QString) {
            const QString text = value.toString();
            if (text == QLatin1String("true") || text == QLatin1String("false"))
                return text == QLatin1String("true");
        }
        return value;
    }

    void QmlBackend::setUiState(const QString &key, const QVariant &value)
    {
        if (!_persistUiState)
            return;
        const QString brand = QString::fromUtf8(BrandInfo::productName);
        QSettings settings(brand, brand);
        settings.setValue(QStringLiteral("QmlLayout/") + key, value);
    }

    QStringList QmlBackend::authMechanisms(const QString &saved) const
    {
        QStringList list = supportedAuthMechanisms();
        if (!saved.isEmpty() && !list.contains(saved))
            list.append(saved);
        return list;
    }

    bool QmlBackend::openLogFolder() const
    {
        const QString dir = appLogDir();
        QDir().mkpath(dir);

#if defined(Q_OS_IOS)
        // iOS sandboxes forbid QDesktopServices::openUrl(file://...); present the native
        // UIActivityViewController Share Sheet for ragoshell.log ("Save to Files", AirDrop, etc.).
        const QString filePath = AppLog::filePath();
        if (!QFile::exists(filePath)) {
            QFile touchFile(filePath);
            touchFile.open(QIODevice::WriteOnly | QIODevice::Append);
        }
        return iosShareFile(filePath);
#elif defined(Q_OS_ANDROID)
        // Android API 24+ forbids file:// intents across apps; share the recent log text via ACTION_SEND.
        QFile logFile(AppLog::filePath());
        QString payload;
        if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            constexpr qint64 kMaxShareBytes = 128 * 1024;
            if (logFile.size() > kMaxShareBytes)
                logFile.seek(logFile.size() - kMaxShareBytes);
            payload = QString::fromUtf8(logFile.readAll());
        }
        if (payload.isEmpty())
            payload = QStringLiteral("%1 diagnostic log is empty.")
                          .arg(QLatin1String(BrandInfo::productName));

        QJniObject context = QNativeInterface::QAndroidApplication::context();
        if (!context.isValid())
            return false;
        QJniObject actionSend = QJniObject::getStaticObjectField<jstring>("android/content/Intent", "ACTION_SEND");
        QJniObject extraText = QJniObject::getStaticObjectField<jstring>("android/content/Intent", "EXTRA_TEXT");
        QJniObject extraSubject = QJniObject::getStaticObjectField<jstring>("android/content/Intent", "EXTRA_SUBJECT");
        QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", actionSend.object<jstring>());
        if (!intent.isValid())
            return false;
        intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;",
                                QJniObject::fromString(QStringLiteral("text/plain")).object<jstring>());
        intent.callObjectMethod("putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
                                extraSubject.object<jstring>(),
                                QJniObject::fromString(
                                    QStringLiteral("%1 Diagnostic Log")
                                        .arg(QLatin1String(BrandInfo::productName))).object<jstring>());
        intent.callObjectMethod("putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
                                extraText.object<jstring>(),
                                QJniObject::fromString(payload).object<jstring>());
        QJniObject chooser = QJniObject::callStaticObjectMethod(
            "android/content/Intent", "createChooser",
            "(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;",
            intent.object<jobject>(),
            QJniObject::fromString(tr("Share Log")).object<jobject>());
        if (!chooser.isValid())
            return false;
        const jint flagNewTask = QJniObject::getStaticField<jint>("android/content/Intent", "FLAG_ACTIVITY_NEW_TASK");
        chooser.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", flagNewTask);
        context.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", chooser.object<jobject>());
        QJniEnvironment env;
        if (env.checkAndClearExceptions())
            return false;
        return true;
#else
        return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
    }

    QString QmlBackend::logFilePath() const
    {
        return AppLog::filePath();
    }

    void QmlBackend::copyToClipboard(const QString &text) const
    {
        if (QClipboard *cb = QGuiApplication::clipboard())
            cb->setText(text);
    }

    QVariantMap QmlBackend::pasteApiKey() const
    {
        QClipboard *cb = QGuiApplication::clipboard();
        QString key;
        switch (AiQueryAssistant::checkPastedKey(cb ? cb->text() : QString(), &key)) {
        case AiQueryAssistant::PastedKey::Ok:
            return {{QStringLiteral("key"), key}};
        case AiQueryAssistant::PastedKey::Empty:
            return {{QStringLiteral("error"), tr("The clipboard has no text.")}};
        case AiQueryAssistant::PastedKey::MultiLine:
            return {{QStringLiteral("error"), tr("The clipboard holds more than one line. Copy only the key.")}};
        case AiQueryAssistant::PastedKey::TooLong:
            return {{QStringLiteral("error"),
                     tr("The clipboard text is too long for an API key (over %1 characters).")
                         .arg(AiQueryAssistant::kMaxApiKeyLength)}};
        case AiQueryAssistant::PastedKey::InvalidCharacters:
            break;
        }
        return {{QStringLiteral("error"),
                 tr("The clipboard text has spaces or characters an API key does not contain.")}};
    }

    bool QmlBackend::clearClipboardIfEquals(const QString &text) const
    {
        QClipboard *cb = QGuiApplication::clipboard();
        if (!cb || text.isEmpty() || cb->text().trimmed() != text)
            return false;
        cb->clear();
        return true;
    }

    QString QmlBackend::insertDocumentsJson(const QString &uuid, const QString &dbName,
                                            const QString &collName, const QString &jsonText)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->worker() || dbName.isEmpty() || collName.isEmpty())
            return tr("Server or target collection is not available.");

        std::vector<bsoncxx::document::value> docs;
        QString err;
        if (!MongoJsonUtils::parseDocuments(jsonText, docs, &err))
            return err;

        const MongoNamespace ns(QtUtils::toStdString(dbName), QtUtils::toStdString(collName));
        if (_pendingExplorerInserts <= 0) {
            _explorerInsertBatchTotal = 0;
            _explorerInsertBatchHadError = false;
            _explorerInsertLastError.clear();
        }
        _pendingExplorerInserts += static_cast<int>(docs.size());
        _explorerInsertBatchTotal += static_cast<int>(docs.size());
        _explorerInsertDb = dbName;
        _explorerInsertColl = collName;

        for (const auto &doc : docs) {
            _bus->send(server->worker(),
                       new InsertDocumentRequest(this, doc.view(), ns, /*overwrite=*/false));
        }
        return QString();
    }

    QString QmlBackend::firstConnectedUuid() const
    {
        return _serverUuids.isEmpty() ? QString()
                                      : _serverUuids.constBegin().value();
    }

    QString QmlBackend::urlToLocalFile(const QUrl &url) const
    {
        return url.isLocalFile() ? url.toLocalFile() : url.toString();
    }

    QString QmlBackend::importCredentialFile(const QUrl &url)
    {
        if (url.isEmpty())
            return QString();
        const QString path = urlToLocalFile(url);
        ensureLegacyStoragePermission(path, false);
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        const bool copyToSandbox = true;
#else
        const bool copyToSandbox = !url.isLocalFile()
                                   && url.scheme().compare(QLatin1String("content"), Qt::CaseInsensitive) == 0;
#endif
        if (!copyToSandbox)
            return path;

        QString err;
        const QString imported = importCredentialFileToDir(url, appCredentialsDir(), &err);
        if (imported.isEmpty()) {
            LOG_MSG(QStringLiteral("Cannot import credential file %1: %2").arg(path, err),
                    LogSeverity::Error);
            setStatus(err);
        }
        return imported;
    }

    QString QmlBackend::readFile(const QUrl &url) const
    {
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        ensureLegacyStoragePermission(path, false);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        QTextStream in(&file);
        return in.readAll();
    }

    QString QmlBackend::licenseDocument(const QString &name) const
    {
        QString resPath;
        if (name == QLatin1String("notices"))
            resPath = QStringLiteral(":/licenses/THIRD-PARTY-NOTICES.txt");
        else if (name == QLatin1String("gplv3"))
            resPath = QStringLiteral(":/licenses/LICENSE");
        else if (name == QLatin1String("lgplv3"))
            resPath = QStringLiteral(":/licenses/LICENSE.LGPLv3");
        else if (name == QLatin1String("copyright"))
            resPath = QStringLiteral(":/licenses/COPYRIGHT");
        else
            return QString();

        QFile file(resPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        return QString::fromUtf8(file.readAll());
    }

    bool QmlBackend::saveFile(const QUrl &url, const QString &text)
    {
        // QSaveFile writes to a temp file and renames on commit() on desktop.
        // setDirectWriteFallback(true) allows Android SAF content:// virtual files,
        // and on iOS external security-scoped URLs (where sibling temp creation fails
        // with EPERM) we fall back cleanly to QFile through QDarwinSecurityScopedFileEngine.
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        ensureLegacyStoragePermission(path, true);
        QSaveFile file(path);
        file.setDirectWriteFallback(true);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << text;
            out.flush();
            if (out.status() == QTextStream::Ok && file.commit())
                return true;
        }
#ifdef Q_OS_IOS
        {
            QFile directFile(path);
            if (directFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream out(&directFile);
                out << text;
                out.flush();
                directFile.close();
                if (out.status() == QTextStream::Ok && directFile.error() == QFileDevice::NoError)
                    return true;
            }
        }
#endif
        LOG_MSG(QStringLiteral("Cannot save %1: %2").arg(path, file.errorString()),
                LogSeverity::Error);
        return false;
    }

    bool QmlBackend::saveFileAs(const QString &fileNameHint, const QString &text)
    {
#ifdef Q_OS_WASM
        // Browser "save" = a download of the content (docs/WebStorage.md §3).
        QFileDialog::saveFileContent(text.toUtf8(), fileNameHint);
        return true;
#else
        Q_UNUSED(fileNameHint);
        Q_UNUSED(text);
        return false;
#endif
    }

    bool QmlBackend::downloadFile(const QString &path)
    {
#ifdef Q_OS_WASM
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            LOG_MSG(QStringLiteral("Cannot download %1: %2").arg(path, file.errorString()),
                    LogSeverity::Error);
            return false;
        }
        QFileDialog::saveFileContent(file.readAll(), QFileInfo(path).fileName());
        return true;
#else
        Q_UNUSED(path);
        return false;
#endif
    }


    void QmlBackend::testConnection(const QVariantMap &map)
    {
        ConnectionSettings probe(true);
        probe.fromVariant(map);
        whenLocalNetworkAllowed(networkHosts(probe), [self = QPointer<QmlBackend>(this), map] {
            if (self)
                self->testConnectionNow(map);
        });
    }

    void QmlBackend::testConnectionNow(const QVariantMap &map)
    {
        _lastTestMap = map;
        std::unique_ptr<ConnectionSettings> conn(new ConnectionSettings(true));
        conn->fromVariant(map);
        // Hosted channel (docs/HostedGateway.md §5): same JWT injection as
        // connectToServer — without it the probe's hello is always denied.
        if (conn->transportKind() == "hosted") {
            CloudService *cloud = QmlSingletonInstance<CloudService>::get();
            const QString jwt = cloud->entitlementJwt();
            if (jwt.isEmpty()) {
                emit testResult(false, cloud->hostedTokenMissingMessage());
                return;
            }
            conn->setTransportToken(jwt.toStdString());
        }

        QVariantList initialSteps;
        auto addStep = [&initialSteps](const QString &label, const QString &status, const QString &detail) {
            initialSteps.append(QVariantMap{
                {QStringLiteral("label"), label},
                {QStringLiteral("status"), status},
                {QStringLiteral("detail"), detail}
            });
        };

        const QString targetDesc = conn->isReplicaSet()
            ? tr("Replica Set (%n seed node(s))", "", conn->replicaSetSettings()->members().size())
            : QtUtils::toQString(conn->getFullAddress());
        addStep(tr("Target Endpoint"), QStringLiteral("ok"), targetDesc);

        if (conn->sshSettings() && conn->sshSettings()->enabled()) {
            addStep(tr("SSH Tunnel"), QStringLiteral("pending"),
                    QStringLiteral("%1:%2").arg(QtUtils::toQString(conn->sshSettings()->host()))
                                           .arg(conn->sshSettings()->port()));
        }
        if (conn->sslSettings() && conn->sslSettings()->sslEnabled()) {
            addStep(tr("TLS / SSL"), QStringLiteral("pending"),
                    conn->sslSettings()->allowInvalidCertificates()
                        ? tr("TLS enabled (allow self-signed)")
                        : tr("TLS enabled (verified CA)"));
        }
        addStep(tr("MongoDB Handshake"), QStringLiteral("pending"),
                tr("Connecting…"));
        if (conn->hasEnabledPrimaryCredential()) {
            addStep(tr("Authentication"), QStringLiteral("pending"),
                    QStringLiteral("%1 @ %2 (%3)")
                        .arg(QtUtils::toQString(conn->primaryCredential()->userName()),
                             QtUtils::toQString(conn->primaryCredential()->databaseName()),
                             QtUtils::toQString(conn->primaryCredential()->mechanism())));
        }
        emit testDiagnosticSteps(initialSteps);

        if (!_app->openServer(conn.release(), ConnectionTest)) {
            emit testResult(false, tr("Connection test could not be started"));
            return;
        }
        _testServerHandle = _app->getLastServerHandle();
    }

    QVariantMap QmlBackend::parseMongoUri(const QString &uriText) const
    {
        QVariantMap out;
        QString clean = uriText.trimmed();
        clean.remove(QLatin1Char(' '));

        if (clean.isEmpty()) {
            out.insert(QStringLiteral("ok"), false);
            out.insert(QStringLiteral("error"),
                       tr("Please enter a MongoDB URI (e.g. mongodb://host1:27017,host2:27018/?replicaSet=rs0)."));
            return out;
        }

        try {
            mongocxx::uri mongoUri{clean.toStdString()};
            const auto opts = mongoUri.options();

            auto uriStringOpt = [&opts](const char *key) -> QString {
                const auto el = opts[key];
                if (el && el.type() == bsoncxx::type::k_string)
                    return QString::fromStdString(std::string(el.get_string().value));
                return QString();
            };

            auto uriBoolOpt = [&opts](const char *key) -> bool {
                const auto el = opts[key];
                if (!el)
                    return false;
                if (el.type() == bsoncxx::type::k_bool)
                    return el.get_bool().value;
                if (el.type() == bsoncxx::type::k_int32)
                    return el.get_int32().value != 0;
                if (el.type() == bsoncxx::type::k_string) {
                    const QString v = QString::fromStdString(std::string(el.get_string().value)).trimmed();
                    return v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ||
                           v == QLatin1String("1");
                }
                return false;
            };

            // mongodb+srv: hosts are resolved later from DNS, so hosts() is
            // empty here; take the seedlist hostname from the URI text.
            const bool isSrv = clean.startsWith(QLatin1String("mongodb+srv://"), Qt::CaseInsensitive);
            QString srvHost;
            if (isSrv) {
                srvHost = clean.mid(int(qstrlen("mongodb+srv://")));
                const int at = srvHost.lastIndexOf(QLatin1Char('@'));
                if (at >= 0)
                    srvHost = srvHost.mid(at + 1);
                const int end = srvHost.indexOf(QRegularExpression(QStringLiteral("[/?]")));
                if (end >= 0)
                    srvHost.truncate(end);
            }

            QStringList hosts;
            QString firstHost = QStringLiteral("localhost");
            int firstPort = 27017;
            bool isFirst = true;

            for (const auto &hp : mongoUri.hosts()) {
                QString host = QString::fromStdString(hp.name);
                if (host.endsWith(QLatin1Char('.')))
                    host.chop(1);
                const int port = hp.port > 0 ? static_cast<int>(hp.port) : 27017;
                if (isFirst) {
                    firstHost = host;
                    firstPort = port;
                    isFirst = false;
                }
                hosts.append(QStringLiteral("%1:%2").arg(host).arg(port));
            }
            if (isSrv) {
                hosts = QStringList{srvHost};
                firstHost = srvHost;
                firstPort = 27017;
            }
            if (hosts.isEmpty())
                hosts.append(QStringLiteral("localhost:27017"));

            const QString setName = QString::fromStdString(std::string(mongoUri.replica_set()));
            const bool isReplicaSet = !isSrv && (!setName.isEmpty() || hosts.size() > 1);
            const QString readPrefStr = uriStringOpt("readPreference");
            // Index = ReplicaSetSettings::ReadPreference value.
            static const QStringList readPrefNames = {
                QStringLiteral("primary"), QStringLiteral("primaryPreferred"),
                QStringLiteral("secondary"), QStringLiteral("secondaryPreferred"),
                QStringLiteral("nearest")};
            int readPref = 0;
            for (int i = 0; i < readPrefNames.size(); ++i) {
                if (readPrefStr.compare(readPrefNames.at(i), Qt::CaseInsensitive) == 0)
                    readPref = i;
            }

            const QString user = QString::fromStdString(std::string(mongoUri.username()));
            const QString pwd = QString::fromStdString(std::string(mongoUri.password()));
            const QString defaultDb = QString::fromStdString(std::string(mongoUri.database()));
            QString authDb = QString::fromStdString(std::string(mongoUri.auth_source()));
            if (authDb.isEmpty())
                authDb = !defaultDb.isEmpty() ? defaultDb : QStringLiteral("admin");

            QString authMech = uriStringOpt("authMechanism").toUpper();
            if (authMech.isEmpty())
                authMech = QStringLiteral("DEFAULT");
            const bool authEnabled = !user.isEmpty() || authMech == QStringLiteral("MONGODB-X509");

            // SRV implies TLS unless the URI turns it off explicitly.
            const bool tlsExplicit = opts["tls"] || opts["ssl"];
            const bool tlsEnabled = mongoUri.tls() || (isSrv && !tlsExplicit);
            QString caFile = uriStringOpt("tlsCAFile");
            if (caFile.isEmpty())
                caFile = uriStringOpt("sslCAFile");
            QString pemFile = uriStringOpt("tlsCertificateKeyFile");
            if (pemFile.isEmpty())
                pemFile = uriStringOpt("sslPEMKeyFile");
            QString pemPass = uriStringOpt("tlsCertificateKeyFilePassword");
            if (pemPass.isEmpty())
                pemPass = uriStringOpt("sslPEMKeyPassword");

            const bool allowInvalidCerts = uriBoolOpt("tlsAllowInvalidCertificates") ||
                                           uriBoolOpt("sslAllowInvalidCertificates") ||
                                           uriBoolOpt("tlsInsecure");
            const bool allowInvalidHosts = uriBoolOpt("tlsAllowInvalidHostnames") ||
                                           uriBoolOpt("sslAllowInvalidHostnames") ||
                                           uriBoolOpt("tlsInsecure");

            out.insert(QStringLiteral("ok"), true);
            out.insert(QStringLiteral("isSrv"), isSrv);
            out.insert(QStringLiteral("isReplicaSet"), isReplicaSet);
            out.insert(QStringLiteral("serverHost"), firstHost);
            out.insert(QStringLiteral("serverPort"), firstPort);
            out.insert(QStringLiteral("hosts"), hosts);
            out.insert(QStringLiteral("setName"), setName);
            out.insert(QStringLiteral("readPreference"), readPref);
            out.insert(QStringLiteral("defaultDatabase"), defaultDb);
            out.insert(QStringLiteral("suggestedName"),
                       isSrv ? srvHost
                       : !setName.isEmpty()
                           ? setName
                           : QStringLiteral("%1:%2").arg(firstHost).arg(firstPort));

            out.insert(QStringLiteral("authEnabled"), authEnabled);
            out.insert(QStringLiteral("userName"), user);
            out.insert(QStringLiteral("userPassword"), pwd);
            out.insert(QStringLiteral("authDatabase"), authDb);
            out.insert(QStringLiteral("authMechanism"), authMech);

            out.insert(QStringLiteral("sslEnabled"), tlsEnabled);
            out.insert(QStringLiteral("caFile"), caFile);
            out.insert(QStringLiteral("pemKeyFile"), pemFile);
            out.insert(QStringLiteral("pemPassPhrase"), pemPass);
            out.insert(QStringLiteral("allowInvalidCertificates"), allowInvalidCerts);
            out.insert(QStringLiteral("allowInvalidHostnames"), allowInvalidHosts);
        } catch (const std::exception &ex) {
            out.insert(QStringLiteral("ok"), false);
            out.insert(QStringLiteral("error"),
                       tr("Invalid MongoDB URI: %1").arg(QString::fromUtf8(ex.what())));
        }

        return out;
    }


    void QmlBackend::openMonitor(const QString &uuid)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord()) {
            emit connectionError(uuid, tr("Server is not connected"));
            return;
        }

        for (int i = 0; i < _monitors.size(); ++i) {
            auto *existing = qobject_cast<ServerMonitorController *>(
                _monitors.at(i).value<QObject *>());
            if (existing && existing->serverUuid() == uuid) {
                _tabs->setCurrentIndex(_tabs->indexOf(existing));
                emit monitorActivated(i);
                return;
            }
        }

        if (server->connectionRecord()->hasTransport()) {
            emit connectionError(uuid, tr("Server monitoring is not yet supported "
                                        "over gateway/demo connection channels."));
            return;
        }

        const QString readable = QtUtils::toQString(server->connectionRecord()->getReadableName());
        const QString title = tr("Monitor: %1").arg(
            readable.isEmpty() ? tr("Server") : readable);

        std::unique_ptr<ConnectionSettings> monConn(server->connectionRecord()->clone());
        auto *ctrl = new ServerMonitorController(uuid, title, std::move(monConn), this);
        _monitors.append(QVariant::fromValue<QObject *>(ctrl));
        _monitorModel->append(ctrl);
        _tabs->addTab(QStringLiteral("monitor"), ctrl, true);
        emit monitorsChanged();
        emit monitorOpened(ctrl);
    }

    void QmlBackend::openMemberMonitor(const QString &uuid, const QString &memberAddress)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord() || memberAddress.trimmed().isEmpty()) {
            emit connectionError(uuid, tr("Server is not connected"));
            return;
        }

        const QString key = uuid + QStringLiteral("@") + memberAddress.trimmed();
        for (int i = 0; i < _monitors.size(); ++i) {
            auto *existing = qobject_cast<ServerMonitorController *>(
                _monitors.at(i).value<QObject *>());
            if (existing && existing->serverUuid() == key) {
                _tabs->setCurrentIndex(_tabs->indexOf(existing));
                emit monitorActivated(i);
                return;
            }
        }

        HostAndPort hp(memberAddress.trimmed().toStdString());
        if (server->connectionRecord()->hasTransport()) {
            emit connectionError(uuid, tr("Server monitoring is not yet supported "
                                        "over gateway/demo connection channels."));
            return;
        }
        std::unique_ptr<ConnectionSettings> memberConn(server->connectionRecord()->clone());
        memberConn->setConnectionName(
            hp.toString() + " [member of " + memberConn->connectionName() + "]");
        memberConn->setServerHost(hp.host());
        memberConn->setServerPort(hp.port());
        memberConn->setReplicaSet(false);
        memberConn->replicaSetSettings()->deleteAllMembers();

        const QString title = tr("Monitor: %1").arg(
            QString::fromStdString(hp.toString()));
        auto *ctrl = new ServerMonitorController(key, title, std::move(memberConn), this);
        _monitors.append(QVariant::fromValue<QObject *>(ctrl));
        _monitorModel->append(ctrl);
        _tabs->addTab(QStringLiteral("monitor"), ctrl, true);
        emit monitorsChanged();
        emit monitorOpened(ctrl);
    }

    void QmlBackend::closeMonitor(int index)
    {
        if (index < 0 || index >= _monitors.size())
            return;
        auto *ctrl = qobject_cast<ServerMonitorController *>(
            _monitors.at(index).value<QObject *>());
        _monitors.removeAt(index);
        _monitorModel->removeAt(index);
        _tabs->removeObject(ctrl);
        emit monitorsChanged();
        if (ctrl)
            ctrl->deleteLater();
    }

}
