#pragma once

#include <cstdint>

#include <QString>
#include <QStringList>
#include <QEvent>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>

#include "ragoshell/core/domain/MongoShellResult.h"
#include "ragoshell/core/domain/CursorPosition.h"
#include "ragoshell/core/domain/MongoUser.h"
#include "ragoshell/core/domain/MongoFunction.h"
#include "ragoshell/core/events/MongoEventsInfo.h"
#include "ragoshell/core/domain/MongoAggregateInfo.h"
#include "ragoshell/core/Event.h"
#include "ragoshell/core/Enums.h"
#include "ragoshell/core/mongodb/ReplicaSet.h"

namespace RagoShell
{
    class MongoServer;
    class MongoShell;
    class MongoDatabase;
    class MongoWorker;
    class ConnectionSettings;
    class SshTunnelWorker;

    /**
     * @brief Query Mongodb
     */

    class ExecuteQueryRequest : public Event
    {
        R_EVENT

    public:
        ExecuteQueryRequest(QObject *sender, int resultIndex, const MongoQueryInfo &queryInfo,
                            uint64_t opGen = 0) :
            Event(sender),
            _resultIndex(resultIndex),
            _queryInfo(queryInfo),
            _opGen(opGen) {}

        int resultIndex() const { return _resultIndex; }
        MongoQueryInfo queryInfo() const { return _queryInfo; }
        uint64_t opGen() const { return _opGen; }

    private:
        int _resultIndex; //external user data;
        MongoQueryInfo _queryInfo;
        uint64_t _opGen = 0;
    };

    class ExecuteQueryResponse : public Event
    {
        R_EVENT

        ExecuteQueryResponse(QObject *sender, int resultIndex, const MongoQueryInfo &queryInfo,
                             const std::vector<MongoDocumentPtr> &documents, uint64_t opGen = 0) :
            Event(sender),
            resultIndex(resultIndex),
            queryInfo(queryInfo),
            documents(documents),
            _opGen(opGen) { }

        ExecuteQueryResponse(QObject *sender, const EventError &error,
                             bool cancelled = false, uint64_t opGen = 0) :
            Event(sender, error),
            _cancelled(cancelled),
            _opGen(opGen) {}

        bool cancelled() const { return _cancelled; }
        uint64_t opGen() const { return _opGen; }

        int resultIndex = 0;
        MongoQueryInfo queryInfo;
        std::vector<MongoDocumentPtr> documents;
        bool _cancelled = false;
        uint64_t _opGen = 0;
    };

    class AutocompleteRequest : public Event
    {
        R_EVENT

        AutocompleteRequest(QObject *sender, const std::string &prefix, const AutocompletionMode mode) :
            Event(sender),
            prefix(prefix),
            mode(mode) {}

        std::string prefix;
        AutocompletionMode mode;
    };

    class AutocompleteResponse : public Event
    {
        R_EVENT

        AutocompleteResponse(QObject *sender, const QStringList &list, const std::string &prefix) :
            Event(sender),
            list(list),
            prefix(prefix) {}

        AutocompleteResponse(QObject *sender, const EventError &error) :
            Event(sender, error) {}

        QStringList list;
        std::string prefix;
    };


    /**
     * @brief ExecuteScript
     */

    class ExecuteScriptRequest : public Event
    {
        R_EVENT

        ExecuteScriptRequest(QObject *sender, const std::string &script, const std::string &dbName, 
                             AggrInfo aggrInfo = AggrInfo(), int take = 0, int skip = 0,
                             uint64_t opGen = 0) :
            Event(sender),
            script(script),
            databaseName(dbName),
            take(take),
            skip(skip),
            aggrInfo(aggrInfo),
            opGen(opGen)
            {}

        std::string script;
        std::string databaseName;
        int take; //
        int skip;
        AggrInfo const aggrInfo;
        uint64_t opGen = 0;
    };

    class ExecuteScriptResponse : public Event
    {
        R_EVENT

        ExecuteScriptResponse(QObject *sender, const MongoShellExecResult &result, bool empty,
                              bool timeoutReached = false, bool cancelled = false, uint64_t opGen = 0) :
            Event(sender),
            result(result),
            empty(empty),
            _timeoutReached(timeoutReached),
            _cancelled(cancelled),
            _opGen(opGen) {}

        ExecuteScriptResponse(QObject *sender, const EventError &error, bool timeoutReached = false,
                              bool cancelled = false, uint64_t opGen = 0) :
            Event(sender, error),
            empty(false),
            _timeoutReached(timeoutReached),
            _cancelled(cancelled),
            _opGen(opGen) {}

        bool timeoutReached() const { return _timeoutReached; }
        bool cancelled() const { return _cancelled; }
        uint64_t opGen() const { return _opGen; }

        MongoShellExecResult result;
        bool empty = false;
        bool const _timeoutReached = false;
        bool const _cancelled = false;
        uint64_t const _opGen = 0;
    };


    class DocumentListLoadedEvent : public Event
    {
        R_EVENT

    public:
        DocumentListLoadedEvent(QObject *sender, int resultIndex, const MongoQueryInfo &queryInfo,
                                const std::string &query, const std::vector<MongoDocumentPtr> &docs,
                                uint64_t opGen = 0) :
            Event(sender),
            _resultIndex(resultIndex),
            _queryInfo(queryInfo),
            _documents(docs),
            _query(query),
            _opGen(opGen) { }

        DocumentListLoadedEvent(QObject *sender, const EventError &error,
                                bool cancelled = false, uint64_t opGen = 0) :
            Event(sender, error),
            _resultIndex(0),
            _cancelled(cancelled),
            _opGen(opGen) {}

        int resultIndex() const { return _resultIndex; }
        MongoQueryInfo queryInfo() const { return _queryInfo; }
        const std::vector<MongoDocumentPtr> &documents() const { return _documents; }
        std::string query() const { return _query; }
        bool cancelled() const { return _cancelled; }
        uint64_t opGen() const { return _opGen; }

    private:
        int _resultIndex = 0;
        MongoQueryInfo _queryInfo;
        std::vector<MongoDocumentPtr> _documents;
        std::string _query;
        bool _cancelled = false;
        uint64_t _opGen = 0;
    };

    class ScriptExecutedEvent : public Event
    {
        R_EVENT

    public:
        ScriptExecutedEvent(QObject *sender, const MongoShellExecResult &result, bool empty,
                            bool timeoutReached = false, bool cancelled = false, uint64_t opGen = 0) :
            Event(sender),
            _result(result),
            _empty(empty),
            _timeoutReached(timeoutReached),
            _cancelled(cancelled),
            _opGen(opGen) {}

        ScriptExecutedEvent(QObject *sender, const EventError &error, bool timeoutReached = false,
                            bool cancelled = false, uint64_t opGen = 0) :
            Event(sender, error),
            _empty(false),
            _timeoutReached(timeoutReached),
            _cancelled(cancelled),
            _opGen(opGen) {}

        MongoShellExecResult result() const { return _result; }
        bool empty() const { return _empty; }
        bool timeoutReached() const { return _timeoutReached; }
        bool cancelled() const { return _cancelled; }
        uint64_t opGen() const { return _opGen; }

    private:
        MongoShellExecResult _result;
        bool _empty = false;
        bool const _timeoutReached = false;
        bool const _cancelled = false;
        uint64_t const _opGen = 0;
    };

    class ScriptExecutingEvent : public Event
    {
        R_EVENT

    public:
        ScriptExecutingEvent(QObject *sender) :
            Event(sender) { }
    };


    class StopScriptRequest : public Event
    {
    R_EVENT

        StopScriptRequest(QObject *sender) :
            Event(sender) {}
    };
}
