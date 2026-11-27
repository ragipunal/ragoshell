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
        int viewModeFromSettings(ViewMode m)
        {
            switch (m) {
            case Table: return 1;
            case Text:  return 2;
            case Tree:
            case Custom:
            default:    return 0;
            }
        }
    }

    ShellController::ShellController(MongoShell *shell, const QString &title,
                                     QObject *parent)
        : QObject(parent),
          _bus(AppRegistry::instance().bus()),
          _shell(shell),
          _title(title)
    {
        auto *settings = AppRegistry::instance().settingsManager();
        _preferredDocViewMode = settings ? viewModeFromSettings(settings->viewMode()) : 0;
        _viewMode = _preferredDocViewMode;

        if (_shell) {
            _busy = _shell->isExecutable();
            _activeDatabase = QString::fromStdString(_shell->dbname());
            if (_shell->server() && _shell->server()->connectionRecord()) {
                _activeServer = QString::fromStdString(
                    _shell->server()->connectionRecord()->getFullAddress());
                if (_activeDatabase.isEmpty()) {
                    _activeDatabase = QString::fromStdString(
                        _shell->server()->connectionRecord()->defaultDatabase());
                }
            }
            _activeCollection = inferCollectionFromScript(_shell->script());

            // Tabs opened with an auto-executing script (collection double-click,
            // management actions, history "New Tab") never pass through execute().
            if (_busy) {
                _historyPending = true;
                _historyScript = _shell->script();
                _historyDatabase = _activeDatabase;
                _historyTimer.start();
            }
        }

        _bus->subscribe(this, ScriptExecutingEvent::Type, shell);
        _bus->subscribe(this, ScriptExecutedEvent::Type, shell);
        _bus->subscribe(this, DocumentListLoadedEvent::Type, shell);
        _bus->subscribe(this, AutocompleteResponse::Type, shell);

        if (_shell && !_shell->draftId().isEmpty()) {
            // A tab recovered from a draft: same draft, same file, text still unsaved.
            _draftId = _shell->draftId();
            if (!_shell->filePath().isEmpty())
                _filePath = _shell->filePath();
            _dirty = true;
            if (ScriptDraftModel *drafts = ScriptDraftModel::instance())
                drafts->claim(_draftId);
        } else {
            _draftId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
    }

    ShellController::~ShellController()
    {
        _bus->unsubscibe(this);
    }

    void ShellController::detachShell()
    {
        _bus->unsubscibe(this);
        _shell = nullptr;
    }

    QString ShellController::serverUuid() const
    {
        if (!_shell || !_shell->server() || !_shell->server()->connectionRecord())
            return QString();
        return _shell->server()->connectionRecord()->uuid();
    }

    QString ShellController::database() const
    {
        return _shell ? QString::fromStdString(_shell->dbname()) : QString();
    }

    QString ShellController::script() const
    {
        return _shell ? _shell->script() : QString();
    }

    void ShellController::setDirty(bool d)
    {
        if (_dirty == d)
            return;
        _dirty = d;
        emit dirtyChanged();
    }

    void ShellController::updateDraft(const QString &script)
    {
        ScriptDraftModel *drafts = ScriptDraftModel::instance();
        if (!drafts || !_shell)             // no model (tests) or the tab is closing
            return;
        if (!_dirty || script.trimmed().isEmpty()) {
            drafts->remove(_draftId);
            return;
        }

        ScriptDraft draft;
        draft.id = _draftId;
        // A replica set member tab ("<uuid>@host:port") recovers on the saved connection.
        draft.serverUuid = serverUuid().section(QLatin1Char('@'), 0, 0);
        if (_shell->server() && _shell->server()->connectionRecord())
            draft.serverName = QString::fromStdString(_shell->server()->connectionRecord()->getReadableName());
        draft.database = _activeDatabase;
        draft.title = _title;
        draft.filePath = _filePath;
        draft.script = script;
        drafts->update(std::move(draft));
    }

    void ShellController::setFilePath(const QString &p)
    {
        if (_filePath == p)
            return;
        _filePath = p;
        if (!_filePath.isEmpty()) {
            const QString base = QFileInfo(_filePath).fileName();
            if (!base.isEmpty() && _title != base) {
                _title = base;
                emit titleChanged();
            }
        }
        emit filePathChanged();
    }

    void ShellController::setViewMode(int mode)
    {
        if (mode < 0 || mode > 4)
            return;
        _autoForcedTextMode = false;
        if (mode <= 2)
            _preferredDocViewMode = mode;
        if (_viewMode == mode)
            return;
        _viewMode = mode;
        emit viewModeChanged();
    }

    void ShellController::setActiveResultIndex(int idx)
    {
        if (idx < 0 || static_cast<size_t>(idx) >= _results.size())
            return;
        if (_currentResultIndex == idx)
            return;
        _currentResultIndex = idx;
        emit activeResultIndexChanged();
        presentSingleResult(idx);
    }

    QObject *ShellController::treeModel() const { return _treeModel; }
    QObject *ShellController::tableModel() const { return _tableModel; }

    bool ShellController::hasPaging() const
    {
        return (_hasQueryInfo && _lastQueryInfo._info.isValid()) || _lastAggrInfo.isValid;
    }

    int ShellController::currentSkip() const
    {
        if (_hasQueryInfo && _lastQueryInfo._info.isValid())
            return _lastQueryInfo._skip;
        if (_lastAggrInfo.isValid)
            return _lastAggrInfo.skip;
        return 0;
    }

    int ShellController::currentBatchSize() const
    {
        auto *settings = AppRegistry::instance().settingsManager();
        const int defaultBatch = settings && settings->batchSize() > 0 ? settings->batchSize() : 50;
        // A page asked for by a script (cursor.batchSize(n)) is capped like the setting.
        const auto capped = [](int size) { return std::min(size, SettingsManager::kMaxBatchSize); };
        if (_hasQueryInfo && _lastQueryInfo._info.isValid())
            return _lastQueryInfo._batchSize > 0 ? capped(_lastQueryInfo._batchSize) : defaultBatch;
        if (_lastAggrInfo.isValid)
            return _lastAggrInfo.batchSize > 0 ? capped(_lastAggrInfo.batchSize) : defaultBatch;
        return defaultBatch;
    }

    bool ShellController::canPagePrev() const
    {
        return hasPaging() && currentSkip() > _initialSkip;
    }

    bool ShellController::canPageNext() const
    {
        if (!hasPaging())
            return false;
        const int batch = currentBatchSize();
        if (_resultCount < batch)
            return false;
        if (_initialLimit > 0) {
            const int consumed = (currentSkip() - _initialSkip) + _resultCount;
            if (consumed >= _initialLimit)
                return false;
        }
        return true;
    }

    bool ShellController::canInsertDocuments() const
    {
        return !_activeDatabase.isEmpty() && !_activeCollection.isEmpty() &&
               !_activeCollection.startsWith(QLatin1String("system."));
    }

    bool ShellController::canDeleteDocuments() const
    {
        if (!canInsertDocuments())
            return false;
        if (_currentResultType != "query" || !_hasQueryInfo || !_lastQueryInfo._info.isValid())
            return false;
        if (_currentDocs.empty())
            return false;
        for (const auto &doc : _currentDocs) {
            if (doc && doc->bsonObj()["_id"])
                return true;
        }
        return false;
    }

    bool ShellController::canModifyDocuments() const
    {
        if (!canDeleteDocuments())
            return false;
        // Full-document replace / scalar cell save requires unprojected documents
        // so unprojected fields are never accidentally dropped.
        return _lastQueryInfo._fields.view().empty();
    }

    bool ShellController::isReadOnlyResult() const
    {
        return _resultCount > 0 && !canModifyDocuments();
    }

    QString ShellController::inferCollectionFromScript(const QString &scriptText) const
    {
        static const QRegularExpression reGetColl(
            QStringLiteral("db\\.getCollection\\(\\s*['\"]([^'\"]+)['\"]\\s*\\)"));
        const auto m1 = reGetColl.match(scriptText);
        if (m1.hasMatch())
            return m1.captured(1);

        static const QRegularExpression reDotColl(
            QStringLiteral("\\bdb\\.([A-Za-z_$][A-Za-z0-9_$]*)\\s*\\."));
        const auto m2 = reDotColl.match(scriptText);
        if (m2.hasMatch()) {
            const QString cand = m2.captured(1);
            static const QSet<QString> dbMethods = {
                QStringLiteral("system"), QStringLiteral("stats"), QStringLiteral("serverStatus"),
                QStringLiteral("hostInfo"), QStringLiteral("version"), QStringLiteral("currentOp"),
                QStringLiteral("killOp"), QStringLiteral("getUsers"), QStringLiteral("getUser"),
                QStringLiteral("createUser"), QStringLiteral("getRoles"), QStringLiteral("runCommand"),
                QStringLiteral("adminCommand"), QStringLiteral("getCollectionNames"),
                QStringLiteral("getCollectionInfos"), QStringLiteral("printCollectionStats"),
                QStringLiteral("createCollection"), QStringLiteral("dropDatabase"),
                QStringLiteral("repairDatabase"), QStringLiteral("getSiblingDB"),
                QStringLiteral("getMongo"), QStringLiteral("getName")
            };
            if (!dbMethods.contains(cand))
                return cand;
        }
        return QString();
    }

    void ShellController::execute(const QString &script)
    {
        if (_busy || !_shell)
            return;
        const bool wasCancelled = _cancelled;
        _cancelled = false;
        if (wasCancelled)
            emit cancelledChanged();
        _errorText.clear();
        emit errorChanged();
        _busy = true;
        emit busyChanged();

        const QString inferred = inferCollectionFromScript(script);
        if (!inferred.isEmpty())
            _activeCollection = inferred;

        _historyPending = true;
        _historyScript = script;
        _historyDatabase = _activeDatabase;
        _historyTimer.start();

        _shell->setAggrInfo(AggrInfo());
        _shell->setScriptExecutable(true);
        _shell->execute(QtUtils::toStdString(script), QtUtils::toStdString(_activeDatabase));
    }

    bool ShellController::saveAsSnippet(const QString &script, const QString &name)
    {
        QueryHistoryModel *history = QueryHistoryModel::instance();
        if (!history)
            return false;
        QString serverName;
        QString serverUuid;
        if (_shell && _shell->server() && _shell->server()->connectionRecord()) {
            const ConnectionSettings *conn = _shell->server()->connectionRecord();
            serverName = QtUtils::toQString(conn->getReadableName());
            serverUuid = conn->uuid().section(QLatin1Char('@'), 0, 0);
        }
        return history->saveSnippet(script, name, _activeDatabase, serverUuid, serverName) != 0;
    }

    void ShellController::recordHistory(bool ok, const QString &error)
    {
        if (!_historyPending)
            return;
        _historyPending = false;
        QueryHistoryModel *history = QueryHistoryModel::instance();
        if (!history || !_shell)
            return;

        QueryHistoryEntry entry;
        entry.script = _historyScript;
        entry.database = _historyDatabase;
        if (_shell->server() && _shell->server()->connectionRecord()) {
            const ConnectionSettings *conn = _shell->server()->connectionRecord();
            entry.serverName = QtUtils::toQString(conn->getReadableName());
            // Shell connections may carry a "@..." suffix; keep the saved connection's uuid.
            entry.serverUuid = conn->uuid().section(QLatin1Char('@'), 0, 0);
        }
        entry.executedAtMs = QDateTime::currentMSecsSinceEpoch();
        entry.durationMs = ok ? _elapsedMs : _historyTimer.elapsed();
        entry.resultCount = ok ? _resultCount : 0;
        entry.ok = ok;
        entry.error = error;
        history->record(entry);
    }

    void ShellController::executeQueryBar(const QString &collectionName,
                                          const QString &filterJson,
                                          const QString &projectionJson,
                                          const QString &sortJson,
                                          int limitVal,
                                          bool explainMode)
    {
        const QString coll = collectionName.trimmed().isEmpty()
            ? _activeCollection
            : collectionName.trimmed();
        if (coll.isEmpty()) {
            _errorText = tr("Select or specify a collection name first.");
            emit errorChanged();
            return;
        }

        const QString expr = buildFindQuery(
            coll, true, filterJson, projectionJson, sortJson, limitVal, 0, explainMode);
        execute(expr);
    }

    QVariantMap ShellController::parseFindQuery(const QString &scriptText) const
    {
        const MongoJsonUtils::ParsedFindQuery parsed =
            MongoJsonUtils::parseFindQueryChain(scriptText);
        return QVariantMap{
            {QStringLiteral("valid"),            parsed.valid},
            {QStringLiteral("collection"),       parsed.collection},
            {QStringLiteral("useGetCollection"), parsed.useGetCollection},
            {QStringLiteral("filter"),           parsed.filter},
            {QStringLiteral("projection"),       parsed.projection},
            {QStringLiteral("sort"),             parsed.sort},
            {QStringLiteral("limit"),            parsed.limit},
            {QStringLiteral("skip"),             parsed.skip},
            {QStringLiteral("explain"),          parsed.explain},
            {QStringLiteral("explainArg"),       parsed.explainArg}
        };
    }

    QString ShellController::buildFindQuery(const QString &collectionName,
                                            bool useGetCollection,
                                            const QString &filterJson,
                                            const QString &projectionJson,
                                            const QString &sortJson,
                                            int limitVal,
                                            int skipVal,
                                            bool explainMode,
                                            const QString &explainArg) const
    {
        MongoJsonUtils::ParsedFindQuery q;
        q.valid = true;
        q.collection = collectionName.trimmed().isEmpty()
            ? _activeCollection
            : collectionName.trimmed();
        q.useGetCollection = useGetCollection;
        q.filter = filterJson;
        q.projection = projectionJson;
        q.sort = sortJson;
        q.limit = std::max(0, limitVal);
        q.skip = std::max(0, skipVal);
        q.explain = explainMode;
        q.explainArg = explainArg;
        return MongoJsonUtils::buildFindQueryChain(q);
    }

    void ShellController::previewActiveCollection(const QString &collectionName)
    {
        const QString trimmed = collectionName.trimmed();
        if (trimmed.isEmpty() || _activeCollection == trimmed)
            return;
        _activeCollection = trimmed;
        emit resultChanged();
    }

    void ShellController::fetchMore()
    {
        nextPage();
    }

    void ShellController::prevPage()
    {
        if (!hasPaging() || _busy)
            return;
        const int batch = currentBatchSize();
        const int nextSkip = std::max(_initialSkip, currentSkip() - batch);
        loadPage(nextSkip, batch);
    }

    void ShellController::nextPage()
    {
        if (!hasPaging() || _busy)
            return;
        const int batch = currentBatchSize();
        loadPage(currentSkip() + batch, batch);
    }

    void ShellController::loadPage(int skip, int batchSize)
    {
        if (!hasPaging() || _busy || !_shell)
            return;

        if (batchSize <= 0)
            batchSize = currentBatchSize();
        if (skip < _initialSkip)
            skip = _initialSkip;

        const bool wasCancelled = _cancelled;
        _cancelled = false;
        if (wasCancelled)
            emit cancelledChanged();
        _errorText.clear();
        emit errorChanged();
        _busy = true;
        emit busyChanged();

        if (_lastAggrInfo.isValid) {
            // The recorded pipeline plus $skip and $limit, with every type (ObjectId, date, ...) kept.
            const std::string query = aggregatePageScript(_lastAggrInfo.collectionName, _lastAggrInfo.pipeline.view(),
                                                          _lastAggrInfo.options.view(), skip, batchSize);

            const AggrInfo nextAggr(_lastAggrInfo.collectionName, skip, batchSize,
                                    _lastAggrInfo.pipeline.view(), _lastAggrInfo.options.view(),
                                    _currentResultIndex);
            _lastAggrInfo = nextAggr;
            _shell->setScriptExecutable(true);
            _shell->setAggrInfo(nextAggr);
            _shell->execute(query, QtUtils::toStdString(_activeDatabase));
            return;
        }

        int limit = batchSize;
        if (_initialLimit != 0) {
            const int skipDelta = skip - _initialSkip;
            limit = _initialLimit - skipDelta;
            if (limit <= 0)
                limit = -1;
            else if (limit > batchSize)
                limit = batchSize;
        }

        MongoQueryInfo info(_lastQueryInfo);
        info._skip = skip;
        info._batchSize = batchSize;
        info._limit = limit;
        _lastQueryInfo = info;
        _shell->query(_currentResultIndex, info);
    }

    void ShellController::reloadCurrentPage()
    {
        if (_busy || !_shell)
            return;
        if (hasPaging()) {
            loadPage(currentSkip(), currentBatchSize());
        } else if (!_shell->script().trimmed().isEmpty()) {
            execute(_shell->script());
        }
    }

    void ShellController::stop()
    {
        if (_shell) {
            _cancelledUpToOpGen = _shell->lastDispatchedOpGen();
            _shell->stop();
        }
        const bool wasBusy = _busy;
        const bool wasCancelled = _cancelled;
        const bool hadError = !_errorText.isEmpty();
        _busy = false;
        _cancelled = true;
        _errorText.clear();
        if (wasBusy)
            emit busyChanged();
        if (!wasCancelled)
            emit cancelledChanged();
        if (hadError)
            emit errorChanged();
        emit resultChanged();
        emit statusMessage(tr("Operation cancelled."));
    }

    void ShellController::requestAutocomplete(const QString &prefix)
    {
        // Member access on a collection: the user works with it.
        static const QRegularExpression collectionAccess(
            QStringLiteral(R"(^db\.(?:getCollection\(\s*['"]([^'"]+)['"]\s*\)|([A-Za-z_$][\w$]*))\.)"));
        const QRegularExpressionMatch m = collectionAccess.match(prefix);
        if (m.hasMatch()) {
            const QString collection = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
            if (!collection.startsWith(QLatin1String("get")))   // db.getSiblingDB(...).x and the like
                emit collectionReferenced(collection);
        }
        if (!_busy && _shell)
            _shell->autocomplete(prefix.toStdString());
    }

    QVariantMap ShellController::requestCompletion(const QString &textBeforeCaret,
                                                   bool explicitTrigger,
                                                   bool delimiterTrigger)
    {
        QVariantMap answer{
            {QStringLiteral("handled"), false},
            {QStringLiteral("pending"), false},
            {QStringLiteral("replaceLength"), 0},
        };

        SettingsManager *settings = AppRegistry::instance().settingsManager();
        const AutocompletionMode mode = settings ? settings->autocompletionMode() : AutocompleteAll;
        if (mode == AutocompleteNone)
            return answer;

        const ShellCompletion::Context preview = ShellCompletion::analyze(textBeforeCaret);
        answer.insert(QStringLiteral("replaceLength"), preview.replaceLength);
        if (preview.scope == ShellCompletion::Scope::None)
            return answer;

        ShellCompletion::Environment env;
        env.mode = mode;
        env.explicitTrigger = explicitTrigger;
        env.delimiterTrigger = delimiterTrigger;

        const QString connUuid = serverUuid().section(QLatin1Char('@'), 0, 0);
        SchemaCatalog *catalog = SchemaCatalog::instance();

        if (!_activeDatabase.isEmpty() && mode != AutocompleteNoCollectionNames) {
            QSet<QString> seen;
            auto addList = [&](const QStringList &list) {
                for (const QString &c : list) {
                    if (!c.isEmpty() && !seen.contains(c)) {
                        seen.insert(c);
                        env.collections << c;
                    }
                }
            };
            if (_cachedCollectionsDb == _activeDatabase)
                addList(_cachedCollections);
            if (_shell && _shell->server()) {
                if (MongoDatabase *dbObj = _shell->server()->findDatabaseByName(_activeDatabase.toStdString()))
                    addList(dbObj->getCollectionNames());
            }
            if (catalog && !connUuid.isEmpty())
                addList(catalog->analyzedCollections(connUuid, _activeDatabase));
            env.collections.sort(Qt::CaseInsensitive);
        }

        if (_shell && _shell->server())
            env.databases = _shell->server()->getDatabasesNames();
        if (!_activeDatabase.isEmpty() && !env.databases.contains(_activeDatabase))
            env.databases << _activeDatabase;

        if (catalog && !connUuid.isEmpty() && !_activeDatabase.isEmpty() && !preview.collection.isEmpty()) {
            if (const auto snapshot = catalog->find(connUuid, _activeDatabase, preview.collection))
                env.fields = snapshot->result.fields;
        }

        const ShellCompletion::Result res = ShellCompletion::complete(textBeforeCaret, env);
        answer.insert(QStringLiteral("replaceLength"), res.context.replaceLength);

        if (!_activeDatabase.isEmpty() && !res.context.collection.isEmpty()) {
            const bool memberOnColl = res.context.scope == ShellCompletion::Scope::MemberAccess
                && res.context.receiver == ShellCompletion::Receiver::Collection;
            if (res.needsSchema || memberOnColl)
                emit collectionReferenced(res.context.collection);
        }

        if (res.needsLiveCollections && !_activeDatabase.isEmpty()
            && _cachedCollectionsDb != _activeDatabase && !_fetchingLiveCollections
            && _shell && !_busy) {
            _fetchingLiveCollections = true;
            _fetchingCollectionsForDb = _activeDatabase;
            _shell->autocomplete("db.");
        }
        answer.insert(QStringLiteral("pending"), _fetchingLiveCollections && res.needsLiveCollections);

        if (res.items.empty())
            return answer;

        _autocompleteList.clear();
        _autocompleteInserts.clear();
        _autocompleteDetails.clear();
        _autocompleteList.reserve(static_cast<qsizetype>(res.items.size()));
        _autocompleteInserts.reserve(static_cast<qsizetype>(res.items.size()));
        _autocompleteDetails.reserve(static_cast<qsizetype>(res.items.size()));
        for (const ShellCompletion::Item &item : res.items) {
            _autocompleteList << item.label;
            _autocompleteInserts << item.insert;
            _autocompleteDetails << item.detail;
        }
        emit autocompleteReady();
        answer.insert(QStringLiteral("handled"), true);
        return answer;
    }

    QVariantMap ShellController::requestFieldCompletion(const QString &textBeforeCaret)
    {
        QVariantMap answer{{QStringLiteral("fieldPosition"), false}, {QStringLiteral("handled"), false},
                           {QStringLiteral("replaceLength"), 0}};
        const FieldCompletion::Context context = FieldCompletion::analyze(textBeforeCaret);
        if (context.kind == FieldCompletion::Kind::None || _activeDatabase.isEmpty())
            return answer;
        answer.insert(QStringLiteral("fieldPosition"), true);

        const QString connUuid = serverUuid().section(QLatin1Char('@'), 0, 0);
        SchemaCatalog *catalog = SchemaCatalog::instance();
        const auto snapshot = catalog ? catalog->find(connUuid, _activeDatabase, context.collection)
                                      : std::optional<SchemaSnapshot>();
        if (!snapshot) {
            emit collectionReferenced(context.collection);   // the collector samples it
            return answer;
        }
        const std::vector<FieldCompletion::Item> items = FieldCompletion::complete(context, snapshot->result.fields);
        if (items.empty())
            return answer;
        _autocompleteList.clear();
        _autocompleteInserts.clear();
        _autocompleteDetails.clear();
        for (const FieldCompletion::Item &item : items) {
            _autocompleteList << item.label;
            _autocompleteInserts << item.insert;
            _autocompleteDetails << item.detail;
        }
        emit autocompleteReady();
        answer.insert(QStringLiteral("handled"), true);
        answer.insert(QStringLiteral("replaceLength"), context.replaceLength);
        return answer;
    }

}
