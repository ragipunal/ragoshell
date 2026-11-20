#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/settings/ScriptDraftStore.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    /**
     * @brief QML singleton "Drafts": unsaved script text of shell tabs, kept
     *        on disk (<appDataDir>/drafts.json, atomic) so it survives a crash
     *        or a forced shutdown.
     *
     * Open tabs keep their draft up to date through ShellController::updateDraft()
     * and remove it when they close or the text is saved. A draft found on disk
     * at startup has no tab yet; those are the rows of this model (the
     * recovery list). Recovering one opens a tab that takes the draft over
     * (claim()), and the row disappears. Nothing connects by itself: that is
     * QmlBackend::recoverDraft(), started by the user.
     */
    class ScriptDraftModel : public QAbstractListModel
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(Drafts)
        QML_SINGLETON

        /// Drafts waiting for recovery (not claimed by an open tab).
        Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)

    public:
        enum Roles {
            IdRole = Qt::UserRole + 1,
            TitleRole,
            ServerNameRole,
            ServerUuidRole,
            DatabaseRole,
            FilePathRole,
            PreviewRole,
            ScriptRole,
            SavedAtRole
        };

        explicit ScriptDraftModel(const QString &filePath, QObject *parent = nullptr);

        static ScriptDraftModel *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<ScriptDraftModel>::get();
        }
        /// The instance main() registered, or nullptr (e.g. in tests).
        static ScriptDraftModel *instance() { return QmlSingletonInstance<ScriptDraftModel>::peek(); }

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        /// Stores the draft of an open tab (stamps the time) and saves.
        /// A blank script removes it instead.
        void update(ScriptDraft draft);
        /// Removes a draft (tab closed, text saved or discarded) and saves.
        void remove(const QString &id);
        /// A tab took over this draft: it leaves the recovery list.
        void claim(const QString &id);
        /// Copy of a draft; false when unknown.
        bool find(const QString &id, ScriptDraft *out) const;

        /// Discards every draft that is waiting for recovery.
        Q_INVOKABLE void discardAll();
        Q_INVOKABLE void discard(const QString &id) { remove(id); }

        /// The recovery list is shown once at startup when there is something to
        /// recover; returns true once. main() turns it off for dev/CI runs.
        Q_INVOKABLE bool takeStartupPrompt();
        void setStartupPromptEnabled(bool on) { _startupPrompt = on; }

    Q_SIGNALS:
        void countChanged();

    private:
        void rebuild();
        void saveNow();

        ScriptDraftStore _store;
        QSet<QString> _liveIds;          ///< Drafts owned by an open tab
        std::vector<QString> _visible;   ///< Ids of the recoverable drafts, newest first
        QString _filePath;
        bool _startupPrompt = true;
    };
}
