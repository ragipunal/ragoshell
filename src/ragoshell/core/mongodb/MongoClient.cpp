#include "ragoshell/core/mongodb/MongoClient.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/replace.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/transport/IMongoTransport.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"

namespace
{
    RagoShell::IndexInfo makeIndexInfoFromBsonObj(
        const RagoShell::MongoCollectionInfo &collection,
        const bsoncxx::document::view &obj)
    {
        using namespace RagoShell::BsonUtils;
        RagoShell::IndexInfo info(collection);
        info._name = getField<std::string>(obj, "name");
        const auto keyElem = obj["key"];
        if (keyElem && keyElem.type() == bsoncxx::type::k_document)
            info._keys = jsonString(keyElem.get_document().view(), RagoShell::TenGen, 1,
                                    RagoShell::DefaultEncoding, RagoShell::Utc);

        info._unique = getField<bool>(obj, "unique");
        info._backGround = getField<bool>(obj, "background");
        info._sparse = getField<bool>(obj, "sparse");
        info._hidden = getField<bool>(obj, "hidden");
        if (const auto ttlElem = obj["expireAfterSeconds"])
            info._ttl = getField<int>(obj, "expireAfterSeconds");
        else
            info._ttl = -1;
        info._defaultLanguage = getField<std::string>(obj, "default_language");
        info._languageOverride = getField<std::string>(obj, "language_override");
        const auto weightsElem = obj["weights"];
        if (weightsElem && weightsElem.type() == bsoncxx::type::k_document)
            info._textWeights = jsonString(weightsElem.get_document().view(), RagoShell::TenGen, 1,
                                           RagoShell::DefaultEncoding, RagoShell::Utc);

        const auto partialElem = obj["partialFilterExpression"];
        if (partialElem && partialElem.type() == bsoncxx::type::k_document)
            info._partialFilterExpression = jsonString(partialElem.get_document().view(), RagoShell::TenGen, 1,
                                                       RagoShell::DefaultEncoding, RagoShell::Utc);

        const auto collationElem = obj["collation"];
        if (collationElem && collationElem.type() == bsoncxx::type::k_document)
            info._collation = jsonString(collationElem.get_document().view(), RagoShell::TenGen, 1,
                                         RagoShell::DefaultEncoding, RagoShell::Utc);

        const auto wildcardElem = obj["wildcardProjection"];
        if (wildcardElem && wildcardElem.type() == bsoncxx::type::k_document)
            info._wildcardProjection = jsonString(wildcardElem.get_document().view(), RagoShell::TenGen, 1,
                                                  RagoShell::DefaultEncoding, RagoShell::Utc);

        return info;
    }

    /**
     * @brief Parses a JSON / Mongo-Shell JSON string into a BSON document.
     */
    bsoncxx::document::value parseJson(const std::string &str)
    {
        bsoncxx::document::value parsed = bsoncxx::from_json("{}");
        QString err;
        if (RagoShell::MongoJsonUtils::parseSingleDocument(QString::fromUtf8(str.c_str()), parsed, err))
            return parsed;
        return bsoncxx::from_json(str);
    }

    bool sameJsonDoc(const std::string &a, const std::string &b)
    {
        const QString qa = QString::fromUtf8(a.c_str()).trimmed();
        const QString qb = QString::fromUtf8(b.c_str()).trimmed();
        const bool aEmpty = qa.isEmpty() || qa == QLatin1String("{}");
        const bool bEmpty = qb.isEmpty() || qb == QLatin1String("{}");
        if (aEmpty && bEmpty)
            return true;
        if (aEmpty != bEmpty)
            return false;
        if (qa == qb)
            return true;
        try {
            const auto docA = parseJson(a);
            const auto docB = parseJson(b);
            return docA.view() == docB.view();
        } catch (const std::exception &) {
            return false;
        }
    }

    bool isAdminOnlyBuiltinRole(const std::string &roleName)
    {
        return roleName == "clusterMonitor" ||
               roleName == "clusterManager" ||
               roleName == "clusterAdmin" ||
               roleName == "hostManager" ||
               roleName == "readAnyDatabase" ||
               roleName == "readWriteAnyDatabase" ||
               roleName == "userAdminAnyDatabase" ||
               roleName == "dbAdminAnyDatabase" ||
               roleName == "backup" ||
               roleName == "restore" ||
               roleName == "root";
    }
}

namespace RagoShell
{
    MongoClient::MongoClient(mongocxx::client *const client) :
        _client(client) { }

    MongoClient::MongoClient(IMongoTransport *transport) :
        _transport(transport) { }

    bsoncxx::document::value MongoClient::runCommandChecked(
        const std::string &db, bsoncxx::document::view_or_value cmd) const
    {
        if (_transport) {
            const auto res = _transport->runCommand(
                QString::fromStdString(db),
                QString::fromStdString(
                    bsoncxx::to_json(cmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
            if (!res.ok) {
                const QString msg = res.codeName.isEmpty()
                    ? res.error
                    : QStringLiteral("%1 (%2 %3)").arg(res.error).arg(res.codeName).arg(res.code);
                throw std::runtime_error(msg.toStdString());
            }
            return bsoncxx::from_json(res.ejson.toStdString());
        }

        try {
            return _client->database(db).run_command(cmd);
        } catch (const mongocxx::exception &e) {
            throw std::runtime_error(e.what());
        }
    }

    std::vector<bsoncxx::document::value> MongoClient::execCursor(
        const std::string &db, bsoncxx::document::view_or_value cmd) const
    {
        std::vector<bsoncxx::document::value> docs;
        const auto res = runCommandChecked(db, std::move(cmd));
        const auto batch = res.view()["cursor"]["firstBatch"];
        if (batch && batch.type() == bsoncxx::type::k_array) {
            for (auto const& e : batch.get_array().value) {
                if (e.type() == bsoncxx::type::k_document)
                    docs.emplace_back(e.get_document().view());
            }
        }
        return docs;
    }

    std::vector<bsoncxx::document::value> MongoClient::listCollectionDocs(
        const std::string &db, bool nameOnly) const
    {
        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            return execCursor(db, basic::make_document(
                basic::kvp("listCollections", 1),
                basic::kvp("nameOnly", nameOnly)));
        }

        std::vector<bsoncxx::document::value> docs;
        for (auto const& coll : _client->database(db).list_collections())
            docs.emplace_back(bsoncxx::document::value(coll));
        return docs;
    }

    std::vector<std::string> MongoClient::getCollectionNamesWithDbname(const std::string &dbname) const
    {
        std::vector<std::string> collNames;
        for (auto const& coll : listCollectionDocs(dbname))
            collNames.push_back(dbname + '.' + BsonUtils::getField<std::string>(coll.view(), "name"));

        std::sort(collNames.begin(), collNames.end());
        return collNames;
    }

    std::vector<MongoCollectionInfo> MongoClient::listCollectionInfos(const std::string &dbname) const
    {
        std::vector<MongoCollectionInfo> infos;
        if (dbname.empty() || dbname == "$external")
            return infos;

        for (auto const& coll : listCollectionDocs(dbname)) {
            const std::string collName = BsonUtils::getField<std::string>(coll.view(), "name");
            if (collName.empty())
                continue;

            MongoCollectionInfo info(dbname + '.' + collName);
            if (!info.ns().isValid())
                continue;

            std::string type = BsonUtils::getField<std::string>(coll.view(), "type");
            const auto optsEl = coll["options"];
            if (optsEl && optsEl.type() == bsoncxx::type::k_document) {
                const auto opts = optsEl.get_document().view();
                const std::string viewTarget = BsonUtils::getField<std::string>(opts, "viewOn");
                if (!viewTarget.empty()) {
                    info.setViewOn(viewTarget);
                    if (type.empty())
                        type = "view";
                }
                if (opts["timeseries"] && type.empty())
                    type = "timeseries";
                if (BsonUtils::getField<bool>(opts, "capped"))
                    info.setCapped(true);
            }
            info.setStorageType(type.empty() ? std::string("collection") : type);
            infos.push_back(std::move(info));
        }

        std::sort(infos.begin(), infos.end(), [](const MongoCollectionInfo &a, const MongoCollectionInfo &b) {
            return a.fullName() < b.fullName();
        });
        return infos;
    }

    float MongoClient::getVersion() const
    {
        const std::string resultStr = dbVersionStr();
        return resultStr.empty() ? 0.0f : static_cast<float>(std::atof(resultStr.c_str()));
    }

    std::string MongoClient::dbVersionStr() const
    {
        try {
            auto resultObj = runCommandChecked("admin",
                bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("buildInfo", 1)));
            return BsonUtils::getField<std::string>(resultObj.view(), "version");
        } catch (const std::exception &) {
            return std::string();
        }
    }

    std::string MongoClient::getStorageEngineType() const
    {
        namespace basic = bsoncxx::builder::basic;
        try {
            auto helloObj = runCommandChecked("admin",
                basic::make_document(basic::kvp("ismaster", 1)));
            if (BsonUtils::getField<std::string>(helloObj.view(), "msg") == "isdbgrid")
                return "mongos";
        } catch (const std::exception &) {
            // Fall through to serverStatus
        }

        try {
            auto resultObj = runCommandChecked("admin",
                basic::make_document(basic::kvp("serverStatus", 1)));
            if (BsonUtils::getField<std::string>(resultObj.view(), "process") == "mongos")
                return "mongos";
            const auto engine = resultObj.view()["storageEngine"];
            if (engine && engine.type() == bsoncxx::type::k_document)
                return BsonUtils::getField<std::string>(engine.get_document().view(), "name");
        } catch (const std::exception &) {
            // Restricted single-DB users lack clusterMonitor privilege for serverStatus;
            // return empty cleanly without failing connection establishment.
        }
        return std::string();
    }

    std::vector<std::string> MongoClient::getDatabaseNames() const
    {
        if (_transport) {
            std::vector<std::string> names;
            const auto res = _transport->listDatabases();
            if (!res.ok)
                throw std::runtime_error(res.error.toStdString());
            const auto doc = bsoncxx::from_json(
                "{\"databases\":" + res.ejson.toStdString() + "}");
            const auto dbs = doc.view()["databases"];
            if (dbs && dbs.type() == bsoncxx::type::k_array) {
                for (auto const& d : dbs.get_array().value) {
                    if (d.type() == bsoncxx::type::k_document)
                        names.push_back(BsonUtils::getField<std::string>(d.get_document().view(), "name"));
                }
            }
            std::sort(names.begin(), names.end());
            return names;
        }
        auto dbs = _client->list_database_names();
        std::vector<std::string> dbNames(dbs.begin(), dbs.end());
        std::sort(dbNames.begin(), dbNames.end());
        return dbNames;
    }

    std::vector<MongoUser> MongoClient::getUsers(const std::string &dbName)
    {
        namespace basic = bsoncxx::builder::basic;
        auto result = runCommandChecked(dbName, basic::make_document(basic::kvp("usersInfo", 1)));

        const float ver = getVersion();
        std::vector<MongoUser> users;
        const auto usersField = result.view()["users"];
        if (usersField && usersField.type() == bsoncxx::type::k_array) {
            for (auto const& usr : usersField.get_array().value) {
                if (usr.type() == bsoncxx::type::k_document)
                    users.push_back(MongoUser(ver, usr.get_document().view()));
            }
        }

        return users;
    }

    void MongoClient::createUser(const std::string &dbName, const MongoUser &user)
    {
        namespace basic = bsoncxx::builder::basic;

        const bool externalDb = (dbName == "$external");
        const std::string defaultRoleDb = externalDb ? "admin" : dbName;
        const std::string targetRoleDb = (user.userSource().empty() || user.userSource() == "$external")
            ? defaultRoleDb
            : user.userSource();

        basic::array roles;
        for (auto const& roleStr : user.roles()) {
            basic::document role;
            const auto atPos = roleStr.find('@');
            if (atPos != std::string::npos && atPos > 0 && atPos + 1 < roleStr.size()) {
                role.append(basic::kvp("role", roleStr.substr(0, atPos)),
                            basic::kvp("db", roleStr.substr(atPos + 1)));
            } else if ((dbName == "admin" || externalDb) && isAdminOnlyBuiltinRole(roleStr)) {
                role.append(basic::kvp("role", roleStr), basic::kvp("db", "admin"));
            } else {
                role.append(basic::kvp("role", roleStr), basic::kvp("db", targetRoleDb));
            }
            roles.append(role.extract());
        }

        basic::document cmd;
        if (user.overwrite()) {
            cmd.append(basic::kvp("updateUser", user.name()));
            if (!externalDb && !user.password().empty()) {
                cmd.append(basic::kvp("pwd", user.password()));
                if (!user.mechanisms().empty()) {
                    basic::array mechs;
                    for (auto const& m : user.mechanisms()) {
                        if (!m.empty())
                            mechs.append(m);
                    }
                    cmd.append(basic::kvp("mechanisms", mechs.extract()));
                }
            }
            cmd.append(basic::kvp("roles", roles.extract()));
        } else {
            cmd.append(basic::kvp("createUser", user.name()));
            if (!externalDb) {
                cmd.append(basic::kvp("pwd", user.password()));
                if (!user.mechanisms().empty()) {
                    basic::array mechs;
                    for (auto const& m : user.mechanisms()) {
                        if (!m.empty())
                            mechs.append(m);
                    }
                    cmd.append(basic::kvp("mechanisms", mechs.extract()));
                }
            }
            cmd.append(basic::kvp("roles", roles.extract()));
        }
        runCommandChecked(dbName, cmd.extract());
    }

    void MongoClient::dropUser(const std::string &dbName, const std::string &user)
    {
        namespace basic = bsoncxx::builder::basic;
        runCommandChecked(dbName, basic::make_document(basic::kvp("dropUser", user)));
    }

    std::vector<MongoFunction> MongoClient::getFunctions(const std::string &dbName) const
    {
        std::vector<MongoFunction> functions;

        namespace basic = bsoncxx::builder::basic;
        if (_transport) {
            for (auto const& doc : execCursor(dbName, basic::make_document(
                     basic::kvp("find", "system.js"),
                     basic::kvp("sort", basic::make_document(basic::kvp("_id", 1)))))) {
                try {
                    functions.push_back(MongoFunction(doc.view()));
                } catch (const std::exception &) {
                }
            }
            return functions;
        }

        mongocxx::options::find opts;
        opts.sort(basic::make_document(basic::kvp("_id", 1)));

        auto cursor = _client->database(dbName).collection("system.js").find({}, opts);
        for (auto const& doc : cursor) {
            try {
                MongoFunction func(doc);
                functions.push_back(func);
            } catch (const std::exception &) {
                // skip invalid docs
            }
        }
        return functions;
    }

    std::vector<IndexInfo> MongoClient::getIndexes(const MongoCollectionInfo &collection) const
    {
        std::vector<IndexInfo> result;
        const MongoNamespace &ns = collection.ns();
        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            for (auto const& doc : execCursor(ns.databaseName(), basic::make_document(
                     basic::kvp("listIndexes", ns.collectionName()))))
                result.push_back(makeIndexInfoFromBsonObj(collection, doc.view()));
            return result;
        }

        auto cursor = _client->database(ns.databaseName())
            .collection(ns.collectionName()).indexes().list();

        for (auto const& doc : cursor)
            result.push_back(makeIndexInfoFromBsonObj(collection, doc));

        return result;
    }

    void MongoClient::addEditIndex(const IndexInfo &oldInfo, const IndexInfo &newInfo) const
    {
        bool const editIndex = !oldInfo._name.empty();

        const MongoNamespace &ns = newInfo._collection.ns();

        // MongoDB 4.4+ Hidden Index fast-path: if only the `hidden` flag is being
        // toggled on an existing index (keys, name, and all other options unchanged),
        // update index visibility via O(1) metadata `collMod` without dropping/rebuilding!
        if (editIndex &&
            oldInfo._name == newInfo._name &&
            sameJsonDoc(oldInfo._keys, newInfo._keys) &&
            oldInfo._unique == newInfo._unique &&
            oldInfo._backGround == newInfo._backGround &&
            oldInfo._sparse == newInfo._sparse &&
            oldInfo._ttl == newInfo._ttl &&
            oldInfo._defaultLanguage == newInfo._defaultLanguage &&
            oldInfo._languageOverride == newInfo._languageOverride &&
            sameJsonDoc(oldInfo._textWeights, newInfo._textWeights) &&
            sameJsonDoc(oldInfo._partialFilterExpression, newInfo._partialFilterExpression) &&
            sameJsonDoc(oldInfo._collation, newInfo._collation) &&
            sameJsonDoc(oldInfo._wildcardProjection, newInfo._wildcardProjection) &&
            oldInfo._hidden != newInfo._hidden) {
            namespace basic = bsoncxx::builder::basic;
            runCommandChecked(ns.databaseName(),
                              basic::make_document(
                                  basic::kvp("collMod", ns.collectionName()),
                                  basic::kvp("index", basic::make_document(
                                      basic::kvp("name", newInfo._name),
                                      basic::kvp("hidden", newInfo._hidden)))));
            return;
        }

        auto const buildKeys = [](IndexInfo const& indexInfo) {
            return parseJson(indexInfo._keys);
        };

        auto const buildOptions = [](IndexInfo const& indexInfo) {
            namespace basic = bsoncxx::builder::basic;
            basic::document optionsBuilder;
            optionsBuilder.append(basic::kvp("name", indexInfo._name));

            auto const addIfTrue = [&](const char *key, bool value) {
                if (value)
                    optionsBuilder.append(basic::kvp(std::string(key), true));
            };

            addIfTrue("unique", indexInfo._unique);
            addIfTrue("background", indexInfo._backGround);
            addIfTrue("sparse", indexInfo._sparse);
            addIfTrue("hidden", indexInfo._hidden);

            if (!indexInfo._defaultLanguage.empty())
                optionsBuilder.append(basic::kvp("default_language", indexInfo._defaultLanguage));

            if (!indexInfo._languageOverride.empty())
                optionsBuilder.append(basic::kvp("language_override", indexInfo._languageOverride));

            if (!indexInfo._textWeights.empty())
                optionsBuilder.append(basic::kvp("weights", parseJson(indexInfo._textWeights)));

            if (!indexInfo._partialFilterExpression.empty())
                optionsBuilder.append(basic::kvp("partialFilterExpression",
                                                 parseJson(indexInfo._partialFilterExpression)));

            if (!indexInfo._collation.empty())
                optionsBuilder.append(basic::kvp("collation", parseJson(indexInfo._collation)));

            if (!indexInfo._wildcardProjection.empty())
                optionsBuilder.append(basic::kvp("wildcardProjection",
                                                 parseJson(indexInfo._wildcardProjection)));

            if (indexInfo._ttl >= 0)
                optionsBuilder.append(basic::kvp("expireAfterSeconds", indexInfo._ttl));

            return optionsBuilder.extract();
        };

        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            auto specOf = [&](const IndexInfo &i) {
                basic::document spec;
                spec.append(basic::kvp("key", buildKeys(i).view()));
                for (auto const& e : buildOptions(i).view())
                    spec.append(basic::kvp(std::string(e.key().data(), e.key().size()),
                                           e.get_value()));
                return spec.extract();
            };
            auto createIndexes = [&](const IndexInfo &i) {
                basic::array idx;
                idx.append(specOf(i));
                runCommandChecked(ns.databaseName(),
                    basic::make_document(basic::kvp("createIndexes", ns.collectionName()),
                                         basic::kvp("indexes", idx.extract())));
            };
            if (editIndex)
                runCommandChecked(ns.databaseName(),
                    basic::make_document(basic::kvp("dropIndexes", ns.collectionName()),
                                         basic::kvp("index", oldInfo._name)));
            try {
                createIndexes(newInfo);
            } catch (const std::exception &) {
                if (editIndex)
                    createIndexes(oldInfo);  // recover the dropped old index
                throw;
            }
            return;
        }

        auto coll = _client->database(ns.databaseName()).collection(ns.collectionName());

        // 1.Step: Drop Index (if this is an Edit Index action).
        // MongoDB docs: To modify an existing index, you need to drop and recreate the index.
        if (editIndex)
            coll.indexes().drop_one(oldInfo._name);

        // 2.Step: Add/Edit Index
        try {
            coll.indexes().create_one(buildKeys(newInfo).view(), buildOptions(newInfo).view());
        }
        catch (std::exception const& /*ex*/) { // Logging of "ex" is done in upper scope
            if (editIndex) {
                // If we are here, index that is being edited, must have already been dropped and 
                // creation of new index failed. So, we try to at least recover the dropped (old) index
                coll.indexes().create_one(buildKeys(oldInfo).view(), buildOptions(oldInfo).view());
            }
            throw;
        }
    }

    void MongoClient::renameIndexFromCollection(const MongoCollectionInfo &collection,
                                                const std::string &oldIndexName,
                                                const std::string &newIndexName) const
    {
        // Rename = read spec via listIndexes, drop old index, re-create under new name.
        const MongoNamespace &ns = collection.ns();

        namespace basic = bsoncxx::builder::basic;
        if (_transport) {
            bsoncxx::document::value spec = basic::make_document();
            bool found = false;
            for (auto const& doc : execCursor(ns.databaseName(),
                     basic::make_document(basic::kvp("listIndexes", ns.collectionName())))) {
                if (BsonUtils::getField<std::string>(doc.view(), "name") == oldIndexName) {
                    spec = bsoncxx::document::value(doc.view());
                    found = true;
                    break;
                }
            }
            if (!found)
                return;
            bsoncxx::document::element keyElem = spec.view()["key"];
            if (!keyElem)
                return;
            runCommandChecked(ns.databaseName(), basic::make_document(
                basic::kvp("dropIndexes", ns.collectionName()),
                basic::kvp("index", oldIndexName)));
            basic::document opts;
            opts.append(basic::kvp("name", newIndexName));
            for (auto const& e : spec.view()) {
                const std::string key(e.key().data(), e.key().size());
                if (key == "key" || key == "name" || key == "v" || key == "ns")
                    continue;
                opts.append(basic::kvp(key, e.get_value()));
            }
            opts.append(basic::kvp("key", keyElem.get_value()));
            basic::array idx;
            idx.append(opts.extract());
            runCommandChecked(ns.databaseName(), basic::make_document(
                basic::kvp("createIndexes", ns.collectionName()),
                basic::kvp("indexes", idx.extract())));
            return;
        }

        auto coll = _client->database(ns.databaseName()).collection(ns.collectionName());
        bsoncxx::document::value spec = basic::make_document();
        bool found = false;
        for (auto const& doc : coll.indexes().list()) {
            if (BsonUtils::getField<std::string>(doc, "name") == oldIndexName) {
                spec = bsoncxx::document::value(doc);
                found = true;
                break;
            }
        }
        if (!found)
            return;

        // Copy "key" subdocument verbatim; copy remaining option fields (except name/v/ns).
        bsoncxx::document::element keyElem = spec.view()["key"];
        if (!keyElem)
            return;

        coll.indexes().drop_one(oldIndexName);

        basic::document opts;
        opts.append(basic::kvp("name", newIndexName));
        for (auto const& e : spec.view()) {
            const std::string key(e.key().data(), e.key().size());
            if (key == "key" || key == "name" || key == "v" || key == "ns")
                continue;
            opts.append(basic::kvp(key, e.get_value()));
        }
        coll.indexes().create_one(keyElem.get_document().view(), opts.extract());
    }

    void MongoClient::dropIndexFromCollection(const MongoCollectionInfo &collection,
                                              const std::string &indexName) const
    {
        const MongoNamespace &ns = collection.ns();
        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            runCommandChecked(ns.databaseName(), basic::make_document(
                basic::kvp("dropIndexes", ns.collectionName()),
                basic::kvp("index", indexName)));
            return;
        }
        _client->database(ns.databaseName()).collection(ns.collectionName())
            .indexes().drop_one(indexName);
    }

    void MongoClient::createFunction(const std::string &dbName, const MongoFunction &fun,
                                     const std::string &existingFunctionName /* = QString() */)
    {
        auto obj = fun.toBson();
        namespace basic = bsoncxx::builder::basic;
        if (_transport) {
            auto insertDoc = [&] {
                basic::array docs;
                docs.append(obj.view());
                runCommandChecked(dbName, basic::make_document(
                    basic::kvp("insert", "system.js"),
                    basic::kvp("documents", docs.extract())));
            };
            if (existingFunctionName.empty() || existingFunctionName != fun.name()) {
                insertDoc();
                if (!existingFunctionName.empty()) {
                    basic::array dels;
                    dels.append(basic::make_document(
                        basic::kvp("q", basic::make_document(
                            basic::kvp("_id", existingFunctionName))),
                        basic::kvp("limit", 1)));
                    runCommandChecked(dbName, basic::make_document(
                        basic::kvp("delete", "system.js"),
                        basic::kvp("deletes", dels.extract())));
                }
            } else {  // same _id: replace upsert
                basic::array ups;
                ups.append(basic::make_document(
                    basic::kvp("q", basic::make_document(basic::kvp("_id", fun.name()))),
                    basic::kvp("u", obj.view()),
                    basic::kvp("upsert", true)));
                runCommandChecked(dbName, basic::make_document(
                    basic::kvp("update", "system.js"),
                    basic::kvp("updates", ups.extract())));
            }
            return;
        }

        auto coll = _client->database(dbName).collection("system.js");

        if (existingFunctionName.empty()) { // create new function
            coll.insert_one(obj.view());
        } else { // this is update
            std::string name = fun.name();

            if (existingFunctionName == name) { // update existing function code
                mongocxx::options::replace updateOpts;
                updateOpts.upsert(true);
                coll.replace_one(
                    basic::make_document(basic::kvp("_id", name)).view(),
                    obj.view(), updateOpts);
            } else {    // update function name (remove & insert)
                coll.insert_one(obj.view());
                coll.delete_one(
                    basic::make_document(basic::kvp("_id", existingFunctionName)).view());
            }
        }
    }

    void MongoClient::dropFunction(const std::string &dbName, const std::string &name)
    {
        namespace basic = bsoncxx::builder::basic;
        if (_transport) {
            basic::array dels;
            dels.append(basic::make_document(
                basic::kvp("q", basic::make_document(basic::kvp("_id", name))),
                basic::kvp("limit", 1)));
            runCommandChecked(dbName, basic::make_document(
                basic::kvp("delete", "system.js"),
                basic::kvp("deletes", dels.extract())));
            return;
        }
        _client->database(dbName).collection("system.js")
            .delete_one(basic::make_document(basic::kvp("_id", name)).view());
    }

    void MongoClient::createDatabase(const std::string &dbName)
    {
        /*
        *  Here we are going to insert temp document to "<dbName>.temp" collection.
        *  This will create <dbName> database for us.
        *  Finally we are dropping just created temporary collection.
        */
        // If <dbName>.temp already exists, stop.
        for (auto const& c : listCollectionDocs(dbName, true)) {
            if (BsonUtils::getField<std::string>(c.view(), "name") == "temp")
                throw std::runtime_error(dbName + ".temp already exists.");
        }

        // Building { _id : "temp" } document
        namespace basic = bsoncxx::builder::basic;
        auto obj = basic::make_document(basic::kvp("_id", "temp"));

        if (_transport) {
            basic::array docs;
            docs.append(obj.view());
            runCommandChecked(dbName, basic::make_document(
                basic::kvp("insert", "temp"),
                basic::kvp("documents", docs.extract())));
            runCommandChecked(dbName, basic::make_document(basic::kvp("drop", "temp")));
            return;
        }

        auto coll = _client->database(dbName).collection("temp");

        // Insert this document
        coll.insert_one(obj.view());

        // Drop temp collection
        coll.drop();
    }

    void MongoClient::dropDatabase(const std::string &dbName)
    {
        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            runCommandChecked(dbName, basic::make_document(basic::kvp("dropDatabase", 1)));
            return;
        }
        try {
            _client->database(dbName).drop();
        } catch (const mongocxx::exception &e) {
            throw std::runtime_error(e.what());
        }
    }

    void MongoClient::createCollection(const std::string& ns, long long size, bool capped, int max,
                                       bsoncxx::document::view_or_value extraOptions,
                                       bsoncxx::document::value *info)
    {
        if (capped && !size)
            throw std::invalid_argument("Capped collections require a size.");

        MongoNamespace mongons(ns);
        const std::string db = mongons.databaseName();

        namespace basic = bsoncxx::builder::basic;
        basic::document b;
        b.append(basic::kvp("create", mongons.collectionName()));
        if (size)
            b.append(basic::kvp("size", static_cast<std::int64_t>(size)));  // long long is not int64_t on Linux
        if (capped)
            b.append(basic::kvp("capped", true));
        if (max)
            b.append(basic::kvp("max", max));
        for (auto const& e : extraOptions.view())
            b.append(basic::kvp(std::string(e.key().data(), e.key().size()), e.get_value()));

        bool exists = false;
        for (auto const& c : listCollectionDocs(db, true)) {
            if (BsonUtils::getField<std::string>(c.view(), "name") == mongons.collectionName()) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            auto result = runCommandChecked(db, b.extract());
            if (info)
                *info = bsoncxx::document::value(result.view());
        }
        else {
            throw std::runtime_error("Collection with same name already exists.");
        }
    }

    void MongoClient::renameCollection(const MongoNamespace &ns, const std::string &newCollectionName)
    {
        MongoNamespace from(ns);
        MongoNamespace to(ns.databaseName(), newCollectionName);

        // Building { renameCollection: <source-namespace>, to: <target-namespace> }
        namespace basic = bsoncxx::builder::basic;
        runCommandChecked("admin",
            basic::make_document(
                basic::kvp("renameCollection", from.toString()),
                basic::kvp("to", to.toString())));
    }

    void MongoClient::duplicateCollection(const MongoNamespace &ns, const std::string &newCollectionName)
    {
        MongoNamespace const newCollection(ns.databaseName(), newCollectionName);

        bool exists = false;
        for (auto const& c : listCollectionDocs(ns.databaseName(), true)) {
            if (BsonUtils::getField<std::string>(c.view(), "name") == newCollectionName) {
                exists = true;
                break;
            }
        }

        if (exists)
            throw std::runtime_error("Collection with same name already exists.");

        // todo: Issue #1258 : Duplicate Collection should support advanced collection options.
        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            const auto docs = execCursor(ns.databaseName(),
                basic::make_document(basic::kvp("find", ns.collectionName())));
            if (!docs.empty()) {
                basic::array arr;
                for (auto const& d : docs)
                    arr.append(d.view());
                runCommandChecked(ns.databaseName(), basic::make_document(
                    basic::kvp("insert", newCollectionName),
                    basic::kvp("documents", arr.extract())));
            }
            return;
        }

        auto source = _client->database(ns.databaseName()).collection(ns.collectionName());
        auto dest = _client->database(ns.databaseName()).collection(newCollectionName);

        for (auto const& doc : source.find({}))
            dest.insert_one(doc);
    }

    void MongoClient::dropCollection(const MongoNamespace &ns)
    {
        bool exists = false;
        for (auto const& c : listCollectionDocs(ns.databaseName(), true)) {
            if (BsonUtils::getField<std::string>(c.view(), "name") == ns.collectionName()) {
                exists = true;
                break;
            }
        }

        if (!exists)
            throw std::runtime_error("Collection does not exist.");

        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            runCommandChecked(ns.databaseName(),
                basic::make_document(basic::kvp("drop", ns.collectionName())));
            return;
        }
        try {
            _client->database(ns.databaseName()).collection(ns.collectionName()).drop();
        } catch (const mongocxx::exception &e) {
            throw std::runtime_error(e.what());
        }
    }

    void MongoClient::insertDocument(bsoncxx::document::view_or_value obj, const MongoNamespace &ns)
    {
        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            basic::array docs;
            docs.append(obj.view());
            runCommandChecked(ns.databaseName(), basic::make_document(
                basic::kvp("insert", ns.collectionName()),
                basic::kvp("documents", docs.extract())));
            return;
        }
        _client->database(ns.databaseName()).collection(ns.collectionName())
            .insert_one(obj.view());
    }

    void MongoClient::saveDocument(bsoncxx::document::view_or_value obj, const MongoNamespace &ns)
    {
        const auto id = obj.view()["_id"];
        if (!id)
            throw std::runtime_error("Document has no _id field.");

        namespace basic = bsoncxx::builder::basic;
        if (_transport) {
            basic::array ups;
            ups.append(basic::make_document(
                basic::kvp("q", basic::make_document(basic::kvp("_id", id.get_value()))),
                basic::kvp("u", obj.view()),
                basic::kvp("upsert", true)));
            runCommandChecked(ns.databaseName(), basic::make_document(
                basic::kvp("update", ns.collectionName()),
                basic::kvp("updates", ups.extract())));
            return;
        }
        mongocxx::options::replace updateOpts;
        updateOpts.upsert(true);
        _client->database(ns.databaseName()).collection(ns.collectionName())
            .replace_one(basic::make_document(basic::kvp("_id", id.get_value())).view(),
                         obj.view(), updateOpts);
    }

    void MongoClient::removeDocuments(const MongoNamespace &ns,
                                      bsoncxx::document::view_or_value query,
                                      bool justOne /*= true*/)
    {
        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            basic::array dels;
            dels.append(basic::make_document(
                basic::kvp("q", query.view()),
                basic::kvp("limit", justOne ? 1 : 0)));
            runCommandChecked(ns.databaseName(), basic::make_document(
                basic::kvp("delete", ns.collectionName()),
                basic::kvp("deletes", dels.extract())));
            return;
        }
        auto coll = _client->database(ns.databaseName()).collection(ns.collectionName());
        if (justOne)
            coll.delete_one(query.view());
        else
            coll.delete_many(query.view());
    }

    std::vector<MongoDocumentPtr> MongoClient::query(const MongoQueryInfo &info)
    {
        MongoNamespace ns(info._info._ns);

        std::vector<MongoDocumentPtr> docs;

        if (info._limit == -1) // it means that we do not need to load any documents
            return docs;

        if (_transport) {
            namespace basic = bsoncxx::builder::basic;
            basic::document cmd;
            cmd.append(basic::kvp("find", ns.collectionName()));
            cmd.append(basic::kvp("filter", info._query.view()));
            if (info._limit > 0)
                cmd.append(basic::kvp("limit", info._limit));
            if (info._skip > 0)
                cmd.append(basic::kvp("skip", info._skip));
            if (info._batchSize > 0)
                cmd.append(basic::kvp("batchSize", info._batchSize));
            if (!info._fields.view().empty())
                cmd.append(basic::kvp("projection", info._fields.view()));
            for (auto const& d : execCursor(ns.databaseName(), cmd.extract()))
                docs.push_back(MongoDocumentPtr(new MongoDocument(std::move(d))));
            return docs;
        }

        mongocxx::options::find opts;
        if (info._limit > 0)
            opts.limit(info._limit);
        if (info._skip > 0)
            opts.skip(info._skip);
        if (info._batchSize > 0)
            opts.batch_size(info._batchSize);
        if (!info._fields.view().empty())
            opts.projection(info._fields.view());

        auto cursor = _client->database(ns.databaseName())
            .collection(ns.collectionName())
            .find(info._query.view(), opts);

        for (auto const& bsonObj : cursor) {
            MongoDocumentPtr doc(new MongoDocument(bsoncxx::document::value(bsonObj)));
            docs.push_back(doc);
        }

        return docs;
    }

    MongoCollectionInfo MongoClient::runCollStatsCommand(const std::string &ns)
    {
        MongoCollectionInfo info(ns);
        return info;
    }

    std::vector<MongoCollectionInfo> MongoClient::runCollStatsCommand(const std::vector<std::string> &namespaces)
    {
        std::vector<MongoCollectionInfo> infos;
        for (auto const& ns : namespaces) {
            MongoCollectionInfo info = runCollStatsCommand(ns);
            if (info.ns().isValid())
                infos.push_back(info);
        }
        return infos;
    }

    void MongoClient::done()
    {
        // do nothing here, the connection is owned by MongoWorker
    }
}
