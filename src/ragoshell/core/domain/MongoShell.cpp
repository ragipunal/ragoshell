#include "ragoshell/core/domain/MongoShell.h"

#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/mongodb/MongoWorker.h"
#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/settings/SettingsManager.h"

namespace RagoShell
{  
    auto const& eventBus = []() { return AppRegistry::instance().bus(); };

    MongoShell::MongoShell(MongoServer *server, ScriptInfo scriptInfo) :
        QObject(),
        _scriptInfo(scriptInfo),
        _server(server)      
    {
    }

    void MongoShell::open(const std::string &script, const std::string &dbName)
    {
        _lastDispatchedOpGen = (_server && _server->worker())
            ? _server->worker()->beginNewOperation()
            : 0;
        eventBus()->publish(new ScriptExecutingEvent(this));
        _scriptInfo.setScript(QtUtils::toQString(script));
        eventBus()->send(_server->worker(),
            new ExecuteScriptRequest(this, query(), dbName, AggrInfo(), 0, 0, _lastDispatchedOpGen));
        LOG_MSG(_scriptInfo.script(), RagoShell::LogSeverity::Info);
    }

    std::string MongoShell::query() const 
    {
        return QtUtils::toStdString(_scriptInfo.script()); 
    }

    void MongoShell::execute(const std::string &script /* = "" */, 
                             const std::string &dbName /* = "" */)
    {
        if (!_scriptInfo.execute())
            return;

        std::string const finalScript = script.empty() ? query() : script;
        _lastDispatchedOpGen = (_server && _server->worker())
            ? _server->worker()->beginNewOperation()
            : 0;
        eventBus()->publish(new ScriptExecutingEvent(this));
        eventBus()->send(_server->worker(), 
            new ExecuteScriptRequest(this, finalScript, dbName, _aggrInfo, 0, 0, _lastDispatchedOpGen));
        if (!_scriptInfo.script().isEmpty())
            LOG_MSG(_scriptInfo.script(), RagoShell::LogSeverity::Info);
    }

    void MongoShell::query(int resultIndex, const MongoQueryInfo &info)
    {
        _lastDispatchedOpGen = (_server && _server->worker())
            ? _server->worker()->beginNewOperation()
            : 0;
        eventBus()->send(_server->worker(),
            new ExecuteQueryRequest(this, resultIndex, info, _lastDispatchedOpGen));
    }

    void MongoShell::autocomplete(const std::string &prefix)
    {
        AutocompletionMode autocompletionMode {
            AppRegistry::instance().settingsManager()->autocompletionMode()
        };
        if (autocompletionMode == AutocompleteNone)
            return;

        eventBus()->send(_server->worker(), 
            new AutocompleteRequest(this, prefix, autocompletionMode)
        );
    }

    void MongoShell::stop()
    {
        if (_server && _server->worker())
            _server->worker()->interrupt();
    }

    bool MongoShell::loadFromFile()
    {
        return _scriptInfo.loadFromFile();
    }

    bool MongoShell::saveToFileAs()
    {
        return _scriptInfo.saveToFileAs();
    }

    bool MongoShell::saveToFile()
    {
        return _scriptInfo.saveToFile();
    }

    void MongoShell::handle(ExecuteQueryResponse *event)
    {
        if (event->isError()) {
            eventBus()->publish(
                new DocumentListLoadedEvent(this, event->error(), event->cancelled(), event->opGen()));
            return;
        }

        eventBus()->publish(
            new DocumentListLoadedEvent(this, 
                event->resultIndex, event->queryInfo, query(), event->documents, event->opGen())
        );
    }

    void MongoShell::handle(ExecuteScriptResponse *event)
    {
        if (!event->isError()) {
            eventBus()->publish(
                new ScriptExecutedEvent(this, event->result, event->empty,
                                        event->timeoutReached(), event->cancelled(), event->opGen())
            );
            return;
        }

        if (!event->cancelled() && !event->timeoutReached()
            && _server->connectionRecord()->isReplicaSet()
            && event->error().errorCode() == EventError::SetPrimaryUnreachable) {
            eventBus()->publish(
                new ReplicaSetRefreshed(this, event->error(), event->error().replicaSetInfo())
            );
        }

        eventBus()->publish(
            new ScriptExecutedEvent(this, event->error(), event->timeoutReached(),
                                    event->cancelled(), event->opGen())
        );
    }

    void MongoShell::handle(AutocompleteResponse *event)
    {
        if (event->isError()) {
            eventBus()->publish(new AutocompleteResponse(this, event->error()));
            return;
        }

        eventBus()->publish(new AutocompleteResponse(this, event->list, event->prefix));
    }
}
