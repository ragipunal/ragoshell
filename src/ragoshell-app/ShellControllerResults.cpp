#include "ragoshell-app/ShellController.h"

#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QVariantMap>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/utils/AggregatePaging.h"
#include "ragoshell/core/utils/FieldCompletion.h"
#include "ragoshell/core/utils/ShellCompletion.h"
#include "ragoshell-app/ScriptDraftModel.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/EventError.h"
#include "ragoshell/core/bson/BsonTableModel.h"
#include "ragoshell/core/bson/BsonTreeModel.h"
#include "ragoshell/core/domain/MongoDatabase.h"
#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/domain/MongoNamespace.h"
#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/domain/MongoShell.h"
#include "ragoshell/core/domain/MongoUtils.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/mongodb/MongoWorker.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/settings/QueryHistoryStore.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/ExplainPlan.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"
#include "ragoshell-app/QueryHistoryModel.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{
    namespace
    {
        double readNumericField(bsoncxx::document::view doc, const char *key, double fallback = 0.0)
        {
            const auto el = doc[key];
            if (!el)
                return fallback;
            switch (el.type()) {
            case bsoncxx::type::k_int32:  return static_cast<double>(el.get_int32().value);
            case bsoncxx::type::k_int64:  return static_cast<double>(el.get_int64().value);
            case bsoncxx::type::k_double: return el.get_double().value;
            default: return fallback;
            }
        }

    }

    // ---- Document inspection, Copy & Native C++ CRUD -----------------------

    QString ShellController::documentJsonAt(int docIndex) const
    {
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size())
            return QString();
        auto *settings = AppRegistry::instance().settingsManager();
        const std::string json = BsonUtils::jsonString(
            _currentDocs[docIndex]->bsonObj(), TenGen, 1,
            settings ? settings->uuidEncoding() : DefaultEncoding,
            settings ? settings->timeZone() : Utc);
        return QtUtils::toQString(json);
    }

    QString ShellController::documentJsonForView(int docIndex) const
    {
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size())
            return QString();
        static constexpr size_t kViewFieldPreviewBytes = 16384;
        auto *settings = AppRegistry::instance().settingsManager();
        const std::string json = BsonUtils::jsonString(
            _currentDocs[docIndex]->bsonObj(), TenGen, 1,
            settings ? settings->uuidEncoding() : DefaultEncoding,
            settings ? settings->timeZone() : Utc,
            /*isArray=*/false,
            kViewFieldPreviewBytes);
        return QtUtils::toQString(json);
    }

    int ShellController::documentByteSizeAt(int docIndex) const
    {
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size())
            return 0;
        return static_cast<int>(_currentDocs[docIndex]->bsonObj().length());
    }

    QString ShellController::documentIdPreviewAt(int docIndex) const
    {
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size())
            return QString();
        const auto view = _currentDocs[docIndex]->bsonObj();
        const auto idElem = view["_id"];
        if (!idElem)
            return QStringLiteral("(no _id)");
        auto *settings = AppRegistry::instance().settingsManager();
        const std::string valStr = BsonUtils::jsonString(
            idElem, TenGen, /*includeFieldNames=*/false, /*pretty=*/0,
            settings ? settings->uuidEncoding() : DefaultEncoding,
            settings ? settings->timeZone() : Utc);
        return QtUtils::toQString(valStr).trimmed();
    }

    QVariantMap ShellController::duplicateTemplateAt(int docIndex) const
    {
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size()) {
            return QVariantMap{
                {QStringLiteral("json"), QStringLiteral("{\n    \n}")},
                {QStringLiteral("caretOffset"), 6}
            };
        }
        auto *settings = AppRegistry::instance().settingsManager();
        const auto tpl = MongoJsonUtils::buildDuplicateDocumentTemplate(
            _currentDocs[docIndex]->bsonObj(),
            settings ? settings->uuidEncoding() : DefaultEncoding,
            settings ? settings->timeZone() : Utc);
        return QVariantMap{
            {QStringLiteral("json"), tpl.json},
            {QStringLiteral("caretOffset"), tpl.caretUtf16Offset}
        };
    }

    QString ShellController::duplicateTemplateJsonAt(int docIndex) const
    {
        return duplicateTemplateAt(docIndex).value(QStringLiteral("json")).toString();
    }

    bool ShellController::isEditIdChanged(int docIndex, const QString &jsonText) const
    {
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size())
            return false;
        const auto origId = _currentDocs[docIndex]->bsonObj()["_id"];
        if (!origId)
            return false;

        bsoncxx::document::value updatedDoc(bsoncxx::builder::basic::make_document());
        QString err;
        if (!MongoJsonUtils::parseSingleDocument(jsonText, updatedDoc, err))
            return false;

        const auto newId = updatedDoc.view()["_id"];
        if (!newId)
            return false;
        return origId.get_value() != newId.get_value();
    }

    QString ShellController::validateJson(const QString &jsonText, bool singleDoc) const
    {
        QString err;
        if (singleDoc) {
            bsoncxx::document::value doc(bsoncxx::builder::basic::make_document());
            MongoJsonUtils::parseSingleDocument(jsonText, doc, err);
        } else {
            std::vector<bsoncxx::document::value> docs;
            MongoJsonUtils::parseDocuments(jsonText, docs, err);
        }
        return err; // empty string == valid
    }

    QString ShellController::formatJson(const QString &jsonText) const
    {
        QString formatted;
        QString err;
        if (MongoJsonUtils::formatJsonDocuments(jsonText, formatted, err))
            return formatted;
        return QString();
    }

    QStringList ShellController::detectDestructiveOperations(const QString &scriptText) const
    {
        return MongoJsonUtils::detectDestructiveScriptOperations(scriptText);
    }

    void ShellController::copyToClipboard(const QString &text) const
    {
        if (auto *cb = QGuiApplication::clipboard())
            cb->setText(text);
    }

    QString ShellController::objectIdTimestamp(const QString &oidText) const
    {
        return MongoJsonUtils::extractIsoDateFromObjectId(oidText);
    }

    bool ShellController::insertDocumentsFromJson(const QString &jsonText, QString *outError)
    {
        const QString err = submitInsertJson(jsonText);
        if (outError)
            *outError = err;
        return err.isEmpty();
    }

    QString ShellController::submitInsertJson(const QString &jsonText)
    {
        if (!canInsertDocuments() || !_shell || !_shell->server() || !_shell->server()->worker())
            return tr("No active editable collection in this tab.");

        std::vector<bsoncxx::document::value> docs;
        QString err;
        if (!MongoJsonUtils::parseDocuments(jsonText, docs, err))
            return err;

        const MongoNamespace ns(QtUtils::toStdString(_activeDatabase),
                                QtUtils::toStdString(_activeCollection));
        if (_pendingCrudOps <= 0)
            _crudBatchHadError = false;
        _pendingCrudOps += static_cast<int>(docs.size());
        for (const auto &doc : docs) {
            _bus->send(_shell->server()->worker(),
                       new InsertDocumentRequest(this, doc.view(), ns, /*overwrite=*/false));
        }
        return QString();
    }

    QString ShellController::submitEditJson(int docIndex, const QString &jsonText)
    {
        if (!canModifyDocuments() || !_shell || !_shell->server() || !_shell->server()->worker())
            return tr("No active editable collection in this tab.");
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size())
            return tr("Selected document index is out of range.");

        bsoncxx::document::value updatedDoc(bsoncxx::builder::basic::make_document());
        QString err;
        if (!MongoJsonUtils::parseSingleDocument(jsonText, updatedDoc, err))
            return err;

        const auto origView = _currentDocs[docIndex]->bsonObj();
        const auto origId = origView["_id"];
        if (!origId)
            return tr("Cannot edit a result document that has no '_id' field.");
        const auto newId = updatedDoc.view()["_id"];
        if (!newId)
            return tr("The '_id' field cannot be removed when editing an existing document.");

        // When '_id' is changed during Edit, treat the operation as an Insert (overwrite=false)
        // so the original document stays intact and any existing document with newId is never overwritten.
        const bool idChanged = (origId.get_value() != newId.get_value());

        const MongoNamespace ns(QtUtils::toStdString(_activeDatabase),
                                QtUtils::toStdString(_activeCollection));
        if (_pendingCrudOps <= 0)
            _crudBatchHadError = false;
        _pendingCrudOps += 1;
        _bus->send(_shell->server()->worker(),
                   new InsertDocumentRequest(this, updatedDoc.view(), ns, /*overwrite=*/!idChanged));
        return QString();
    }

    QString ShellController::updateScalarCell(int docIndex,
                                              const QString &fieldPath,
                                              const QString &newValue,
                                              const QString &typeName)
    {
        auto failWith = [this](const QString &msg) -> QString {
            _errorText = msg;
            emit errorChanged();
            emit crudCompleted(false, _errorText);
            return msg;
        };

        if (!canModifyDocuments() || !_shell || !_shell->server() || !_shell->server()->worker())
            return failWith(tr("This result view is read-only (no target collection)."));
        if (docIndex < 0 || static_cast<size_t>(docIndex) >= _currentDocs.size())
            return failWith(tr("Selected document is out of range."));

        const auto origView = _currentDocs[docIndex]->bsonObj();
        if (!origView["_id"])
            return failWith(tr("Cannot edit document without an '_id' field."));

        bsoncxx::document::value updatedDoc(bsoncxx::builder::basic::make_document());
        QString err;
        if (!MongoJsonUtils::updateScalarFieldAtPath(origView, fieldPath, newValue,
                                                     typeName, updatedDoc, err)) {
            return failWith(err);
        }

        const MongoNamespace ns(QtUtils::toStdString(_activeDatabase),
                                QtUtils::toStdString(_activeCollection));
        if (_pendingCrudOps <= 0)
            _crudBatchHadError = false;
        _pendingCrudOps += 1;
        _bus->send(_shell->server()->worker(),
                   new InsertDocumentRequest(this, updatedDoc.view(), ns, /*overwrite=*/true));
        return QString();
    }

    QString ShellController::deleteDocumentsAt(const QVariantList &docIndices)
    {
        if (!canDeleteDocuments() || !_shell || !_shell->server() || !_shell->server()->worker())
            return tr("This result set is read-only and cannot be deleted.");
        if (docIndices.isEmpty())
            return tr("No documents selected.");

        using bsoncxx::builder::basic::kvp;
        std::vector<bsoncxx::document::value> queries;
        QSet<int> seen;
        for (const QVariant &v : docIndices) {
            const int idx = v.toInt();
            if (seen.contains(idx))
                continue;
            seen.insert(idx);
            if (idx < 0 || static_cast<size_t>(idx) >= _currentDocs.size())
                continue;
            const auto idElem = _currentDocs[idx]->bsonObj()["_id"];
            if (!idElem)
                return tr("Document #%1 has no '_id' field and cannot be deleted safely.").arg(idx + 1);
            bsoncxx::builder::basic::document q;
            q.append(kvp("_id", idElem.get_value()));
            queries.push_back(q.extract());
        }

        if (queries.empty())
            return tr("No valid documents selected for deletion.");

        const MongoNamespace ns(QtUtils::toStdString(_activeDatabase),
                                QtUtils::toStdString(_activeCollection));
        const RemoveDocumentCount mode = queries.size() > 1
            ? RemoveDocumentCount::MULTI
            : RemoveDocumentCount::ONE;

        if (_pendingCrudOps <= 0)
            _crudBatchHadError = false;
        _pendingCrudOps += static_cast<int>(queries.size());
        for (size_t i = 0; i < queries.size(); ++i) {
            _bus->send(_shell->server()->worker(),
                       new RemoveDocumentRequest(this, queries[i].view(), ns, mode, static_cast<int>(i)));
        }
        return QString();
    }

    // ---- Event handlers ----------------------------------------------------

    void ShellController::handle(InsertDocumentResponse *event)
    {
        if (_pendingCrudOps > 0)
            --_pendingCrudOps;

        if (event->isError()) {
            _crudBatchHadError = true;
            _errorText = QtUtils::toQString(event->error().errorMessage());
            emit errorChanged();
        }

        if (_pendingCrudOps <= 0) {
            _pendingCrudOps = 0;
            const bool hadErr = _crudBatchHadError;
            _crudBatchHadError = false;
            if (hadErr) {
                emit crudCompleted(false, _errorText);
            } else {
                if (!_errorText.isEmpty()) {
                    _errorText.clear();
                    emit errorChanged();
                }
                emit crudCompleted(true, tr("Document saved."));
            }
            reloadCurrentPage();
        }
    }

    void ShellController::handle(RemoveDocumentResponse *event)
    {
        if (_pendingCrudOps > 0)
            --_pendingCrudOps;

        if (event->isError()) {
            _crudBatchHadError = true;
            _errorText = QtUtils::toQString(event->error().errorMessage());
            emit errorChanged();
        }

        if (_pendingCrudOps <= 0) {
            _pendingCrudOps = 0;
            const bool hadErr = _crudBatchHadError;
            _crudBatchHadError = false;
            if (hadErr) {
                emit crudCompleted(false, _errorText);
            } else {
                if (!_errorText.isEmpty()) {
                    _errorText.clear();
                    emit errorChanged();
                }
                emit crudCompleted(true, tr("Document(s) deleted."));
            }
            reloadCurrentPage();
        }
    }

    void ShellController::handle(AutocompleteResponse *event)
    {
        if (event->sender() != _shell)
            return;
        if (event->isError()) {
            _fetchingLiveCollections = false;
            return;
        }
        if (_fetchingLiveCollections && event->prefix == "db.") {
            _fetchingLiveCollections = false;
            QStringList parsed;
            QSet<QString> seen;
            const QString getCollPrefix = QStringLiteral("db.getCollection('");
            const QString getCollSuffix = QStringLiteral("')");
            for (const QString &entry : event->list) {
                QString coll;
                if (entry.startsWith(getCollPrefix) && entry.endsWith(getCollSuffix)) {
                    const QString inner = entry.mid(getCollPrefix.size(),
                                                    entry.size() - getCollPrefix.size() - getCollSuffix.size());
                    coll.reserve(inner.size());
                    for (int i = 0; i < inner.size(); ++i) {
                        if (inner[i] == QLatin1Char('\\') && i + 1 < inner.size())
                            coll += inner[++i];
                        else
                            coll += inner[i];
                    }
                } else if (entry.startsWith(QLatin1String("db.")) && !entry.contains(QLatin1Char('('))) {
                    const QString rest = entry.mid(3);
                    if (!rest.isEmpty() && rest != QLatin1String("system")
                        && !rest.startsWith(QLatin1String("system."))) {
                        coll = rest;
                    }
                }
                if (!coll.isEmpty() && !seen.contains(coll)) {
                    seen.insert(coll);
                    parsed << coll;
                }
            }
            _cachedCollections = parsed;
            _cachedCollectionsDb = _fetchingCollectionsForDb;
            emit liveCollectionsUpdated();
            return;
        }
        _autocompleteList = event->list;
        _autocompleteInserts.clear();   // the shell's items insert themselves
        _autocompleteDetails.clear();
        emit autocompleteReady();
    }

    void ShellController::handle(ScriptExecutingEvent *event)
    {
        if (event->sender() != _shell)
            return;
        const bool wasCancelled = _cancelled;
        _cancelled = false;
        if (!_busy) {
            _busy = true;
            emit busyChanged();
        }
        if (wasCancelled)
            emit cancelledChanged();
        emit statusMessage(tr("Executing..."));
    }

    void ShellController::handle(ScriptExecutedEvent *event)
    {
        if (event->sender() != _shell)
            return;

        if (event->opGen() > 0 && _shell && event->opGen() < _shell->lastDispatchedOpGen())
            return;

        if ((event->opGen() > 0 && event->opGen() <= _cancelledUpToOpGen) || event->cancelled()) {
            _historyPending = false;   // cancelled runs are not history
            const bool wasBusy = _busy;
            const bool wasCancelled = _cancelled;
            _busy = false;
            _cancelled = true;
            if (wasBusy)
                emit busyChanged();
            if (!wasCancelled)
                emit cancelledChanged();
            return;
        }

        const bool wasCancelled = _cancelled;
        _cancelled = false;
        _busy = false;
        emit busyChanged();
        if (wasCancelled)
            emit cancelledChanged();

        if (event->isError()) {
            _errorText = QtUtils::toQString(event->error().errorMessage());
            _resultCount = 0;
            emit errorChanged();
            emit resultChanged();
            emit statusMessage(tr("Error: %1").arg(_errorText));
            recordHistory(false, _errorText);
            return;
        }

        _errorText.clear();
        _cachedCollectionsDb.clear();
        _cachedCollections.clear();
        _fetchingLiveCollections = false;
        emit errorChanged();
        showResults(event->result());
        recordHistory(_errorText.isEmpty(), _errorText);
    }

    void ShellController::handle(DocumentListLoadedEvent *event)
    {
        if (event->sender() != _shell)
            return;

        if (event->opGen() > 0 && _shell && event->opGen() < _shell->lastDispatchedOpGen())
            return;

        if ((event->opGen() > 0 && event->opGen() <= _cancelledUpToOpGen) || event->cancelled()) {
            const bool wasBusy = _busy;
            const bool wasCancelled = _cancelled;
            _busy = false;
            _cancelled = true;
            if (wasBusy)
                emit busyChanged();
            if (!wasCancelled)
                emit cancelledChanged();
            return;
        }

        const bool wasCancelled = _cancelled;
        _cancelled = false;
        _busy = false;
        emit busyChanged();
        if (wasCancelled)
            emit cancelledChanged();

        if (event->isError()) {
            _errorText = QtUtils::toQString(event->error().errorMessage());
            emit errorChanged();
            return;
        }

        _lastQueryInfo = event->queryInfo();
        _hasQueryInfo = _lastQueryInfo._info.isValid();
        if (_hasQueryInfo) {
            _activeCollection = QtUtils::toQString(_lastQueryInfo._info._ns.collectionName());
            const QString qDb = QtUtils::toQString(_lastQueryInfo._info._ns.databaseName());
            if (!qDb.isEmpty() && qDb != _activeDatabase) {
                _activeDatabase = qDb;
                emit activeDatabaseChanged();
            }
        }
        _currentResultIndex = event->resultIndex();
        _currentDocs = event->documents();
        buildStatsSummary(_currentDocs, "");
        buildExplain(_currentDocs);
        rebuildModels(_currentDocs);
        emit statusMessage(tr("%n document(s)", "", _resultCount));
    }

    void ShellController::showResults(const MongoShellExecResult &execResult)
    {
        _results = std::vector<MongoShellResult>(execResult.results());
        _elapsedMs = 0;
        for (const auto &r : _results)
            _elapsedMs += r.elapsedMs();

        if (execResult.isCurrentDatabaseValid() && !execResult.currentDatabase().empty()) {
            const QString newDb = QtUtils::toQString(execResult.currentDatabase());
            if (newDb != _activeDatabase) {
                _activeDatabase = newDb;
                emit activeDatabaseChanged();
            }
        }
        if (execResult.isCurrentServerValid() && !execResult.currentServer().empty()) {
            _activeServer = QtUtils::toQString(execResult.currentServer());
        }

        QStringList titles;
        if (_results.size() > 1) {
            for (size_t i = 0; i < _results.size(); ++i) {
                const auto &r = _results[i];
                QString shortName = QtUtils::toQString(r.statementShort()).simplified();
                if (shortName.isEmpty())
                    shortName = tr("Result %1").arg(i + 1);
                if (shortName.size() > 28)
                    shortName = shortName.left(26) + QStringLiteral("…");
                titles.append(QStringLiteral("%1: %2 (%3)")
                                  .arg(i + 1)
                                  .arg(shortName)
                                  .arg(r.documents().size()));
            }
        }
        _resultTabTitles = titles;
        emit resultTabsChanged();

        if (_results.empty()) {
            _currentResultIndex = 0;
            _currentDocs.clear();
            _hasQueryInfo = false;
            _lastAggrInfo = AggrInfo();
            _statsSummaryRows.clear();
            _explainStages.clear();
            _explainSummary.clear();
            rebuildModels({}, QString());
            return;
        }

        // Default to the last statement result (matching interactive shell expectations)
        _currentResultIndex = static_cast<int>(_results.size()) - 1;
        emit activeResultIndexChanged();
        presentSingleResult(_currentResultIndex);
    }

    void ShellController::presentSingleResult(int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= _results.size())
            return;

        const MongoShellResult &r = _results[index];
        _currentResultType = r.type();
        _currentDocs = r.documents();
        _lastQueryInfo = r.queryInfo();
        _hasQueryInfo = _lastQueryInfo._info.isValid();
        _lastAggrInfo = r.aggrInfo();

        if (_hasQueryInfo) {
            _initialSkip = _lastQueryInfo._skip;
            _initialLimit = _lastQueryInfo._limit;
            _activeCollection = QtUtils::toQString(_lastQueryInfo._info._ns.collectionName());
            const QString qDb = QtUtils::toQString(_lastQueryInfo._info._ns.databaseName());
            if (!qDb.isEmpty() && qDb != _activeDatabase) {
                _activeDatabase = qDb;
                emit activeDatabaseChanged();
            }
        } else if (_lastAggrInfo.isValid) {
            _initialSkip = 0;
            _initialLimit = 0;
            _activeCollection = QtUtils::toQString(_lastAggrInfo.collectionName);
        } else {
            _initialSkip = 0;
            _initialLimit = 0;
            const QString inferred = inferCollectionFromScript(QtUtils::toQString(r.statement()));
            if (!inferred.isEmpty())
                _activeCollection = inferred;
        }

        buildStatsSummary(_currentDocs, r.type());
        buildExplain(_currentDocs);

        if (r.type() == "error") {
            _errorText = QtUtils::toQString(r.response());
            emit errorChanged();
        } else if (!_errorText.isEmpty()) {
            _errorText.clear();
            emit errorChanged();
        }

        const QString rawResponse = QtUtils::toQString(r.response());

        if (!_currentDocs.empty()) {
            // Restore preferred Tree/Table/JSON view if a previous scalar command
            // had auto-switched into Text mode, or if Stats Summary is no longer applicable.
            if (_autoForcedTextMode || (_viewMode == 3 && !hasStatsSummary()) ||
                (_viewMode == 4 && !hasExplain())) {
                _autoForcedTextMode = false;
                if (_viewMode != _preferredDocViewMode) {
                    _viewMode = _preferredDocViewMode;
                    emit viewModeChanged();
                }
            }
            // Explain output is unreadable as a raw tree: show the plan view,
            // and return to the preferred view on the next normal result.
            if (hasExplain() && _viewMode != 4) {
                _autoForcedTextMode = true;
                _viewMode = 4;
                emit viewModeChanged();
            }
            rebuildModels(_currentDocs);
        } else {
            rebuildModels({}, rawResponse);
            if (!rawResponse.trimmed().isEmpty() && r.type() != "error" && _viewMode != 2) {
                _autoForcedTextMode = true;
                _viewMode = 2;
                emit viewModeChanged();
            }
        }

        emit statusMessage(
            tr("%n document(s) in %1 ms", "", _resultCount)
                .arg(_elapsedMs));
    }

    void ShellController::buildStatsSummary(const std::vector<MongoDocumentPtr> &docs,
                                            const std::string &type)
    {
        _statsSummaryRows.clear();
        if (docs.empty())
            return;

        // Detect collection stats documents (either tagged 'collectionStats' or
        // having standard collStats keys 'ns' + 'count' + 'size').
        const auto firstView = docs[0]->bsonObj();
        const bool looksLikeCollStats = (type == "collectionStats") ||
            (firstView["ns"] && firstView["count"] && (firstView["size"] || firstView["storageSize"]));
        if (!looksLikeCollStats)
            return;

        for (const auto &docPtr : docs) {
            const auto v = docPtr->bsonObj();
            const std::string nsStr = BsonUtils::getField<std::string>(v, "ns");
            const MongoNamespace ns(nsStr);
            const QString collName = !ns.collectionName().empty()
                ? QtUtils::toQString(ns.collectionName())
                : QtUtils::toQString(nsStr);

            QVariantMap row;
            row.insert(QStringLiteral("name"), collName);
            row.insert(QStringLiteral("ns"), QtUtils::toQString(nsStr));
            row.insert(QStringLiteral("count"),
                       QString::number(static_cast<qlonglong>(readNumericField(v, "count"))));
            row.insert(QStringLiteral("size"),
                       MongoUtils::buildNiceSizeString(readNumericField(v, "size")));
            row.insert(QStringLiteral("storageSize"),
                       MongoUtils::buildNiceSizeString(readNumericField(v, "storageSize")));
            row.insert(QStringLiteral("totalIndexSize"),
                       MongoUtils::buildNiceSizeString(readNumericField(v, "totalIndexSize")));
            row.insert(QStringLiteral("avgObjSize"),
                       MongoUtils::buildNiceSizeString(readNumericField(v, "avgObjSize")));
            row.insert(QStringLiteral("nindexes"),
                       QString::number(static_cast<int>(readNumericField(v, "nindexes"))));
            const auto cappedEl = v["capped"];
            const bool isCapped = cappedEl && cappedEl.type() == bsoncxx::type::k_bool &&
                                  cappedEl.get_bool().value;
            row.insert(QStringLiteral("capped"), isCapped ? QStringLiteral("Yes") : QStringLiteral("No"));
            _statsSummaryRows.append(row);
        }
    }

    void ShellController::buildExplain(const std::vector<MongoDocumentPtr> &docs)
    {
        _explainStages.clear();
        _explainSummary.clear();
        if (docs.size() != 1 || !ExplainPlanParser::isExplain(docs[0]->bsonObj()))
            return;
        const ExplainPlan plan = ExplainPlanParser::parse(docs[0]->bsonObj());
        if (!plan.valid)
            return;

        for (const ExplainStage &s : plan.stages) {
            _explainStages.append(QVariantMap{
                {QStringLiteral("depth"), s.depth},
                {QStringLiteral("stage"), s.stage},
                {QStringLiteral("detail"), s.detail},
                {QStringLiteral("nReturned"), s.nReturned},
                {QStringLiteral("docsExamined"), s.docsExamined},
                {QStringLiteral("keysExamined"), s.keysExamined},
                {QStringLiteral("timeMs"), s.executionTimeMs},
                {QStringLiteral("warning"), s.warning},
            });
        }
        _explainSummary = QVariantMap{
            {QStringLiteral("ns"), plan.ns},
            {QStringLiteral("hasExecutionStats"), plan.hasExecutionStats},
            {QStringLiteral("nReturned"), plan.nReturned},
            {QStringLiteral("docsExamined"), plan.totalDocsExamined},
            {QStringLiteral("keysExamined"), plan.totalKeysExamined},
            {QStringLiteral("timeMs"), plan.executionTimeMs},
            {QStringLiteral("indexes"), plan.indexesUsed},
            {QStringLiteral("warnings"), plan.warnings},
        };
    }

    QString ShellController::textResult() const
    {
        if (_textResultDirty) {
            _textResult = !_currentDocs.empty()
                ? documentsToJson(_currentDocs)
                : _fallbackTextResult;
            _textResultDirty = false;
        }
        return _textResult;
    }

    void ShellController::rebuildModels(const std::vector<MongoDocumentPtr> &docs,
                                        const QString &fallbackText)
    {
        _resultCount = static_cast<int>(docs.size());
        _fallbackTextResult = docs.empty() ? fallbackText : QString();
        _textResult.clear();
        // Defer expensive multi-document JSON serialization until viewMode == 2 (JSON view)
        // or explicit Copy All, keeping Tree and Table queries instantaneous on large payloads.
        _textResultDirty = !docs.empty();
        if (docs.empty())
            _textResult = fallbackText;

        // Keep the old models alive until the views have rebound to the new
        // ones; deleting first makes both views tear down against a dying model.
        QObject *oldTable = _tableModel;
        QObject *oldTree = _treeModel;
        _tableModel = nullptr;
        _treeModel = nullptr;

        if (!docs.empty()) {
            _treeModel = new BsonTreeModel(docs, this);
            auto *proxy = new BsonTableModelProxy(this);
            proxy->setSourceModel(_treeModel);
            _tableModel = proxy;
        }

        emit modelsChanged();
        emit resultChanged();

        if (oldTable)
            oldTable->deleteLater();
        if (oldTree)
            oldTree->deleteLater();
    }

    QString ShellController::documentsToJson(
        const std::vector<MongoDocumentPtr> &docs) const
    {
        static constexpr size_t kViewerFieldPreviewBytes = 8192;
        static constexpr int kMaxTotalViewerChars = 2 * 1024 * 1024;

        auto *settings = AppRegistry::instance().settingsManager();
        const UUIDEncoding uuidEnc = settings ? settings->uuidEncoding() : DefaultEncoding;
        const SupportedTimes tz = settings ? settings->timeZone() : Utc;

        QString out;
        out.reserve(std::min<int>(static_cast<int>(docs.size()) * 512, 262144));
        for (size_t i = 0; i < docs.size(); ++i) {
            std::string json = BsonUtils::jsonString(
                docs[i]->bsonObj(), TenGen, 1,
                uuidEnc, tz, /*isArray=*/false, kViewerFieldPreviewBytes);
            if (docs.size() > 1)
                out += QStringLiteral("/* %1 */\n").arg(i + 1);
            out += QtUtils::toQString(json);
            if (i + 1 < docs.size())
                out += QStringLiteral("\n\n");
            if (out.size() >= kMaxTotalViewerChars && i + 1 < docs.size()) {
                out += QStringLiteral(
                    "/* Output truncated at 2 MB (showing %1 of %2 documents) to keep the UI responsive.\n"
                    "   Use Tree/Table view, View Document, or Export Collection for full binary/text payloads. */")
                    .arg(i + 1)
                    .arg(docs.size());
                break;
            }
        }
        return out;
    }
}
