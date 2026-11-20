#include "ragoshell/core/utils/ShellCompletionInternal.h"

#include <algorithm>

#include <QRegularExpression>

#include "ragoshell/core/utils/FieldCompletion.h"

namespace RagoShell::ShellCompletion
{
    namespace detail
    {
            bool isIdentStart(QChar c)
            {
                return c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$');
            }

            bool isIdentChar(QChar c)
            {
                return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
            }

            void skipTriviaBack(const QString &t, const std::vector<bool> &skipMask, int &p)
            {
                while (p >= 0 && (t[p].isSpace() || skipMask[static_cast<size_t>(p)]))
                    --p;
            }

            void skipTriviaForward(const QString &t, const std::vector<bool> &skipMask, int &p)
            {
                const int n = t.size();
                while (p < n && (t[p].isSpace() || skipMask[static_cast<size_t>(p)]))
                    ++p;
            }

            QString identifierBack(const QString &t, int &p)
            {
                const int end = p;
                while (p >= 0 && isIdentChar(t[p]))
                    --p;
                return t.mid(p + 1, end - p);
            }

            QString identifierForward(const QString &t, int &p)
            {
                const int n = t.size();
                if (p >= n || !isIdentStart(t[p]))
                    return {};
                const int start = p;
                while (p < n && isIdentChar(t[p]))
                    ++p;
                return t.mid(start, p - start);
            }

            QString unescapeJsString(const QString &raw)
            {
                QString out;
                out.reserve(raw.size());
                for (int i = 0; i < raw.size(); ++i) {
                    if (raw[i] == QLatin1Char('\\') && i + 1 < raw.size()) {
                        ++i;
                        const QChar esc = raw[i];
                        if (esc == QLatin1Char('n')) out += QLatin1Char('\n');
                        else if (esc == QLatin1Char('t')) out += QLatin1Char('\t');
                        else if (esc == QLatin1Char('r')) out += QLatin1Char('\r');
                        else out += esc;
                    } else {
                        out += raw[i];
                    }
                }
                return out;
            }

            QString unquoteSingleArg(const QString &raw)
            {
                const QString s = raw.trimmed();
                if (s.size() < 2)
                    return {};
                const QChar q = s[0];
                if ((q != QLatin1Char('\'') && q != QLatin1Char('"') && q != QLatin1Char('`')) || s.back() != q)
                    return {};
                // Ensure there is no unescaped closing quote + comma inside (e.g. "'a', 'b'").
                for (int i = 1; i < s.size() - 1; ++i) {
                    if (s[i] == QLatin1Char('\\'))
                        ++i;
                    else if (s[i] == q)
                        return {};
                }
                return unescapeJsString(s.mid(1, s.size() - 2));
            }

            QString escapeForQuote(const QString &s, QChar quoteChar)
            {
                const QChar q = (quoteChar == QLatin1Char('"')) ? QLatin1Char('"') : QLatin1Char('\'');
                QString out;
                out.reserve(s.size() + 4);
                for (const QChar c : s) {
                    if (c == QLatin1Char('\\') || c == q)
                        out += QLatin1Char('\\');
                    out += c;
                }
                return out;
            }

            const QSet<QString> &dbMethodNameSet()
            {
                static const QSet<QString> s = {
                    QStringLiteral("getName"),
                    QStringLiteral("toString"),
                    QStringLiteral("getSiblingDB"),
                    QStringLiteral("getCollection"),
                    QStringLiteral("getMongo"),
                    QStringLiteral("runCommand"),
                    QStringLiteral("adminCommand"),
                    QStringLiteral("getCollectionNames"),
                    QStringLiteral("getCollectionInfos"),
                    QStringLiteral("createCollection"),
                    QStringLiteral("createView"),
                    QStringLiteral("connPoolStats"),
                    QStringLiteral("getFeatureCompatibilityVersion"),
                    QStringLiteral("dropDatabase"),
                    QStringLiteral("repairDatabase"),
                    QStringLiteral("stats"),
                    QStringLiteral("printCollectionStats"),
                    QStringLiteral("serverStatus"),
                    QStringLiteral("hostInfo"),
                    QStringLiteral("version"),
                    QStringLiteral("serverBuildInfo"),
                    QStringLiteral("listCommands"),
                    QStringLiteral("currentOp"),
                    QStringLiteral("killOp"),
                    QStringLiteral("getUsers"),
                    QStringLiteral("getUser"),
                    QStringLiteral("createUser"),
                    QStringLiteral("updateUser"),
                    QStringLiteral("changeUserPassword"),
                    QStringLiteral("dropUser"),
                    QStringLiteral("dropAllUsers"),
                    QStringLiteral("grantRolesToUser"),
                    QStringLiteral("revokeRolesFromUser"),
                    QStringLiteral("getRoles"),
                    QStringLiteral("getProfilingStatus"),
                    QStringLiteral("setProfilingLevel"),
                    QStringLiteral("isMaster"),
                    QStringLiteral("hello"),
                    QStringLiteral("system"),
                };
                return s;
            }

            const QSet<QString> &collectionMethodNameSet()
            {
                static const QSet<QString> s = {
                    QStringLiteral("find"), QStringLiteral("findOne"), QStringLiteral("aggregate"),
                    QStringLiteral("count"), QStringLiteral("countDocuments"), QStringLiteral("estimatedDocumentCount"),
                    QStringLiteral("distinct"), QStringLiteral("insert"), QStringLiteral("insertOne"),
                    QStringLiteral("insertMany"), QStringLiteral("save"), QStringLiteral("replaceOne"),
                    QStringLiteral("update"), QStringLiteral("updateOne"), QStringLiteral("updateMany"),
                    QStringLiteral("findAndModify"), QStringLiteral("findOneAndUpdate"),
                    QStringLiteral("findOneAndReplace"), QStringLiteral("findOneAndDelete"),
                    QStringLiteral("remove"), QStringLiteral("deleteOne"), QStringLiteral("deleteMany"),
                    QStringLiteral("drop"), QStringLiteral("renameCollection"), QStringLiteral("stats"),
                    QStringLiteral("storageSize"), QStringLiteral("totalIndexSize"), QStringLiteral("dataSize"),
                    QStringLiteral("totalSize"), QStringLiteral("isCapped"), QStringLiteral("validate"),
                    QStringLiteral("reIndex"), QStringLiteral("getIndexes"), QStringLiteral("getIndexSpecs"),
                    QStringLiteral("getIndexKeys"), QStringLiteral("createIndex"), QStringLiteral("ensureIndex"),
                    QStringLiteral("createIndexes"), QStringLiteral("dropIndex"), QStringLiteral("dropIndexes"),
                    QStringLiteral("hideIndex"), QStringLiteral("unhideIndex"), QStringLiteral("bulkWrite"),
                    QStringLiteral("compact"), QStringLiteral("convertToCapped"), QStringLiteral("analyzeShardKey"),
                    QStringLiteral("explain"), QStringLiteral("latencyStats"), QStringLiteral("getShardDistribution"),
                    QStringLiteral("getShardVersion"), QStringLiteral("getName"), QStringLiteral("getFullName"),
                    QStringLiteral("getDB"), QStringLiteral("toString"),
                };
                return s;
            }

            const QSet<QString> &chainableQueryCursorMethods()
            {
                static const QSet<QString> s = {
                    QStringLiteral("sort"),
                    QStringLiteral("limit"),
                    QStringLiteral("skip"),
                    QStringLiteral("projection"),
                    QStringLiteral("hint"),
                    QStringLiteral("collation"),
                    QStringLiteral("batchSize"),
                    QStringLiteral("maxTimeMS"),
                    QStringLiteral("pretty"),
                    QStringLiteral("readPref"),
                };
                return s;
            }

            const QSet<QString> &jsReservedKeywords()
            {
                static const QSet<QString> s = {
                    QStringLiteral("await"), QStringLiteral("break"), QStringLiteral("case"),
                    QStringLiteral("catch"), QStringLiteral("class"), QStringLiteral("const"),
                    QStringLiteral("continue"), QStringLiteral("debugger"), QStringLiteral("default"),
                    QStringLiteral("delete"), QStringLiteral("do"), QStringLiteral("else"),
                    QStringLiteral("export"), QStringLiteral("extends"), QStringLiteral("false"),
                    QStringLiteral("finally"), QStringLiteral("for"), QStringLiteral("function"),
                    QStringLiteral("if"), QStringLiteral("import"), QStringLiteral("in"),
                    QStringLiteral("instanceof"), QStringLiteral("let"), QStringLiteral("new"),
                    QStringLiteral("null"), QStringLiteral("return"), QStringLiteral("super"),
                    QStringLiteral("switch"), QStringLiteral("this"), QStringLiteral("throw"),
                    QStringLiteral("true"), QStringLiteral("try"), QStringLiteral("typeof"),
                    QStringLiteral("var"), QStringLiteral("void"), QStringLiteral("while"),
                    QStringLiteral("with"), QStringLiteral("yield"),
                };
                return s;
            }

            // ---- Method & Operator Catalogs ------------------------------------

            const std::vector<EntryDef> &dbMemberEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"getCollection(name)", "getCollection(", "db"},
                    {"getCollectionNames()", "getCollectionNames()", "db"},
                    {"getCollectionInfos()", "getCollectionInfos()", "db"},
                    {"createCollection(name)", "createCollection(", "db"},
                    {"createView(name, source, pipeline)", "createView(", "db"},
                    {"getSiblingDB(name)", "getSiblingDB(", "db"},
                    {"getName()", "getName()", "db"},
                    {"getMongo()", "getMongo()", "db"},
                    {"system", "system", "namespace"},
                    {"stats()", "stats()", "db"},
                    {"runCommand({})", "runCommand({})", "db"},
                    {"adminCommand({})", "adminCommand({})", "db"},
                    {"serverStatus()", "serverStatus()", "db"},
                    {"serverBuildInfo()", "serverBuildInfo()", "db"},
                    {"hostInfo()", "hostInfo()", "db"},
                    {"version()", "version()", "db"},
                    {"hello()", "hello()", "db"},
                    {"isMaster()", "isMaster()", "db"},
                    {"listCommands()", "listCommands()", "db"},
                    {"currentOp()", "currentOp()", "db"},
                    {"killOp(opId)", "killOp(", "db"},
                    {"connPoolStats()", "connPoolStats()", "db"},
                    {"getFeatureCompatibilityVersion()", "getFeatureCompatibilityVersion()", "db"},
                    {"printCollectionStats()", "printCollectionStats()", "db"},
                    {"getProfilingStatus()", "getProfilingStatus()", "db"},
                    {"setProfilingLevel(level)", "setProfilingLevel(", "db"},
                    {"getUsers()", "getUsers()", "db"},
                    {"getUser(username)", "getUser(", "db"},
                    {"createUser({})", "createUser({})", "db"},
                    {"updateUser(username, doc)", "updateUser(", "db"},
                    {"changeUserPassword(username, pwd)", "changeUserPassword(", "db"},
                    {"dropUser(username)", "dropUser(", "db"},
                    {"dropAllUsers()", "dropAllUsers()", "db"},
                    {"grantRolesToUser(username, roles)", "grantRolesToUser(", "db"},
                    {"revokeRolesFromUser(username, roles)", "revokeRolesFromUser(", "db"},
                    {"getRoles()", "getRoles()", "db"},
                    {"dropDatabase()", "dropDatabase()", "db"},
                    {"repairDatabase()", "repairDatabase()", "db"},
                };
                return list;
            }

            const std::vector<EntryDef> &systemNamespaceEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"users", "users", "system"},
                    {"roles", "roles", "system"},
                    {"profile", "profile", "system"},
                    {"views", "views", "system"},
                    {"js", "js", "system"},
                };
                return list;
            }

            const std::vector<EntryDef> &mongoMemberEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"getDBNames()", "getDBNames()", "mongo"},
                    {"getDBs()", "getDBs()", "mongo"},
                    {"host", "host", "mongo"},
                    {"setReadPref(mode)", "setReadPref(", "mongo"},
                    {"setSlaveOk()", "setSlaveOk()", "mongo"},
                };
                return list;
            }

            const std::vector<EntryDef> &collectionMemberEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"find({})", "find({})", "collection"},
                    {"findOne({})", "findOne({})", "collection"},
                    {"aggregate([])", "aggregate([])", "collection"},
                    {"countDocuments({})", "countDocuments({})", "collection"},
                    {"estimatedDocumentCount()", "estimatedDocumentCount()", "collection"},
                    {"count({})", "count({})", "collection"},
                    {"distinct(field, filter)", "distinct(", "collection"},
                    {"insertOne({})", "insertOne({})", "collection"},
                    {"insertMany([])", "insertMany([])", "collection"},
                    {"updateOne(filter, update)", "updateOne(", "collection"},
                    {"updateMany(filter, update)", "updateMany(", "collection"},
                    {"replaceOne(filter, doc)", "replaceOne(", "collection"},
                    {"deleteOne({})", "deleteOne({})", "collection"},
                    {"deleteMany({})", "deleteMany({})", "collection"},
                    {"findOneAndUpdate(filter, update)", "findOneAndUpdate(", "collection"},
                    {"findOneAndReplace(filter, doc)", "findOneAndReplace(", "collection"},
                    {"findOneAndDelete({})", "findOneAndDelete({})", "collection"},
                    {"findAndModify({})", "findAndModify({})", "collection"},
                    {"bulkWrite([])", "bulkWrite([])", "collection"},
                    {"insert(docs)", "insert(", "collection"},
                    {"update(filter, update)", "update(", "collection"},
                    {"save(doc)", "save(", "collection"},
                    {"remove(filter)", "remove(", "collection"},
                    {"getIndexes()", "getIndexes()", "collection"},
                    {"getIndexSpecs()", "getIndexSpecs()", "collection"},
                    {"getIndexKeys()", "getIndexKeys()", "collection"},
                    {"createIndex({})", "createIndex({})", "collection"},
                    {"createIndexes([])", "createIndexes([])", "collection"},
                    {"ensureIndex({})", "ensureIndex({})", "collection"},
                    {"dropIndex(index)", "dropIndex(", "collection"},
                    {"dropIndexes()", "dropIndexes()", "collection"},
                    {"hideIndex(name)", "hideIndex(", "collection"},
                    {"unhideIndex(name)", "unhideIndex(", "collection"},
                    {"reIndex()", "reIndex()", "collection"},
                    {"explain()", "explain()", "collection"},
                    {"stats()", "stats()", "collection"},
                    {"latencyStats()", "latencyStats()", "collection"},
                    {"storageSize()", "storageSize()", "collection"},
                    {"totalIndexSize()", "totalIndexSize()", "collection"},
                    {"dataSize()", "dataSize()", "collection"},
                    {"totalSize()", "totalSize()", "collection"},
                    {"isCapped()", "isCapped()", "collection"},
                    {"validate()", "validate()", "collection"},
                    {"compact()", "compact()", "collection"},
                    {"renameCollection(target)", "renameCollection(", "collection"},
                    {"convertToCapped(size)", "convertToCapped(", "collection"},
                    {"analyzeShardKey({})", "analyzeShardKey({})", "collection"},
                    {"getShardDistribution()", "getShardDistribution()", "collection"},
                    {"getShardVersion()", "getShardVersion()", "collection"},
                    {"getName()", "getName()", "collection"},
                    {"getFullName()", "getFullName()", "collection"},
                    {"getDB()", "getDB()", "collection"},
                    {"drop()", "drop()", "collection"},
                };
                return list;
            }

            const std::vector<EntryDef> &queryCursorEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"sort({})", "sort({})", "cursor"},
                    {"limit(n)", "limit(", "cursor"},
                    {"skip(n)", "skip(", "cursor"},
                    {"projection({})", "projection({})", "cursor"},
                    {"hint(index)", "hint(", "cursor"},
                    {"collation({})", "collation({})", "cursor"},
                    {"batchSize(n)", "batchSize(", "cursor"},
                    {"maxTimeMS(ms)", "maxTimeMS(", "cursor"},
                    {"pretty()", "pretty()", "cursor"},
                    {"readPref(mode)", "readPref(", "cursor"},
                    {"toArray()", "toArray()", "cursor"},
                    {"count()", "count()", "cursor"},
                    {"size()", "size()", "cursor"},
                    {"itcount()", "itcount()", "cursor"},
                    {"forEach(fn)", "forEach(", "cursor"},
                    {"map(fn)", "map(", "cursor"},
                    {"hasNext()", "hasNext()", "cursor"},
                    {"next()", "next()", "cursor"},
                    {"explain()", "explain()", "cursor"},
                    {"close()", "close()", "cursor"},
                };
                return list;
            }

            const std::vector<EntryDef> &aggCursorEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"toArray()", "toArray()", "cursor"},
                    {"pretty()", "pretty()", "cursor"},
                    {"forEach(fn)", "forEach(", "cursor"},
                    {"map(fn)", "map(", "cursor"},
                    {"itcount()", "itcount()", "cursor"},
                    {"hasNext()", "hasNext()", "cursor"},
                    {"next()", "next()", "cursor"},
                    {"explain()", "explain()", "cursor"},
                    {"close()", "close()", "cursor"},
                };
                return list;
            }

            const std::vector<EntryDef> &explainableEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"find({})", "find({})", "explain"},
                    {"findOne({})", "findOne({})", "explain"},
                    {"aggregate([])", "aggregate([])", "explain"},
                    {"count({})", "count({})", "explain"},
                    {"distinct(field, filter)", "distinct(", "explain"},
                    {"remove(filter)", "remove(", "explain"},
                };
                return list;
            }

            const std::vector<EntryDef> &replicaSetEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"status()", "status()", "rs"},
                    {"conf()", "conf()", "rs"},
                    {"config()", "config()", "rs"},
                    {"hello()", "hello()", "rs"},
                    {"isMaster()", "isMaster()", "rs"},
                    {"secondaryOk()", "secondaryOk()", "rs"},
                    {"slaveOk()", "slaveOk()", "rs"},
                };
                return list;
            }

            const std::vector<EntryDef> &shardEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"status()", "status()", "sh"},
                    {"getBalancerState()", "getBalancerState()", "sh"},
                };
                return list;
            }

            const std::vector<EntryDef> &ejsonEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"stringify(value)", "stringify(", "EJSON"},
                    {"parse(text)", "parse(", "EJSON"},
                    {"serialize(value)", "serialize(", "EJSON"},
                    {"deserialize(doc)", "deserialize(", "EJSON"},
                };
                return list;
            }

            const std::vector<EntryDef> &consoleEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"log(value)", "log(", "console"},
                    {"info(value)", "info(", "console"},
                    {"warn(value)", "warn(", "console"},
                    {"error(value)", "error(", "console"},
                };
                return list;
            }

            const std::vector<EntryDef> &statementEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"db", "db", "shell"},
                    {"rs", "rs", "shell"},
                    {"sh", "sh", "shell"},
                    {"show dbs", "show dbs", "command"},
                    {"show collections", "show collections", "command"},
                    {"show users", "show users", "command"},
                    {"show roles", "show roles", "command"},
                    {"show profile", "show profile", "command"},
                    {"show logs", "show logs", "command"},
                    {"use ", "use ", "command"},
                    {"ObjectId()", "ObjectId(", "BSON"},
                    {"ISODate()", "ISODate(", "BSON"},
                    {"NumberLong()", "NumberLong(", "BSON"},
                    {"NumberInt()", "NumberInt(", "BSON"},
                    {"NumberDecimal()", "NumberDecimal(", "BSON"},
                    {"Timestamp()", "Timestamp(", "BSON"},
                    {"UUID()", "UUID(", "BSON"},
                    {"BinData()", "BinData(", "BSON"},
                    {"HexData()", "HexData(", "BSON"},
                    {"MinKey()", "MinKey()", "BSON"},
                    {"MaxKey()", "MaxKey()", "BSON"},
                    {"DBRef()", "DBRef(", "BSON"},
                    {"EJSON", "EJSON", "shell"},
                    {"console", "console", "shell"},
                    {"print()", "print(", "shell"},
                    {"printjson()", "printjson(", "shell"},
                    {"printjsononeline()", "printjsononeline(", "shell"},
                    {"tojson()", "tojson(", "shell"},
                };
                return list;
            }

            const std::vector<EntryDef> &expressionEntries()
            {
                static const std::vector<EntryDef> list = {
                    {"ObjectId()", "ObjectId(", "BSON"},
                    {"ISODate()", "ISODate(", "BSON"},
                    {"NumberLong()", "NumberLong(", "BSON"},
                    {"NumberInt()", "NumberInt(", "BSON"},
                    {"NumberDecimal()", "NumberDecimal(", "BSON"},
                    {"UUID()", "UUID(", "BSON"},
                    {"Timestamp()", "Timestamp(", "BSON"},
                    {"BinData()", "BinData(", "BSON"},
                    {"HexData()", "HexData(", "BSON"},
                    {"MinKey()", "MinKey()", "BSON"},
                    {"MaxKey()", "MaxKey()", "BSON"},
                    {"DBRef()", "DBRef(", "BSON"},
                    {"new Date()", "new Date()", "JS"},
                    {"RegExp()", "RegExp(", "JS"},
                    {"true", "true", "literal"},
                    {"false", "false", "literal"},
                    {"null", "null", "literal"},
                    {"db", "db", "shell"},
                    {"EJSON", "EJSON", "shell"},
                };
                return list;
            }

            const std::vector<const char *> &pipelineStageOperators()
            {
                static const std::vector<const char *> ops = {
                    "$match", "$group", "$project", "$sort", "$limit", "$skip", "$lookup",
                    "$unwind", "$addFields", "$set", "$unset", "$count", "$facet",
                    "$replaceRoot", "$replaceWith", "$bucket", "$bucketAuto", "$sortByCount",
                    "$sample", "$unionWith", "$setWindowFields", "$densify", "$fill",
                    "$redact", "$geoNear", "$graphLookup", "$merge", "$out",
                    "$collStats", "$indexStats", "$search", "$vectorSearch",
                };
                return ops;
            }

            const std::vector<const char *> &updateOperators()
            {
                static const std::vector<const char *> ops = {
                    "$set", "$unset", "$inc", "$push", "$pull", "$addToSet", "$pop",
                    "$pullAll", "$min", "$max", "$mul", "$rename", "$currentDate",
                    "$setOnInsert", "$bit",
                };
                return ops;
            }

            const std::vector<const char *> &fieldConditionOperators()
            {
                static const std::vector<const char *> ops = {
                    "$eq", "$ne", "$gt", "$gte", "$lt", "$lte", "$in", "$nin",
                    "$exists", "$type", "$regex", "$options", "$not", "$elemMatch",
                    "$all", "$size", "$mod", "$bitsAllClear", "$bitsAllSet",
                    "$bitsAnyClear", "$bitsAnySet", "$geoWithin", "$geoIntersects",
                    "$near", "$nearSphere",
                };
                return ops;
            }

            const std::vector<const char *> &topFilterOperators()
            {
                static const std::vector<const char *> ops = {
                    "$and", "$or", "$nor", "$expr", "$jsonSchema", "$text", "$where", "$comment",
                };
                return ops;
            }

            const std::vector<const char *> &groupAccumulatorOperators()
            {
                static const std::vector<const char *> ops = {
                    "$sum", "$avg", "$min", "$max", "$first", "$last", "$push", "$addToSet",
                    "$count", "$topN", "$bottomN", "$maxN", "$minN", "$median", "$percentile",
                    "$stdDevPop", "$stdDevSamp", "$mergeObjects",
                };
                return ops;
            }

            const std::vector<const char *> &aggExpressionOperators()
            {
                static const std::vector<const char *> ops = {
                    "$cond", "$ifNull", "$switch", "$eq", "$ne", "$gt", "$gte", "$lt", "$lte",
                    "$cmp", "$and", "$or", "$not", "$in", "$add", "$subtract", "$multiply",
                    "$divide", "$mod", "$abs", "$ceil", "$floor", "$round", "$concat",
                    "$substrBytes", "$toLower", "$toUpper", "$trim", "$split", "$regexMatch",
                    "$strLenCP", "$size", "$arrayElemAt", "$slice", "$filter", "$map",
                    "$reduce", "$concatArrays", "$setUnion", "$objectToArray", "$arrayToObject",
                    "$mergeObjects", "$getField", "$type", "$convert", "$toString", "$toInt",
                    "$toLong", "$toDouble", "$toDecimal", "$toBool", "$toObjectId", "$toDate",
                    "$dateToString", "$dateFromString", "$dateTrunc", "$dateAdd",
                    "$dateSubtract", "$dateDiff", "$year", "$month", "$dayOfMonth", "$hour",
                    "$minute", "$second", "$literal", "$let",
                };
                return ops;
            }


            QString schemaDetailText(const SchemaField &f)
            {
                QStringList types;
                for (const auto &[type, n] : f.types) {
                    Q_UNUSED(n)
                    if (type == QLatin1String("Array") && !f.itemTypes.empty())
                        types << QStringLiteral("Array<%1>").arg(f.itemTypes.front().first);
                    else
                        types << type;
                    if (types.size() == 2)
                        break;
                }
                const int pct = qRound(f.presence * 100);
                const QString presence = (pct == 0 && f.presence > 0)
                    ? QStringLiteral("<1%")
                    : QStringLiteral("%1%").arg(pct);
                return types.isEmpty() ? presence : types.join(QStringLiteral(" | ")) + QStringLiteral(" · ") + presence;
            }

            void appendFilteredEntries(const std::vector<EntryDef> &defs,
                                       const QString &prefix,
                                       std::vector<Item> &out)
            {
                std::vector<Item> exact;
                std::vector<Item> ci;
                for (const EntryDef &d : defs) {
                    const QString label = QString::fromLatin1(d.label);
                    const QString insert = QString::fromLatin1(d.insert);
                    if (prefix.isEmpty() || label.startsWith(prefix, Qt::CaseSensitive)
                        || insert.startsWith(prefix, Qt::CaseSensitive)) {
                        exact.push_back({label, insert, QString::fromLatin1(d.detail)});
                    } else if (label.startsWith(prefix, Qt::CaseInsensitive)
                               || insert.startsWith(prefix, Qt::CaseInsensitive)) {
                        ci.push_back({label, insert, QString::fromLatin1(d.detail)});
                    }
                }
                out.insert(out.end(), exact.begin(), exact.end());
                out.insert(out.end(), ci.begin(), ci.end());
            }

            void appendOperators(const std::vector<const char *> &ops,
                                 const QString &prefix,
                                 const char *detail,
                                 bool quotedKey,
                                 std::vector<Item> &out)
            {
                Q_UNUSED(quotedKey)
                std::vector<Item> exact;
                std::vector<Item> ci;
                for (const char *raw : ops) {
                    const QString op = QString::fromLatin1(raw);
                    if (prefix.isEmpty() || op.startsWith(prefix, Qt::CaseSensitive)) {
                        exact.push_back({op, op, QString::fromLatin1(detail)});
                    } else if (op.startsWith(prefix, Qt::CaseInsensitive)) {
                        ci.push_back({op, op, QString::fromLatin1(detail)});
                    }
                }
                out.insert(out.end(), exact.begin(), exact.end());
                out.insert(out.end(), ci.begin(), ci.end());
            }

            void appendSchemaFields(const std::vector<SchemaField> &fields,
                                    const QString &prefix,
                                    FieldCompletion::Kind kind,
                                    std::vector<Item> &out)
            {
                std::vector<Item> commonExact;
                std::vector<Item> commonCi;
                std::vector<Item> rareExact;
                std::vector<Item> rareCi;

                for (const SchemaField &f : fields) {
                    const bool exact = prefix.isEmpty() || f.path.startsWith(prefix, Qt::CaseSensitive);
                    const bool ci = !exact && f.path.startsWith(prefix, Qt::CaseInsensitive);
                    if (!exact && !ci)
                        continue;

                    Item item;
                    item.label = kind == FieldCompletion::Kind::Reference
                        ? (QLatin1Char('$') + f.path)
                        : f.path;
                    switch (kind) {
                    case FieldCompletion::Kind::Key:
                        item.insert = FieldCompletion::isPlainKey(f.path)
                            ? f.path
                            : QStringLiteral("\"%1\"").arg(f.path);
                        break;
                    case FieldCompletion::Kind::Reference:
                        item.insert = QLatin1Char('$') + f.path;
                        break;
                    default:
                        item.insert = f.path;
                        break;
                    }
                    item.detail = schemaDetailText(f);

                    if (f.presence < kRarePresence)
                        (exact ? rareExact : rareCi).push_back(std::move(item));
                    else
                        (exact ? commonExact : commonCi).push_back(std::move(item));
                }

                out.insert(out.end(), commonExact.begin(), commonExact.end());
                out.insert(out.end(), commonCi.begin(), commonCi.end());
                out.insert(out.end(), rareExact.begin(), rareExact.end());
                out.insert(out.end(), rareCi.begin(), rareCi.end());
            }
        }

    using namespace detail;

        bool isDotAccessibleCollection(const QString &collectionName)
        {
            if (collectionName.isEmpty() || collectionName.startsWith(QLatin1Char('_')))
                return false;
            if (dbMethodNameSet().contains(collectionName) || jsReservedKeywords().contains(collectionName))
                return false;
            static const QRegularExpression identRe(QStringLiteral(R"(^[A-Za-z$][A-Za-z0-9_$]*$)"));
            return identRe.match(collectionName).hasMatch();
        }

        QString formatDbCollectionMember(const QString &collectionName)
        {
            if (isDotAccessibleCollection(collectionName))
                return collectionName;
            return QStringLiteral("getCollection('%1')").arg(escapeForQuote(collectionName, QLatin1Char('\'')));
        }

}
