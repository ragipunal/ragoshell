#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/settings/QueryHistoryStore.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    /**
     * @brief QML singleton "QueryHistory": executed scripts and saved snippets.
     *
     * Wraps QueryHistoryStore, filters it for the History panel and persists
     * it to ~/.ragoshell/query_history.json (debounced, atomic).
     */
    class QueryHistoryModel : public QAbstractListModel
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(QueryHistory)
        QML_SINGLETON

        Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged FINAL)
        Q_PROPERTY(bool snippetsOnly READ snippetsOnly WRITE setSnippetsOnly NOTIFY filterChanged FINAL)
        Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)

    public:
        enum Roles {
            IdRole = Qt::UserRole + 1,
            ScriptRole,
            PreviewRole,
            DatabaseRole,
            ServerNameRole,
            ServerUuidRole,
            ExecutedAtRole,
            DurationMsRole,
            ResultCountRole,
            OkRole,
            ErrorRole,
            FavoriteRole,
            NameRole
        };

        explicit QueryHistoryModel(const QString &filePath, QObject *parent = nullptr);
        ~QueryHistoryModel() override;

        static QueryHistoryModel *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<QueryHistoryModel>::get();
        }
        /// The instance main() registered, or nullptr (e.g. in tests).
        static QueryHistoryModel *instance() { return QmlSingletonInstance<QueryHistoryModel>::peek(); }

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString filterText() const { return _filterText; }
        void setFilterText(const QString &text);
        bool snippetsOnly() const { return _snippetsOnly; }
        void setSnippetsOnly(bool on);

        /// Called by ShellController after each execution.
        void record(const QueryHistoryEntry &entry);

        /// Saves a script as a named snippet (favorite); returns its id.
        Q_INVOKABLE qint64 saveSnippet(const QString &script, const QString &name,
                                       const QString &database, const QString &serverUuid,
                                       const QString &serverName);
        Q_INVOKABLE void setFavorite(qint64 id, bool favorite);
        Q_INVOKABLE void rename(qint64 id, const QString &name);
        Q_INVOKABLE void remove(qint64 id);
        /// Writes a pending save now (the app is going to the background).
        void flush();
        /// Removes history but keeps snippets.
        Q_INVOKABLE void clearHistory();
        /// The store was rewritten from outside (cloud sync merge): rebuild the rows and
        /// save at once, so the file matches what sync-state.json is about to record.
        void reloadFromStore();

        QueryHistoryStore *store() { return &_store; }

    Q_SIGNALS:
        void filterChanged();
        void countChanged();

    private:
        void rebuild();
        void scheduleSave();
        void saveNow();

        QueryHistoryStore _store;
        std::vector<int> _visible;   ///< indices into _store.entries()
        QString _filterText;
        bool _snippetsOnly = false;
        QString _filePath;
        QTimer _saveTimer;
    };
}
