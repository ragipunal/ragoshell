#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

namespace RagoShell
{
    /**
     * @brief The unsaved script text of one shell tab, kept so it can be
     *        recovered after a crash or a forced shutdown.
     */
    struct ScriptDraft
    {
        QString id;            ///< Stable per tab (a UUID); the key of the draft.
        QString serverUuid;    ///< Saved connection the tab was open on.
        QString serverName;    ///< Its readable name, for the recovery list.
        QString database;
        QString title;
        QString filePath;      ///< Script file the tab was editing, if any.
        QString script;
        qint64 savedAtMs = 0;
    };

    /**
     * @brief GUI-independent store of script drafts (newest first).
     *
     * A draft reflects the current text of a tab, so update() replaces the
     * previous one. A blank or oversized script is not kept (and removes the
     * previous draft of that tab). Drafts older than kMaxAgeMs are dropped by
     * pruneOlderThan(); only the kMaxDrafts newest are kept.
     */
    class ScriptDraftStore
    {
    public:
        static constexpr int kMaxDrafts = 50;
        static constexpr int kMaxScriptChars = 1000000;
        static constexpr qint64 kMaxAgeMs = 30LL * 24 * 60 * 60 * 1000;

        const std::vector<ScriptDraft> &entries() const { return _entries; }
        const ScriptDraft *find(const QString &id) const;

        /// Stores (or replaces) the draft with draft.id and moves it first.
        /// Returns false when it is not kept: empty id, blank or oversized script.
        bool update(ScriptDraft draft);
        bool remove(const QString &id);
        void clear();
        /// Drops drafts saved more than kMaxAgeMs before nowMs; returns how many.
        int pruneOlderThan(qint64 nowMs);

        QByteArray toJson() const;
        /// Replaces the contents; returns false (and keeps them) on parse error.
        bool fromJson(const QByteArray &json, QString *error = nullptr);

        /// A missing file is an empty store.
        bool load(const QString &path, QString *error = nullptr);
        /// Atomic. An empty store removes the file instead of writing an empty one.
        bool save(const QString &path, QString *error = nullptr) const;

    private:
        std::vector<ScriptDraft> _entries;
    };
}
