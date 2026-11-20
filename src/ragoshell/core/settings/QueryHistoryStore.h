#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

namespace RagoShell
{
    /**
     * @brief One executed script (history) or a saved snippet (favorite).
     */
    struct QueryHistoryEntry
    {
        qint64 id = 0;
        QString script;
        QString database;
        QString serverName;
        QString serverUuid;
        qint64 executedAtMs = 0;
        qint64 durationMs = 0;
        int resultCount = 0;
        bool ok = true;
        QString error;
        bool favorite = false;   ///< Snippets are favorites; never trimmed.
        QString name;            ///< Optional snippet name.
    };

    /**
     * @brief GUI-independent query history + snippet store (newest first).
     *
     * Re-running the same script on the same database/server updates and moves
     * the existing entry instead of adding a duplicate. Only non-favorite
     * entries count towards kMaxEntries; favorites are kept until removed.
     */
    class QueryHistoryStore
    {
    public:
        static constexpr int kMaxEntries = 500;
        static constexpr int kMaxScriptChars = 64 * 1024;
        static constexpr int kMaxErrorChars = 500;

        const std::vector<QueryHistoryEntry> &entries() const { return _entries; }
        const QueryHistoryEntry *find(qint64 id) const;

        /// Records an execution; returns the entry id, or 0 when not recorded
        /// (empty or oversized script).
        qint64 record(QueryHistoryEntry entry);

        bool setFavorite(qint64 id, bool favorite);
        bool rename(qint64 id, const QString &name);
        bool remove(qint64 id);
        /// Removes all non-favorite entries.
        void clearHistory();

        QByteArray toJson() const;
        /// Replaces the contents; returns false (and keeps them) on parse error.
        bool fromJson(const QByteArray &json, QString *error = nullptr);

        bool load(const QString &path, QString *error = nullptr);
        bool save(const QString &path, QString *error = nullptr) const;

    private:
        void trim();

        std::vector<QueryHistoryEntry> _entries;
        qint64 _nextId = 1;
    };
}
