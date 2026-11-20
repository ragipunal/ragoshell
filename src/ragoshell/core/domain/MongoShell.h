#pragma once
#include <cstdint>
#include <QObject>
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/domain/ScriptInfo.h"
#include "ragoshell/core/domain/MongoAggregateInfo.h"

namespace RagoShell
{
    struct AggrInfo;

    class MongoShell : public QObject
    {
        Q_OBJECT

    public:
        MongoShell(MongoServer *server, ScriptInfo scriptInfo);

        void open(const std::string &script, const std::string &dbName = std::string());
        void query(int resultIndex, const MongoQueryInfo &info);
        void autocomplete(const std::string &prefix);
        void stop();
        uint64_t lastDispatchedOpGen() const { return _lastDispatchedOpGen; }
        MongoServer *server() const { return _server; }
        std::string query() const;
        void execute(const std::string &script = "", const std::string &dbName = "");
        bool isExecutable() const { return _scriptInfo.execute(); }
        const QString &title() const { return _scriptInfo.title(); }
        QString script() const { return _scriptInfo.script(); }
        std::string dbname() const { return _scriptInfo.dbname(); }
        const CursorPosition &cursor() const { return _scriptInfo.cursor(); }
        void setScript(const QString &script) { return _scriptInfo.setScript(script); }
        void setScriptExecutable(bool execute) { _scriptInfo.setExecutable(execute); }
        void setAggrInfo(AggrInfo const& aggrInfo) { _aggrInfo = aggrInfo; }
        QString filePath() const { return _scriptInfo.filePath(); }
        QString draftId() const { return _scriptInfo.draftId(); }

        bool saveToFile();
        bool saveToFileAs();
        bool loadFromFile();

    protected Q_SLOTS:
        void handle(ExecuteQueryResponse *event);
        void handle(ExecuteScriptResponse *event);
        void handle(AutocompleteResponse *event);

    private:        
        ScriptInfo _scriptInfo;
        AggrInfo _aggrInfo;
        MongoServer *_server;
        uint64_t _lastDispatchedOpGen = 0;
    };

}
