#include "ragoshell/core/utils/ShellSignatures.h"

#include <QHash>
#include <QVector>

namespace RagoShell
{
    namespace ShellSignatures
    {
        namespace
        {
            using Table = QHash<QString, QStringList>;

            // Collection methods; also used for chained cursor calls unless
            // the cursor table has its own entry.
            const Table &collectionMethods()
            {
                static const Table t = {
                    {"find", {"filter", "projection", "options"}},
                    {"findOne", {"filter", "projection", "options"}},
                    {"aggregate", {"pipeline", "options"}},
                    {"countDocuments", {"filter", "options"}},
                    {"estimatedDocumentCount", {"options"}},
                    {"count", {"filter", "options"}},
                    {"distinct", {"field", "filter", "options"}},
                    {"insertOne", {"document", "options"}},
                    {"insertMany", {"documents", "options"}},
                    {"insert", {"documentOrArray", "options"}},
                    {"updateOne", {"filter", "update", "options"}},
                    {"updateMany", {"filter", "update", "options"}},
                    {"update", {"filter", "update", "options"}},
                    {"replaceOne", {"filter", "replacement", "options"}},
                    {"deleteOne", {"filter", "options"}},
                    {"deleteMany", {"filter", "options"}},
                    {"remove", {"filter", "justOne"}},
                    {"findOneAndUpdate", {"filter", "update", "options"}},
                    {"findOneAndReplace", {"filter", "replacement", "options"}},
                    {"findOneAndDelete", {"filter", "options"}},
                    {"findAndModify", {"spec"}},
                    {"bulkWrite", {"operations", "options"}},
                    {"createIndex", {"keys", "options"}},
                    {"ensureIndex", {"keys", "options"}},
                    {"createIndexes", {"keyPatterns", "options"}},
                    {"dropIndex", {"index"}},
                    {"dropIndexes", {"indexes"}},
                    {"hideIndex", {"indexName"}},
                    {"unhideIndex", {"indexName"}},
                    {"reIndex", {}},
                    {"getIndexes", {}},
                    {"getIndexSpecs", {}},
                    {"getIndexKeys", {}},
                    {"drop", {}},
                    {"renameCollection", {"newName", "dropTarget"}},
                    {"stats", {"scaleOrOptions"}},
                    {"latencyStats", {"options"}},
                    {"storageSize", {}},
                    {"totalIndexSize", {}},
                    {"dataSize", {}},
                    {"totalSize", {}},
                    {"isCapped", {}},
                    {"validate", {"full"}},
                    {"compact", {"options"}},
                    {"convertToCapped", {"sizeBytes"}},
                    {"analyzeShardKey", {"key", "options"}},
                    {"getShardDistribution", {}},
                    {"getShardVersion", {}},
                    {"getName", {}},
                    {"getFullName", {}},
                    {"getDB", {}},
                    {"explain", {"verbosity"}},
                    {"watch", {"pipeline", "options"}},
                    {"save", {"document"}},
                };
                return t;
            }

            // Cursor methods (receiver ends with a call: db.c.find(...).sort().
            const Table &cursorMethods()
            {
                static const Table t = {
                    {"sort", {"sortSpec"}},
                    {"limit", {"n"}},
                    {"skip", {"n"}},
                    {"batchSize", {"n"}},
                    {"hint", {"index"}},
                    {"maxTimeMS", {"milliseconds"}},
                    {"collation", {"collation"}},
                    {"projection", {"projection"}},
                    {"readPref", {"mode", "tagSet"}},
                    {"count", {"applySkipLimit"}},
                    {"size", {}},
                    {"itcount", {}},
                    {"explain", {"verbosity"}},
                    {"toArray", {}},
                    {"forEach", {"callback"}},
                    {"map", {"callback"}},
                    {"hasNext", {}},
                    {"next", {}},
                    {"close", {}},
                    {"pretty", {}},
                };
                return t;
            }

            const Table &dbMethods()
            {
                static const Table t = {
                    {"getCollection", {"name"}},
                    {"getSiblingDB", {"name"}},
                    {"getName", {}},
                    {"getMongo", {}},
                    {"createCollection", {"name", "options"}},
                    {"createView", {"name", "source", "pipeline", "options"}},
                    {"runCommand", {"command"}},
                    {"adminCommand", {"command"}},
                    {"getCollectionNames", {}},
                    {"getCollectionInfos", {"filter", "options"}},
                    {"dropDatabase", {}},
                    {"repairDatabase", {}},
                    {"stats", {"scale"}},
                    {"printCollectionStats", {"scale"}},
                    {"createUser", {"user", "writeConcern"}},
                    {"updateUser", {"username", "update", "writeConcern"}},
                    {"changeUserPassword", {"username", "password"}},
                    {"dropUser", {"username", "writeConcern"}},
                    {"dropAllUsers", {}},
                    {"grantRolesToUser", {"username", "roles"}},
                    {"revokeRolesFromUser", {"username", "roles"}},
                    {"getUser", {"username", "options"}},
                    {"getUsers", {"options"}},
                    {"getRoles", {"options"}},
                    {"createRole", {"role", "writeConcern"}},
                    {"currentOp", {"filter"}},
                    {"killOp", {"opid"}},
                    {"serverStatus", {"options"}},
                    {"serverBuildInfo", {}},
                    {"hostInfo", {}},
                    {"listCommands", {}},
                    {"connPoolStats", {}},
                    {"getFeatureCompatibilityVersion", {}},
                    {"getProfilingStatus", {}},
                    {"setProfilingLevel", {"level", "slowms"}},
                    {"isMaster", {}},
                    {"hello", {}},
                    {"version", {}},
                };
                return t;
            }

            const Table &rsMethods()
            {
                static const Table t = {
                    {"status", {}},
                    {"conf", {}},
                    {"config", {}},
                    {"initiate", {"config"}},
                    {"reconfig", {"config", "options"}},
                    {"add", {"hostOrMember", "arbiterOnly"}},
                    {"addArb", {"host"}},
                    {"remove", {"host"}},
                    {"stepDown", {"stepDownSecs", "catchUpSecs"}},
                    {"freeze", {"seconds"}},
                    {"printReplicationInfo", {}},
                    {"printSecondaryReplicationInfo", {}},
                    {"isMaster", {}},
                    {"hello", {}},
                    {"secondaryOk", {}},
                    {"slaveOk", {}},
                };
                return t;
            }

            const Table &shMethods()
            {
                static const Table t = {
                    {"status", {}},
                    {"getBalancerState", {}},
                };
                return t;
            }

            const Table &mongoMethods()
            {
                static const Table t = {
                    {"getDBNames", {}},
                    {"getDBs", {}},
                    {"setReadPref", {"mode"}},
                    {"setSlaveOk", {}},
                };
                return t;
            }

            const Table &ejsonMethods()
            {
                static const Table t = {
                    {"stringify", {"value", "options", "indent"}},
                    {"parse", {"text"}},
                    {"serialize", {"value", "options"}},
                    {"deserialize", {"doc"}},
                };
                return t;
            }

            // Global functions and BSON constructors (no receiver).
            const Table &globalFunctions()
            {
                static const Table t = {
                    {"ObjectId", {"hex"}},
                    {"ISODate", {"isoString"}},
                    {"Date", {"value"}},
                    {"NumberLong", {"value"}},
                    {"NumberInt", {"value"}},
                    {"NumberDecimal", {"value"}},
                    {"UUID", {"hex"}},
                    {"BinData", {"subtype", "base64"}},
                    {"HexData", {"subtype", "hex"}},
                    {"Timestamp", {"seconds", "increment"}},
                    {"DBRef", {"collection", "id", "database"}},
                    {"MinKey", {}},
                    {"MaxKey", {}},
                    {"print", {"value"}},
                    {"printjson", {"value"}},
                    {"printjsononeline", {"value"}},
                    {"tojson", {"value"}},
                    {"use", {"database"}},
                };
                return t;
            }

            struct Frame
            {
                QChar open;
                int pos = 0;
                int commas = 0;
            };

            bool isIdentChar(QChar c)
            {
                return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
            }

            // Identifier ending right before 'end' (whitespace allowed in between).
            QString identifierBefore(const QString &text, int end, int *start)
            {
                int i = end - 1;
                while (i >= 0 && text.at(i).isSpace())
                    --i;
                const int last = i;
                while (i >= 0 && isIdentChar(text.at(i)))
                    --i;
                *start = i + 1;
                return text.mid(i + 1, last - i);
            }

            // What the method is called on: "db", "rs", "sh", "mongo", "ejson", "global", "cursor" or "collection".
            QString receiverKind(const QString &text, int nameStart, const QHash<int, int> &closeToOpen)
            {
                int i = nameStart - 1;
                while (i >= 0 && text.at(i).isSpace())
                    --i;
                if (i < 0 || text.at(i) != QLatin1Char('.'))
                    return QStringLiteral("global");
                --i;
                while (i >= 0 && text.at(i).isSpace())
                    --i;
                if (i >= 0 && text.at(i) == QLatin1Char(')')) {
                    const int open = closeToOpen.value(i, -1);
                    if (open >= 0) {
                        int prevStart = 0;
                        const QString prevCall = identifierBefore(text, open, &prevStart);
                        if (prevCall == QLatin1String("getCollection") || prevCall == QLatin1String("explain"))
                            return QStringLiteral("collection");
                        if (prevCall == QLatin1String("getSiblingDB") || prevCall == QLatin1String("getDB"))
                            return QStringLiteral("db");
                        if (prevCall == QLatin1String("getMongo"))
                            return QStringLiteral("mongo");
                    }
                    return QStringLiteral("cursor");
                }
                int start = 0;
                const QString receiver = identifierBefore(text, i + 1, &start);
                // A bare identifier before it (not "x.db.") decides db / rs / sh / EJSON.
                const bool bare = start == 0 || text.at(start - 1) != QLatin1Char('.');
                if (bare && receiver == QLatin1String("db"))
                    return QStringLiteral("db");
                if (bare && receiver == QLatin1String("rs"))
                    return QStringLiteral("rs");
                if (bare && receiver == QLatin1String("sh"))
                    return QStringLiteral("sh");
                if (bare && receiver == QLatin1String("EJSON"))
                    return QStringLiteral("ejson");
                return QStringLiteral("collection");
            }

            const QStringList *paramsFor(const QString &kind, const QString &name)
            {
                auto find = [&name](const Table &t) -> const QStringList * {
                    const auto it = t.constFind(name);
                    return it == t.constEnd() ? nullptr : &it.value();
                };
                if (kind == QLatin1String("db"))
                    return find(dbMethods());
                if (kind == QLatin1String("rs"))
                    return find(rsMethods());
                if (kind == QLatin1String("sh"))
                    return find(shMethods());
                if (kind == QLatin1String("mongo"))
                    return find(mongoMethods());
                if (kind == QLatin1String("ejson"))
                    return find(ejsonMethods());
                if (kind == QLatin1String("global"))
                    return find(globalFunctions());
                if (kind == QLatin1String("cursor")) {
                    if (const QStringList *p = find(cursorMethods()))
                        return p;
                }
                return find(collectionMethods());
            }
        }

        CallTip lookup(const QString &textBeforeCaret)
        {
            const QString &text = textBeforeCaret;
            QVector<Frame> stack;
            QHash<int, int> closeToOpen;
            const int n = static_cast<int>(text.size());
            for (int i = 0; i < n; ++i) {
                const QChar c = text.at(i);
                // Strings and template literals (escapes skipped).
                if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')) {
                    for (++i; i < n && text.at(i) != c; ++i) {
                        if (text.at(i) == QLatin1Char('\\'))
                            ++i;
                    }
                    if (i >= n)
                        return {};   // caret inside a string: no tip
                    continue;
                }
                if (c == QLatin1Char('/') && i + 1 < n) {
                    if (text.at(i + 1) == QLatin1Char('/')) {
                        const int nl = text.indexOf(QLatin1Char('\n'), i);
                        if (nl < 0)
                            return {};   // caret inside a line comment
                        i = nl;
                        continue;
                    }
                    if (text.at(i + 1) == QLatin1Char('*')) {
                        const int close = text.indexOf(QLatin1String("*/"), i + 2);
                        if (close < 0)
                            return {};
                        i = close + 1;
                        continue;
                    }
                }
                if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) {
                    stack.append({c, i, 0});
                } else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
                    if (!stack.isEmpty()) {
                        closeToOpen.insert(i, stack.last().pos);
                        stack.removeLast();
                    }
                } else if (c == QLatin1Char(',') && !stack.isEmpty()) {
                    ++stack.last().commas;
                }
            }

            // Innermost open call with a known signature.
            for (int k = static_cast<int>(stack.size()) - 1; k >= 0; --k) {
                const Frame &f = stack.at(k);
                if (f.open != QLatin1Char('('))
                    continue;
                int nameStart = 0;
                const QString name = identifierBefore(text, f.pos, &nameStart);
                if (name.isEmpty())
                    continue;
                const QStringList *params = paramsFor(receiverKind(text, nameStart, closeToOpen), name);
                if (!params)
                    continue;
                CallTip tip;
                tip.found = true;
                tip.name = name;
                tip.params = *params;
                tip.activeParam = f.commas;
                tip.parenOffset = n - f.pos;
                return tip;
            }
            return {};
        }

        QString label(const CallTip &tip)
        {
            return tip.found ? tip.name + QLatin1Char('(') + tip.params.join(QStringLiteral(", "))
                                   + QLatin1Char(')')
                             : QString();
        }
    }
}
