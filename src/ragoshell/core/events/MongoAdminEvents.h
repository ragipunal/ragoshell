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
     * @brief LoadDatabaseNames
     */

    class LoadDatabaseNamesRequest : public Event
    {
        R_EVENT

        LoadDatabaseNamesRequest(QObject *sender) :
            Event(sender) {}
    };

    class LoadDatabaseNamesResponse : public Event
    {
        R_EVENT

        LoadDatabaseNamesResponse(QObject *sender, const std::vector<std::string> &databaseNames) :
            Event(sender),
            databaseNames(databaseNames) {}

        LoadDatabaseNamesResponse(QObject *sender, const EventError &error) :
            Event(sender, error) {}

        std::vector<std::string> databaseNames;
    };


    /**
     * @brief LoadCollectionNames
     */

    class LoadCollectionNamesRequest : public Event
    {
        R_EVENT

    public:
        LoadCollectionNamesRequest(QObject *sender, const std::string &databaseName) :
            Event(sender),
            _databaseName(databaseName) {}

        std::string databaseName() const { return _databaseName; }

    private:
        std::string _databaseName;
    };

    class LoadCollectionNamesResponse : public Event
    {
        R_EVENT

    public:
        LoadCollectionNamesResponse(QObject *sender, const std::string &databaseName,
                                    const std::vector<MongoCollectionInfo> &collectionInfos) :
            Event(sender),
            _databaseName(databaseName),
            _collectionInfos(collectionInfos) { }

        LoadCollectionNamesResponse(QObject *sender, const EventError &error) :
            Event(sender, error) {}

        std::string databaseName() const { return _databaseName; }
        std::vector<MongoCollectionInfo> collectionInfos() const { return _collectionInfos; }

    private:
        std::string _databaseName;
        std::vector<MongoCollectionInfo> _collectionInfos;
    };

    class LoadCollectionIndexesRequest : public Event
    {
        R_EVENT
    public:
        LoadCollectionIndexesRequest(QObject *sender, const MongoCollectionInfo &collection) :
        Event(sender), _collection(collection) {}
        MongoCollectionInfo collection() const { return _collection; }
    private:
        const MongoCollectionInfo _collection;
    };

    class LoadCollectionIndexesResponse : public Event
    {
        R_EVENT
    public:
        LoadCollectionIndexesResponse(QObject *sender, const std::vector<IndexInfo> &indexes) :
            Event(sender), _indexes(indexes) {}

        LoadCollectionIndexesResponse(QObject *sender, const EventError &error) :
            Event(sender, error) {}
        std::vector<IndexInfo> indexes() const { return _indexes; }
    private:
        std::vector<IndexInfo> _indexes;
    };

    class AddEditIndexRequest : public Event
    {
        R_EVENT
            AddEditIndexRequest(QObject *sender, const IndexInfo &oldInfo, const IndexInfo &newInfo)
            : RagoShell::Event(sender), oldInfo_(oldInfo), newInfo_(newInfo) {}
        const IndexInfo &oldInfo() const
        {
            return oldInfo_;
        }
        const IndexInfo &newInfo() const
        {
            return newInfo_;
        }
    private:
        const IndexInfo oldInfo_;
        const IndexInfo newInfo_;
    };

    struct AddEditIndexResponse : public Event
    {
        R_EVENT
    
        AddEditIndexResponse(QObject *sender, const IndexInfo &oldIndex, const IndexInfo &newIndex)
            : Event(sender), oldIndex_(oldIndex), newIndex_(newIndex)
        {}

        AddEditIndexResponse(QObject *sender, const EventError &error, 
            const IndexInfo &oldIndex, const IndexInfo &newIndex) 
            : Event(sender, error), oldIndex_(oldIndex), newIndex_(newIndex)
        {}
    
        const IndexInfo oldIndex_;
        const IndexInfo newIndex_;
    };

    class DropCollectionIndexRequest : public Event
    {
        R_EVENT
    public:
        DropCollectionIndexRequest(QObject *sender, const MongoCollectionInfo &collection, 
            const std::string &index) 
            : Event(sender), _collection(collection), _index(index) 
        {}

        MongoCollectionInfo collection() const { return _collection; }
        std::string index() const { return _index; }
    private:
        const MongoCollectionInfo _collection;
        std::string _index;  // todo: rename to index
    };

    class DropCollectionIndexResponse: public Event
    {
        R_EVENT
    public:
        DropCollectionIndexResponse(QObject *sender, const MongoCollectionInfo &collection, 
                                    const std::string &index) 
            : Event(sender), _collection(collection), _index(index) 
        {}

        DropCollectionIndexResponse(QObject *sender, const EventError &error, 
                                    const std::string &index)
            : Event(sender, error), _index(index) 
        {}

        MongoCollectionInfo collection() const { return _collection; }
        std::string index() const { return _index; }
    private:
        MongoCollectionInfo _collection;
        std::string _index;
    };

    /**
     * @brief Load Users
     */

    class LoadUsersRequest : public Event
    {
        R_EVENT

    public:
        LoadUsersRequest(QObject *sender, const std::string &databaseName) :
            Event(sender),
            _databaseName(databaseName) {}

        std::string databaseName() const { return _databaseName; }

    private:
        std::string _databaseName;
    };

    class LoadUsersResponse : public Event
    {
        R_EVENT

    public:
        LoadUsersResponse(QObject *sender, const std::string &databaseName,
                                    const std::vector<MongoUser> &users) :
            Event(sender),
            _databaseName(databaseName),
            _users(users) { }

        LoadUsersResponse(QObject *sender, const EventError &error) :
            Event(sender, error) {}

        std::string databaseName() const { return _databaseName; }
        std::vector<MongoUser> users() const { return _users; }

    private:
        std::string _databaseName;
        std::vector<MongoUser> _users;
    };


    /**
     * @brief Load Functions
     */

    class LoadFunctionsRequest : public Event
    {
        R_EVENT

    public:
        LoadFunctionsRequest(QObject *sender, const std::string &databaseName) :
            Event(sender),
            _databaseName(databaseName) {}

        std::string databaseName() const { return _databaseName; }

    private:
        std::string _databaseName;
    };

    class LoadFunctionsResponse : public Event
    {
        R_EVENT

    public:
        LoadFunctionsResponse(QObject *sender, const std::string &databaseName,
                                    const std::vector<MongoFunction> &functions) :
            Event(sender),
            _databaseName(databaseName),
            _functions(functions) { }

        LoadFunctionsResponse(QObject *sender, const EventError &error) :
            Event(sender, error) {}

        std::string databaseName() const { return _databaseName; }
        std::vector<MongoFunction> functions() const { return _functions; }

    private:
        std::string _databaseName;
        std::vector<MongoFunction> _functions;
    };

    /**
     * @brief InsertDocument
     */

    class InsertDocumentRequest : public Event
    {
        R_EVENT

    public:
        InsertDocumentRequest(QObject *sender, bsoncxx::document::view_or_value obj, const MongoNamespace &ns, bool overwrite = false) :
            Event(sender),
            _obj(obj.view()),
            _ns(ns),
            _overwrite(overwrite) {}

        bsoncxx::document::view obj() const { return _obj.view(); }
        MongoNamespace ns() const { return _ns; }
        bool overwrite() const { return _overwrite; }

    private:
        bsoncxx::document::value _obj;
        const MongoNamespace _ns;
        bool _overwrite;
    };

    class InsertDocumentResponse : public Event
    {
        R_EVENT

    public:
        InsertDocumentResponse(QObject *sender) :
            Event(sender) {}

        InsertDocumentResponse(QObject *sender, EventError const& error) :
            Event(sender, error) {}
    };

    /**
     * @brief Remove Document
     */

    enum class RemoveDocumentCount { ONE, MULTI, ALL };

    class RemoveDocumentRequest : public Event
    {
        R_EVENT

    public:
        RemoveDocumentRequest(QObject *sender, bsoncxx::document::view_or_value query, const MongoNamespace &ns, 
                              RemoveDocumentCount removeCount, int index) :
            Event(sender),
            _query(query.view()),
            _ns(ns),
            _removeCount(removeCount),
            _index(index) {}

        bsoncxx::document::view query() const { return _query.view(); }
        MongoNamespace ns() const { return _ns; }
        RemoveDocumentCount removeCount() const { return _removeCount; }
        int index() const { return _index; }

    private:
        bsoncxx::document::value const _query;
        MongoNamespace const _ns;
        RemoveDocumentCount const _removeCount;
        // if this is a multi remove, this is the index of current document deleted. 0 for first document.
        int const _index;     
    };

    struct RemoveDocumentResponse : public Event
    {
        R_EVENT

        RemoveDocumentResponse(QObject *sender, RemoveDocumentCount removeCount, int index) :
            Event(sender), removeCount(removeCount), index(index) {}

        RemoveDocumentResponse(QObject *sender, const EventError &error, RemoveDocumentCount removeCount, int index) :
            Event(sender, error), removeCount(removeCount), index(index) {}

        RemoveDocumentCount const removeCount;
        int const index;
    };

    /**
     * @brief Create Database
     */

    class CreateDatabaseRequest : public Event
    {
        R_EVENT

    public:
        CreateDatabaseRequest(QObject *sender, const std::string &database) :
            Event(sender),
            _database(database) {}

        std::string database() const { return _database; }

    private:
        std::string const _database;
    };

    struct CreateDatabaseResponse : public Event
    {
        R_EVENT

        CreateDatabaseResponse(QObject *sender, const std::string &database) :
            Event(sender), database(database) {}

        CreateDatabaseResponse(QObject *sender, const std::string &database, const EventError &error) :
            Event(sender, error), database(database) {}

        std::string const database;
    };


    /**
     * @brief Drop Database
     */

    struct DropDatabaseRequest : public Event
    {
        R_EVENT

        DropDatabaseRequest(QObject *sender, const std::string &database) :
            Event(sender), database(database) {}

        std::string const database;
    };

    struct DropDatabaseResponse : public Event
    {
        R_EVENT

    public:
        DropDatabaseResponse(QObject *sender, const std::string &database) :
            Event(sender), database(database) {}

        DropDatabaseResponse(QObject *sender, const std::string &database, const EventError &error) :
            Event(sender, error), database(database) {}

        std::string const database;
    };

    /**
     * @brief Create Collection
     */

    class CreateCollectionRequest : public Event
    {
        R_EVENT

    public:
        CreateCollectionRequest(QObject *sender, const MongoNamespace &ns, 
                                                 bsoncxx::document::view_or_value extraOptions,
                                                 long long size = 0,
                                                 bool capped = false,
                                                 int maxDocNum = 0
                                                 ) :
            Event(sender), _ns(ns), _extraOptions(extraOptions.view()),
            _size(size), _capped(capped), _maxDocNum(maxDocNum) {}

        MongoNamespace ns() const { return _ns; }
        long long getSize() const { return _size; }
        bool getCapped() const { return _capped; }
        int getMaxDocNum() const { return _maxDocNum; }
        bsoncxx::document::view getExtraOptions() const { return _extraOptions.view(); }

    private:
        MongoNamespace const _ns;
        long long const _size;
        bool const _capped;
        int const _maxDocNum;
        bsoncxx::document::value const _extraOptions;
    };

    struct CreateCollectionResponse : public Event
    {
        R_EVENT

        CreateCollectionResponse(QObject *sender, std::string const& collection) :
        Event(sender), collection(collection) {}

        CreateCollectionResponse(QObject *sender, std::string const& collection, const EventError &error) :
            Event(sender, error), collection(collection) {}

        std::string const collection;
    };



    /**
     * @brief Drop Collection
     */

    class DropCollectionRequest : public Event
    {
        R_EVENT

    public:
        DropCollectionRequest(QObject *sender, const MongoNamespace &ns) :
            Event(sender),
            _ns(ns) {}

        MongoNamespace ns() const { return _ns; }

    private:
        MongoNamespace _ns;
    };

    struct DropCollectionResponse : public Event
    {
        R_EVENT

    public:
        DropCollectionResponse(QObject *sender, std::string const& collection) :
            Event(sender), collection(collection) {}

        DropCollectionResponse(QObject *sender, std::string const& collection, const EventError &error) :
            Event(sender, error), collection(collection) {}

        std::string const collection;
    };

    /**
     * @brief Rename Collection
     */

    class RenameCollectionRequest : public Event
    {
        R_EVENT

    public:
        RenameCollectionRequest(QObject *sender, const MongoNamespace &ns, const std::string &newCollection) :
            Event(sender),
            _ns(ns),
            _newCollection(newCollection) {}

        MongoNamespace ns() const { return _ns; }
        std::string newCollection() const { return _newCollection; }

    private:
        MongoNamespace const _ns;
        std::string const _newCollection;
    };

    struct RenameCollectionResponse : public Event
    {
        R_EVENT

            RenameCollectionResponse(QObject *sender, std::string const& oldCollection, 
                                     std::string const& newCollection) :
            Event(sender), oldCollection(oldCollection), newCollection(newCollection) {}

        RenameCollectionResponse(QObject *sender, const EventError &error) :
            Event(sender, error) {}

        std::string const oldCollection;
        std::string const newCollection;
    };

    /**
     * @brief Clone Collection
     */

    class DuplicateCollectionRequest : public Event
    {
        R_EVENT

    public:
        DuplicateCollectionRequest(QObject *sender, const MongoNamespace &ns, const std::string &newCollection) :
            Event(sender),
            _ns(ns),
            _newCollection(newCollection) {}

        MongoNamespace ns() const { return _ns; }
        std::string newCollection() const { return _newCollection; }

    private:
        MongoNamespace const _ns;
        std::string const _newCollection;
    };

    struct DuplicateCollectionResponse : public Event
    {
        R_EVENT

    public:
        DuplicateCollectionResponse(QObject *sender, std::string const& sourceCollection,
                                    std::string const& duplicateCollection) :
            Event(sender), sourceCollection(sourceCollection), duplicateCollection(duplicateCollection) {}

        DuplicateCollectionResponse(QObject *sender, std::string const& sourceCollection, const EventError &error) :
            Event(sender, error), sourceCollection(sourceCollection) {}

        std::string const sourceCollection;
        std::string const duplicateCollection;
    };

     /**
     * @brief Create User
     */

    class CreateUserRequest : public Event
    {
        R_EVENT

    public:
        CreateUserRequest(QObject *sender, const std::string &database, const MongoUser &user) :
            Event(sender),
            _database(database),
            _user(user) {}

        std::string database() const { return _database; }
        MongoUser user() const { return _user; }

    private:
        std::string _database;
        MongoUser _user;
    };

    struct CreateUserResponse : public Event
    {
        R_EVENT

        CreateUserResponse(QObject *sender, std::string const& userName) :
            Event(sender), userName(userName) {}

        CreateUserResponse(QObject *sender, std::string const& userName, const EventError &error) :
            Event(sender, error), userName(userName) {}

        std::string const userName;
    };


    /**
     * @brief Drop User
     */

    class DropUserRequest : public Event
    {
        R_EVENT

    public:
        DropUserRequest(QObject *sender, const std::string &database, std::string const& username) :
            Event(sender), _database(database), _username(username) {}

        std::string database() const { return _database; }
        std::string username() const { return _username; }

    private:
        std::string _database;
        std::string const _username;
    };

    struct DropUserResponse : public Event
    {
        R_EVENT

        DropUserResponse(QObject *sender, std::string const& username) :
            Event(sender), username(username) {}

        DropUserResponse(QObject *sender, std::string const& username, const EventError &error) :
            Event(sender, error), username(username) {}

        std::string const username;
    };


    /**
     * @brief Create Function
     */

    class CreateFunctionRequest : public Event
    {
        R_EVENT

    public:
        CreateFunctionRequest(QObject *sender, const std::string &database, float dbVersion, 
                              const MongoFunction &function, const std::string &existingFunctionName = std::string(), 
                              bool overwrite = false) :
            Event(sender),
            _database(database),
            _dbVersion(dbVersion),
            _existingFunctionName(existingFunctionName),
            _function(function),
            _overwrite(overwrite) {}

        std::string database() const { return _database; }
        float dbVersion() const { return _dbVersion; }
        std::string existingFunctionName() const { return _existingFunctionName; }
        MongoFunction function() const { return _function; }
        bool overwrite() const { return _overwrite; }

    private:
        std::string _database;
        float const _dbVersion = 0.0f;
        std::string _existingFunctionName;
        MongoFunction _function;
        bool _overwrite;
    };

    struct CreateFunctionResponse : public Event
    {
        R_EVENT
        
        enum Operation { NewFunction, NameUpdate, CodeUpdate }; // todo

        CreateFunctionResponse(QObject *sender, std::string const& functionName) :
            Event(sender), functionName(functionName) {}

        CreateFunctionResponse(QObject *sender, std::string const& functionName, const EventError &error) :
            Event(sender, error), functionName(functionName) {}

        std::string const functionName;
    };

    /**
     * @brief Drop Function
     */

    class DropFunctionRequest : public Event
    {
        R_EVENT

    public:
        DropFunctionRequest(QObject *sender, const std::string &database, float dbVersion, 
                            const std::string &funcName) :
            Event(sender),
            _database(database),
            _dbVersion(dbVersion),
            _functionName(funcName) {}

        std::string database() const { return _database; }
        float dbVersion() const { return _dbVersion; }
        std::string functionName() const { return _functionName; }

    private:
        std::string _database;
        float const _dbVersion = 0.0f;
        std::string _functionName;
    };

    struct DropFunctionResponse : public Event
    {
        R_EVENT

        DropFunctionResponse(QObject *sender, const std::string &functionName) :
        Event(sender), functionName(functionName) {}

        DropFunctionResponse(QObject *sender, const std::string &functionName, const EventError &error) :
            Event(sender, error), functionName(functionName) {}

        std::string const functionName;
    };


    class DatabaseListLoadedEvent : public Event
    {
        R_EVENT

        DatabaseListLoadedEvent(QObject *sender, const QList<MongoDatabase *> &list) :
            Event(sender),
            list(list) { }

        DatabaseListLoadedEvent(QObject *sender, const EventError &error) :
            Event(sender, error) {}

        QList<MongoDatabase *> list;
    };

}
