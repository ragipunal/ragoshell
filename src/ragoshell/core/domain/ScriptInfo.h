#pragma once

#include <QString>

#include "ragoshell/core/domain/CursorPosition.h"

namespace RagoShell
{
    class ScriptInfo
    {
    public:
         ScriptInfo(const QString &script, bool execute = false, const std::string &dbname = "",
                   const CursorPosition &position = CursorPosition(),
                   const QString &title = QString(), const QString &filePath = QString(),
                   const QString &draftId = QString());

        bool execute() const { return _execute; }
        void setExecutable(bool execute) { _execute = execute; }
        QString script() const { return _script; }
        std::string dbname() const { return _dbname; }
        const QString &title() const { return _title; }
        const CursorPosition &cursor() const { return _cursor; }
        void setScript(const QString &script) { _script = script; }
        QString filePath() const { return _filePath; }
        /// Set for a tab recovered from a saved draft (ScriptDraftStore): its id and unsaved state carry over.
        QString draftId() const { return _draftId; }
        bool loadFromFile(const QString &filePath);
        bool loadFromFile();
        bool saveToFileAs();
        bool saveToFile();

    private:
        QString _script;
        std::string _dbname;
        bool _execute;
        const QString _title;
        const CursorPosition _cursor;
        QString _filePath;
        QString _draftId;
    };
}
