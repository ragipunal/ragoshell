#pragma once

#include <cstdint>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <vector>

#include "ragoshell/core/domain/MongoShellResult.h"
#include "ragoshell/core/domain/MongoQueryInfo.h"
#include "ragoshell/core/domain/MongoAggregateInfo.h"

namespace RagoShell
{
    class BsonTreeModel;
    class BsonTableModelProxy;
    class MongoShell;
    class MongoServer;
    class ScriptExecutingEvent;
    class ScriptExecutedEvent;
    class DocumentListLoadedEvent;
    class AutocompleteResponse;
    class InsertDocumentResponse;
    struct RemoveDocumentResponse;
    class EventBus;

    /**
     * @brief QML-facing controller for a single shell tab.
     *
     * Wraps a core MongoShell, manages multi-statement result sub-tabs,
     * Tree/Table/JSON/StatsSummary views, two-way pagination (find & aggregate),
     * native C++ BSON document CRUD (Insert / Edit / Delete / In-Place cell edit),
     * and visual Query Bar execution.
     */
    class ShellController : public QObject
    {
        Q_OBJECT

        Q_PROPERTY(QString title READ title NOTIFY titleChanged FINAL)
        Q_PROPERTY(QString serverUuid READ serverUuid CONSTANT FINAL)
        Q_PROPERTY(QString database READ database CONSTANT FINAL)
        Q_PROPERTY(QString activeDatabase READ activeDatabase NOTIFY activeDatabaseChanged FINAL)
        Q_PROPERTY(QString activeServer READ activeServer NOTIFY resultChanged FINAL)
        Q_PROPERTY(QString activeCollection READ activeCollection NOTIFY resultChanged FINAL)
        Q_PROPERTY(QString script READ script CONSTANT FINAL)
        Q_PROPERTY(bool dirty READ dirty WRITE setDirty NOTIFY dirtyChanged FINAL)
        Q_PROPERTY(QString filePath READ filePath WRITE setFilePath NOTIFY filePathChanged FINAL)
        Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
        Q_PROPERTY(bool cancelled READ cancelled NOTIFY cancelledChanged FINAL)
        Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged FINAL)
        Q_PROPERTY(int resultCount READ resultCount NOTIFY resultChanged FINAL)
        Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY resultChanged FINAL)
        Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged FINAL)
        Q_PROPERTY(bool hasTextOutput READ hasTextOutput NOTIFY resultChanged FINAL)
        Q_PROPERTY(QString textResult READ textResult NOTIFY resultChanged FINAL)

        // Multi-statement result sub-tabs
        Q_PROPERTY(QStringList resultTabTitles READ resultTabTitles NOTIFY resultTabsChanged FINAL)
        Q_PROPERTY(int activeResultIndex READ activeResultIndex
                   WRITE setActiveResultIndex NOTIFY activeResultIndexChanged FINAL)

        // Collection statistics summary view
        Q_PROPERTY(bool hasStatsSummary READ hasStatsSummary NOTIFY resultChanged FINAL)
        Q_PROPERTY(QVariantList statsSummaryRows READ statsSummaryRows NOTIFY resultChanged FINAL)

        // Explain plan view (viewMode 4) for `.explain()` results
        Q_PROPERTY(bool hasExplain READ hasExplain NOTIFY resultChanged FINAL)
        Q_PROPERTY(QVariantList explainStages READ explainStages NOTIFY resultChanged FINAL)
        Q_PROPERTY(QVariantMap explainSummary READ explainSummary NOTIFY resultChanged FINAL)

        // Two-way pagination (Query & Aggregate)
        Q_PROPERTY(bool hasPaging READ hasPaging NOTIFY resultChanged FINAL)
        Q_PROPERTY(int currentSkip READ currentSkip NOTIFY resultChanged FINAL)
        Q_PROPERTY(int currentBatchSize READ currentBatchSize NOTIFY resultChanged FINAL)
        Q_PROPERTY(bool canPagePrev READ canPagePrev NOTIFY resultChanged FINAL)
        Q_PROPERTY(bool canPageNext READ canPageNext NOTIFY resultChanged FINAL)
        Q_PROPERTY(bool canFetchMore READ canFetchMore NOTIFY resultChanged FINAL)

        // Document CRUD availability
        Q_PROPERTY(bool canInsertDocuments READ canInsertDocuments NOTIFY resultChanged FINAL)
        Q_PROPERTY(bool canDeleteDocuments READ canDeleteDocuments NOTIFY resultChanged FINAL)
        Q_PROPERTY(bool canModifyDocuments READ canModifyDocuments NOTIFY resultChanged FINAL)
        Q_PROPERTY(bool isReadOnlyResult READ isReadOnlyResult NOTIFY resultChanged FINAL)

        Q_PROPERTY(QObject *treeModel READ treeModel NOTIFY modelsChanged FINAL)
        Q_PROPERTY(QObject *tableModel READ tableModel NOTIFY modelsChanged FINAL)
        Q_PROPERTY(QStringList autocompleteList READ autocompleteList
                   NOTIFY autocompleteReady FINAL)
        /// Field completions only (empty for the shell's own): the text each
        /// item inserts, and its type and presence ("String · 98%").
        Q_PROPERTY(QStringList autocompleteInserts READ autocompleteInserts
                   NOTIFY autocompleteReady FINAL)
        Q_PROPERTY(QStringList autocompleteDetails READ autocompleteDetails
                   NOTIFY autocompleteReady FINAL)

    public:
        /// viewMode values: 0 = Tree, 1 = Table, 2 = Colored JSON/Text, 3 = Stats Summary,
        /// 4 = Explain plan.
        ShellController(MongoShell *shell, const QString &title,
                        QObject *parent = nullptr);
        ~ShellController() override;

        QString title() const { return _title; }
        QString serverUuid() const;
        QString database() const;
        QString activeDatabase() const { return _activeDatabase; }
        QString activeServer() const { return _activeServer; }
        QString activeCollection() const { return _activeCollection; }
        QString script() const;
        bool dirty() const { return _dirty; }
        void setDirty(bool d);
        QString filePath() const { return _filePath; }
        /// Key of this tab's draft in ScriptDraftModel (see updateDraft()).
        QString draftId() const { return _draftId; }
        void setFilePath(const QString &p);

        bool busy() const { return _busy; }
        bool cancelled() const { return _cancelled; }
        int viewMode() const { return _viewMode; }
        void setViewMode(int mode);
        int resultCount() const { return _resultCount; }
        qint64 elapsedMs() const { return _elapsedMs; }
        QString errorText() const { return _errorText; }
        bool hasTextOutput() const { return _resultCount > 0 || !_fallbackTextResult.isEmpty(); }
        QString textResult() const;

        QStringList resultTabTitles() const { return _resultTabTitles; }
        int activeResultIndex() const { return _currentResultIndex; }
        void setActiveResultIndex(int idx);

        bool hasStatsSummary() const { return !_statsSummaryRows.isEmpty(); }
        bool hasExplain() const { return !_explainStages.isEmpty(); }
        QVariantList explainStages() const { return _explainStages; }
        QVariantMap explainSummary() const { return _explainSummary; }
        QVariantList statsSummaryRows() const { return _statsSummaryRows; }

        bool hasPaging() const;
        int currentSkip() const;
        int currentBatchSize() const;
        bool canPagePrev() const;
        bool canPageNext() const;
        bool canFetchMore() const { return canPageNext(); }

        bool canInsertDocuments() const;
        bool canDeleteDocuments() const;
        bool canModifyDocuments() const;
        bool isReadOnlyResult() const;

        QObject *treeModel() const;
        QObject *tableModel() const;

        MongoShell *shell() const { return _shell; }
        /// Drop event subscriptions and the shell pointer; call before the
        /// owning App destroys the shell so a pending deleteLater() is safe.
        void detachShell();

        QStringList autocompleteList() const { return _autocompleteList; }
        QStringList autocompleteInserts() const { return _autocompleteInserts; }
        QStringList autocompleteDetails() const { return _autocompleteDetails; }

        Q_INVOKABLE void execute(const QString &script);
        /// Keeps the unsaved text of this tab on disk for crash recovery: stores
        /// "script" as the draft while the tab is dirty, removes the draft otherwise.
        Q_INVOKABLE void updateDraft(const QString &script);
        /// Saves 'script' as a named snippet tagged with this tab's server/database.
        Q_INVOKABLE bool saveAsSnippet(const QString &script, const QString &name);
        Q_INVOKABLE void executeQueryBar(const QString &collectionName,
                                         const QString &filterJson,
                                         const QString &projectionJson,
                                         const QString &sortJson,
                                         int limitVal,
                                         bool explainMode = false);
        Q_INVOKABLE QVariantMap parseFindQuery(const QString &scriptText) const;
        Q_INVOKABLE QString buildFindQuery(const QString &collectionName,
                                           bool useGetCollection,
                                           const QString &filterJson,
                                           const QString &projectionJson,
                                           const QString &sortJson,
                                           int limitVal,
                                           int skipVal = 0,
                                           bool explainMode = false,
                                           const QString &explainArg = QString()) const;
        Q_INVOKABLE void previewActiveCollection(const QString &collectionName);
        Q_INVOKABLE void fetchMore();
        Q_INVOKABLE void prevPage();
        Q_INVOKABLE void nextPage();
        Q_INVOKABLE void loadPage(int skip, int batchSize);
        Q_INVOKABLE void reloadCurrentPage();
        Q_INVOKABLE void stop();
        Q_INVOKABLE void requestAutocomplete(const QString &prefix);
        /// Cursor-position-aware IntelliSense completion for the shell editor:
        /// returns { handled, pending, replaceLength } and emits autocompleteReady()
        /// when items are available.
        Q_INVOKABLE QVariantMap requestCompletion(const QString &textBeforeCaret,
                                                  bool explicitTrigger = false,
                                                  bool delimiterTrigger = false);
        /// Field-name completions from the schema catalog where the text
        /// before the caret expects a field (docs/SchemaIntelligence.md).
        /// Answers at once: { fieldPosition, handled, replaceLength };
        /// handled = the list was set (autocompleteReady). A field position
        /// without a snapshot asks the schema collector for one.
        Q_INVOKABLE QVariantMap requestFieldCompletion(const QString &textBeforeCaret);

        // Document inspection, copy & native C++ CRUD
        Q_INVOKABLE QString documentJsonAt(int docIndex) const;
        Q_INVOKABLE QString documentJsonForView(int docIndex) const;
        Q_INVOKABLE int documentByteSizeAt(int docIndex) const;
        Q_INVOKABLE QString documentIdPreviewAt(int docIndex) const;
        Q_INVOKABLE QString duplicateTemplateJsonAt(int docIndex) const;
        Q_INVOKABLE QVariantMap duplicateTemplateAt(int docIndex) const;
        Q_INVOKABLE bool isEditIdChanged(int docIndex, const QString &jsonText) const;
        Q_INVOKABLE QString validateJson(const QString &jsonText, bool singleDoc = false) const;
        Q_INVOKABLE QString formatJson(const QString &jsonText) const;
        Q_INVOKABLE QStringList detectDestructiveOperations(const QString &scriptText) const;
        Q_INVOKABLE void copyToClipboard(const QString &text) const;
        Q_INVOKABLE QString objectIdTimestamp(const QString &oidText) const;

        Q_INVOKABLE bool insertDocumentsFromJson(const QString &jsonText, QString *outError = nullptr);
        Q_INVOKABLE QString submitInsertJson(const QString &jsonText);
        Q_INVOKABLE QString submitEditJson(int docIndex, const QString &jsonText);
        Q_INVOKABLE QString updateScalarCell(int docIndex, const QString &fieldPath,
                                             const QString &newValue, const QString &typeName);
        Q_INVOKABLE QString deleteDocumentsAt(const QVariantList &docIndices);

    public Q_SLOTS:
        void handle(ScriptExecutingEvent *event);
        void handle(ScriptExecutedEvent *event);
        void handle(DocumentListLoadedEvent *event);
        void handle(AutocompleteResponse *event);
        void handle(InsertDocumentResponse *event);
        void handle(RemoveDocumentResponse *event);

    Q_SIGNALS:
        void titleChanged();
        void activeDatabaseChanged();
        /// The editor names a collection of the active database
        /// (db.orders. / db.getCollection('orders').), for the schema collector.
        void collectionReferenced(const QString &collection);
        void dirtyChanged();
        void filePathChanged();
        void busyChanged();
        void cancelledChanged();
        void viewModeChanged();
        void resultChanged();
        void resultTabsChanged();
        void activeResultIndexChanged();
        void errorChanged();
        void modelsChanged();
        void statusMessage(const QString &text);
        void autocompleteReady();
        void liveCollectionsUpdated();
        void crudCompleted(bool ok, const QString &message);

    private:
        void showResults(const MongoShellExecResult &execResult);
        void presentSingleResult(int index);
        void rebuildModels(const std::vector<MongoDocumentPtr> &docs, const QString &fallbackText = QString());
        void buildStatsSummary(const std::vector<MongoDocumentPtr> &docs, const std::string &type);
        void buildExplain(const std::vector<MongoDocumentPtr> &docs);
        void recordHistory(bool ok, const QString &error);
        QString documentsToJson(const std::vector<MongoDocumentPtr> &docs) const;
        QString inferCollectionFromScript(const QString &scriptText) const;

        EventBus *_bus;
        MongoShell *_shell;   // owned by App
        QString _title;
        QString _activeDatabase;
        QString _activeServer;
        QString _activeCollection;
        QString _filePath;
        QString _draftId;
        bool _dirty = false;
        bool _busy = false;
        bool _cancelled = false;
        uint64_t _cancelledUpToOpGen = 0;
        int _viewMode = 0;
        int _preferredDocViewMode = 0;
        bool _autoForcedTextMode = false;

        int _resultCount = 0;
        qint64 _elapsedMs = 0;
        QString _errorText;
        QString _fallbackTextResult;
        mutable QString _textResult;
        mutable bool _textResultDirty = false;

        std::vector<MongoShellResult> _results;
        std::vector<MongoDocumentPtr> _currentDocs;
        QStringList _resultTabTitles;
        QVariantList _statsSummaryRows;
        QVariantList _explainStages;
        QVariantMap _explainSummary;

        // Script awaiting its ScriptExecutedEvent, for the query history.
        bool _historyPending = false;
        QString _historyScript;
        QString _historyDatabase;
        QElapsedTimer _historyTimer;

        MongoQueryInfo _lastQueryInfo;
        bool _hasQueryInfo = false;
        std::string _currentResultType;
        AggrInfo _lastAggrInfo;
        int _initialSkip = 0;
        int _initialLimit = 0;
        int _currentResultIndex = 0;
        int _pendingCrudOps = 0;
        bool _crudBatchHadError = false;

        BsonTreeModel *_treeModel = nullptr;
        BsonTableModelProxy *_tableModel = nullptr;
        QStringList _autocompleteList;
        QStringList _autocompleteInserts;
        QStringList _autocompleteDetails;
        QStringList _cachedCollections;
        QString _cachedCollectionsDb;
        QString _fetchingCollectionsForDb;
        bool _fetchingLiveCollections = false;
    };
}
