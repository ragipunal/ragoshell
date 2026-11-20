#include "ragoshell/core/mongodb/MongoWorker.h"

#include <exception>
#include <memory>

#include <QThread>
#include <mongocxx/exception/exception.hpp>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/domain/MongoCollectionInfo.h"
#include "ragoshell/core/domain/MongoShellResult.h"
#include "ragoshell/core/engine/ScriptEngine.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/mongodb/MongoClient.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{
    void MongoWorker::handle(LoadDatabaseNamesRequest *event)
    {
        try {
            // If user not an admin - he doesn't have access to mongodb 'listDatabases' command
            // Non admin user has access only to the single database he specified while performing auth.
            std::vector<std::string> dbNames = getDatabaseNamesSafe();

            // Remove from list of created databases existing databases
            for (std::vector<std::string>::iterator it = dbNames.begin(); it != dbNames.end(); ++it) {
                std::unordered_set<std::string>::const_iterator exists = _createdDbs.find(*it);
                if (exists != _createdDbs.end()) {
                    _createdDbs.erase(*it);
                }
            }

            // Merge with list of created databases
            for (std::unordered_set<std::string>::iterator it = _createdDbs.begin(); it != _createdDbs.end(); ++it) {
                dbNames.push_back(*it);
            }

            if (dbNames.size()) {
                reply(event->sender(), new LoadDatabaseNamesResponse(this, dbNames));
            } else {
                auto const errorStr{ "Failed to execute \"listdatabases\" command." };
                reply(event->sender(), new LoadDatabaseNamesResponse(this, EventError(errorStr)));
            }
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadDatabaseNamesResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    /**
     * @brief Load list of all collection names
     */
    void MongoWorker::handle(LoadCollectionNamesRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            std::vector<MongoCollectionInfo> const& collInfos = client->listCollectionInfos(event->databaseName());
            client->done();
            reply(event->sender(), new LoadCollectionNamesResponse(this, event->databaseName(), collInfos));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadCollectionNamesResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(LoadUsersRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            const std::vector<MongoUser> &users = client->getUsers(event->databaseName());
            client->done();

            reply(event->sender(), new LoadUsersResponse(this, event->databaseName(), users));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadUsersResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(LoadCollectionIndexesRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            const std::vector<IndexInfo> &ind = client->getIndexes(event->collection());
            client->done();

            reply(event->sender(), new LoadCollectionIndexesResponse(this, ind));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadCollectionIndexesResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    void MongoWorker::handle(AddEditIndexRequest *event)
    {
        const IndexInfo &newIndex = event->newInfo();
        const IndexInfo &oldIndex = event->oldInfo();
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->addEditIndex(oldIndex, newIndex);
            client->done();
            reply(event->sender(), new AddEditIndexResponse(this, oldIndex, newIndex));

            std::vector<IndexInfo> const &indexes = client->getIndexes(newIndex._collection);
            reply(event->sender(), new LoadCollectionIndexesResponse(this, indexes));
        } catch(const std::exception &ex) {
            reply(event->sender(), 
                new AddEditIndexResponse(this, EventError(ex.what()), oldIndex, newIndex)
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropCollectionIndexRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->dropIndexFromCollection(event->collection(), event->index());
            client->done();
            reply(event->sender(), 
                new DropCollectionIndexResponse(this, event->collection(), event->index()));
        } catch(const std::exception &ex) {
            reply(event->sender(), 
                new DropCollectionIndexResponse(this, EventError(ex.what()), event->index()));
            // Logging handled in main thread
        }            
    }

    void MongoWorker::handle(LoadFunctionsRequest *event)
    {
        try {
            if (event->databaseName() == "$external") {
                reply(event->sender(), new LoadFunctionsResponse(this, event->databaseName(), {}));
                return;
            }
            std::unique_ptr<MongoClient> client(getClient());
            const std::vector<MongoFunction> &funcs = client->getFunctions(event->databaseName());
            client->done();

            reply(event->sender(), new LoadFunctionsResponse(this, event->databaseName(), funcs));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadFunctionsResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(InsertDocumentRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
    
            if (event->overwrite())
                client->saveDocument(event->obj(), event->ns());
            else
                client->insertDocument(event->obj(), event->ns());

            client->done();
            reply(event->sender(), new InsertDocumentResponse(this));
        } 
        catch(const std::exception &ex) {
            reply(event->sender(), new InsertDocumentResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    void MongoWorker::handle(RemoveDocumentRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());

            client->removeDocuments(event->ns(), event->query(), 
                                    event->removeCount() == RemoveDocumentCount::ONE);
            client->done();

            reply(event->sender(), new RemoveDocumentResponse(this, event->removeCount(), event->index()));
        } 
        catch(const std::exception &ex) {
            reply(event->sender(), new RemoveDocumentResponse(this, EventError(ex.what()), 
                event->removeCount(), event->index()));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(ExecuteQueryRequest *event)
    {
        if (isOpCancelled(event->opGen())) {
            reply(event->sender(),
                new ExecuteQueryResponse(this, EventError("Operation cancelled by user."),
                                         /*cancelled=*/true, event->opGen()));
            return;
        }

        try {
            std::unique_ptr<MongoClient> client { getClient() };
            if (isOpCancelled(event->opGen())) {
                reply(event->sender(),
                    new ExecuteQueryResponse(this, EventError("Operation cancelled by user."),
                                             /*cancelled=*/true, event->opGen()));
                return;
            }
            std::vector<MongoDocumentPtr> docs = client->query(event->queryInfo());
            client->done();
            if (isOpCancelled(event->opGen())) {
                reply(event->sender(),
                    new ExecuteQueryResponse(this, EventError("Operation cancelled by user."),
                                             /*cancelled=*/true, event->opGen()));
                return;
            }
            reply(event->sender(),
                new ExecuteQueryResponse(this, event->resultIndex(), event->queryInfo(),
                                         docs, event->opGen())
            );
        } catch(const std::exception &ex) {
            const bool wasCancelled = isOpCancelled(event->opGen());
            reply(event->sender(),
                new ExecuteQueryResponse(this, EventError(ex.what()),
                                         wasCancelled, event->opGen()));
            if (!wasCancelled)
                sendLog(this, LogEvent::RBM_ERROR, std::string(ex.what()));
        }
    }

    /**
     * @brief Execute javascript
     */
    void MongoWorker::handle(ExecuteScriptRequest *event)
    {        
        try {           
            ScriptEngine *engine = nullptr;
            {
                QMutexLocker lock(&_engineMutex);
                if (isOpCancelled(event->opGen)) {
                    reply(event->sender(),
                        new ExecuteScriptResponse(this, EventError("Operation cancelled by user."),
                                                  false, /*cancelled=*/true, event->opGen));
                    return;
                }
                if (!_scriptEngine) {
                    auto eng = std::make_unique<ScriptEngine>(_connSettings, _shellTimeoutSec);
                    eng->setBatchSize(_batchSize);
                    eng->setStartupScript(_startupScript);
                    _scriptEngine = std::move(eng);
                }
                _scriptEngine->clearInterrupt();
                engine = _scriptEngine.get();
            }

            if (_timerId == -1) {
                constexpr int PING_INTERVAL_MSEC { 60 * 1000 }; // 60 seconds
                _timerId = startTimer(PING_INTERVAL_MSEC);
            }

            const std::string targetDb = !event->databaseName.empty()
                ? event->databaseName
                : _connSettings->defaultDatabase();

            MongoShellExecResult result {
                engine->exec(
                    event->script, targetDb, event->aggrInfo
                )
            };

            if (isOpCancelled(event->opGen) || result.interrupted()) {
                reply(
                    event->sender(),
                    new ExecuteScriptResponse(this, EventError("Operation cancelled by user."),
                                              false, /*cancelled=*/true, event->opGen)
                );
                return;
            }

            if (!result.error()) {
                if (_connSettings->isReplicaSet() && result.currentServer().empty()
                    && !_connSettings->serverHost().empty()) {
                    result.setCurrentServer(_connSettings->getFullAddress());
                }
                reply(
                    event->sender(),
                    new ExecuteScriptResponse(this, result, event->script.empty(), 
                        result.timeoutReached(), /*cancelled=*/false, event->opGen)
                );
                return;
            }

            // Never retry on user cancellation, timeout, or normal JS runtime/syntax errors;
            // only retry once if the shell scope itself had died unexpectedly.
            if (!result.timeoutReached() && engine->failedScope() && !isOpCancelled(event->opGen)) {
                retry(event);
                return;
            }

            reply(
                event->sender(),
                new ExecuteScriptResponse(this, EventError(result.errorMessage()),
                                          result.timeoutReached(), /*cancelled=*/false, event->opGen)
            );
        } 
        catch(const std::exception &ex) {
            const bool wasCancelled = isOpCancelled(event->opGen);
            auto const error { EventError(ex.what(), EventError::Unknown) };
            reply(event->sender(),
                  new ExecuteScriptResponse(this, error, false, wasCancelled, event->opGen));
            if (!wasCancelled)
                sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    void MongoWorker::retry(ExecuteScriptRequest * event)
    {
        if (isOpCancelled(event->opGen)) {
            reply(event->sender(),
                new ExecuteScriptResponse(this, EventError("Operation cancelled by user."),
                                          false, /*cancelled=*/true, event->opGen));
            return;
        }

        const std::string targetDb = !event->databaseName.empty()
            ? event->databaseName
            : _connSettings->defaultDatabase();
        MongoShellExecResult result {
            _scriptEngine->exec(event->script, targetDb, event->aggrInfo)
        };
        if (isOpCancelled(event->opGen) || result.interrupted()) {
            reply(event->sender(),
                new ExecuteScriptResponse(this, EventError("Operation cancelled by user."),
                                          false, /*cancelled=*/true, event->opGen));
        }
        else if (result.error()) {
            auto const error { EventError(result.errorMessage()) };
            reply(event->sender(),
                new ExecuteScriptResponse(this, error, result.timeoutReached(),
                                          /*cancelled=*/false, event->opGen));
        }
        else {
            if (_connSettings->isReplicaSet() && result.currentServer().empty()
                && !_connSettings->serverHost().empty()) {
                result.setCurrentServer(_connSettings->getFullAddress());
            }
            reply(
                event->sender(),
                new ExecuteScriptResponse(this, result, event->script.empty(),
                    result.timeoutReached(), /*cancelled=*/false, event->opGen)
            );
        }
    }

    /**
     * @brief Interrupt javascript execution
     */
    void MongoWorker::handle(StopScriptRequest *)
    {
        interrupt();
    }

    void MongoWorker::handle(AutocompleteRequest *event)
    {
        try {
            ScriptEngine *engine = ensureScriptEngine();
            QStringList list = engine->complete(event->prefix, event->mode);
            reply(event->sender(), new AutocompleteResponse(this, list, event->prefix));
        } catch(const std::exception &ex) {
            reply(event->sender(), new AutocompleteResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, std::string(ex.what()));            
        }
    }

    void MongoWorker::handle(CreateDatabaseRequest *event)
    {
        std::string dbname = event->database();
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->createDatabase(dbname);

            // Insert to list of created database. Read docs for this hashset in the header
            _createdDbs.insert(dbname);

            reply(event->sender(), new CreateDatabaseResponse(this, dbname));
        } catch(const std::exception &ex) {
            reply(event->sender(), new CreateDatabaseResponse(this, dbname, 
                EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropDatabaseRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->dropDatabase(event->database);

            // Remove from the list of created database, Read docs for this hashset in the header
            _createdDbs.erase(event->database);

            reply(event->sender(), new DropDatabaseResponse(this, event->database));
        } 
        catch(const std::exception &ex) {
            reply(event->sender(), 
                new DropDatabaseResponse(this, event->database, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(CreateCollectionRequest *event)
    {
        std::string const& collection = event->ns().collectionName();

        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->createCollection(event->ns().toString(), event->getSize(), event->getCapped(),
                event->getMaxDocNum(), event->getExtraOptions());
            client->done();

            reply(event->sender(), new CreateCollectionResponse(this, collection));
        } catch(const std::exception &ex) {
            reply(event->sender(), 
                new CreateCollectionResponse(this, collection, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropCollectionRequest *event)
    {
        std::string const& collection = event->ns().collectionName();

        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->dropCollection(event->ns());
            client->done();

            reply(event->sender(), new DropCollectionResponse(this, collection));
        } catch(const std::exception &ex) {
            reply(event->sender(), 
                new DropCollectionResponse(this, collection, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(RenameCollectionRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->renameCollection(event->ns(), event->newCollection());
            client->done();

            reply(event->sender(), new RenameCollectionResponse(this, event->ns().collectionName(),
                                                                event->newCollection()));
        } catch(const std::exception &ex) {
            reply(event->sender(), new RenameCollectionResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DuplicateCollectionRequest *event)
    {
        std::string const& sourceCollection = event->ns().collectionName();

        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->duplicateCollection(event->ns(), event->newCollection());
            client->done();

            reply(event->sender(), 
                new DuplicateCollectionResponse(this, sourceCollection, event->newCollection())
            );
        }
        catch (const std::exception &ex) {
            reply(event->sender(), 
                new DuplicateCollectionResponse(this, sourceCollection, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }
    
    void MongoWorker::handle(CreateUserRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->createUser(event->database(), event->user());
            client->done();

            reply(event->sender(), new CreateUserResponse(this, event->user().name()));
        } catch(const std::exception &ex) {
            reply(event->sender(), 
                new CreateUserResponse(this, event->user().name(), EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropUserRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->dropUser(event->database(), event->username());
            client->done();

            reply(event->sender(), new DropUserResponse(this, event->username()));
        } catch(const std::exception &ex) {
            reply(event->sender(), 
                new DropUserResponse(this, event->username(), EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(CreateFunctionRequest *event)
    {
        std::string const& functionName = event->function().name();

        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->createFunction(event->database(), event->function(), event->existingFunctionName());
            client->done();

            reply(event->sender(), new CreateFunctionResponse(this, functionName));
        } catch(const std::exception &ex) {
            reply(event->sender(), 
                new CreateFunctionResponse(this, functionName, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropFunctionRequest *event)
    {
        try {
            std::unique_ptr<MongoClient> client(getClient());
            client->dropFunction(event->database(), event->functionName());
            client->done();

            reply(event->sender(), new DropFunctionResponse(this, event->functionName()));
        }
        catch (const std::exception &ex) {
            reply(event->sender(), 
                new DropFunctionResponse(this, event->functionName(), EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

}
