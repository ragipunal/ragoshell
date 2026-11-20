#include "ragoshell/core/utils/ShellCompletionInternal.h"

#include <QSet>

#include "ragoshell/core/utils/FieldCompletion.h"

namespace RagoShell::ShellCompletion
{
    using namespace detail;

        Result complete(const QString &textBeforeCaret, const Environment &env, int maxItems)
        {
            Result res;
            if (env.mode == AutocompleteNone)
                return res;

            res.context = analyze(textBeforeCaret);
            const Context &ctx = res.context;
            if (ctx.scope == Scope::None)
                return res;

            const bool allowCollections = (env.mode != AutocompleteNoCollectionNames);
            std::vector<Item> &items = res.items;

            switch (ctx.scope) {
            case Scope::MemberAccess: {
                if (env.delimiterTrigger)
                    break;
                switch (ctx.receiver) {
                case Receiver::Database: {
                    if (allowCollections) {
                        res.needsLiveCollections = true;
                        std::vector<Item> collExact;
                        std::vector<Item> collCi;
                        QSet<QString> seen;
                        for (const QString &c : env.collections) {
                            if (c.isEmpty() || seen.contains(c))
                                continue;
                            seen.insert(c);
                            const QString formatted = formatDbCollectionMember(c);
                            const bool exact = ctx.prefix.isEmpty()
                                || c.startsWith(ctx.prefix, Qt::CaseSensitive)
                                || formatted.startsWith(ctx.prefix, Qt::CaseSensitive);
                            const bool ci = !exact
                                && (c.startsWith(ctx.prefix, Qt::CaseInsensitive)
                                    || formatted.startsWith(ctx.prefix, Qt::CaseInsensitive));
                            if (exact)
                                collExact.push_back({formatted, formatted, QStringLiteral("collection")});
                            else if (ci)
                                collCi.push_back({formatted, formatted, QStringLiteral("collection")});
                        }
                        items.insert(items.end(), collExact.begin(), collExact.end());
                        items.insert(items.end(), collCi.begin(), collCi.end());
                    }
                    appendFilteredEntries(dbMemberEntries(), ctx.prefix, items);
                    break;
                }
                case Receiver::SiblingDatabase:
                    appendFilteredEntries(dbMemberEntries(), ctx.prefix, items);
                    break;
                case Receiver::SystemNamespace:
                    appendFilteredEntries(systemNamespaceEntries(), ctx.prefix, items);
                    break;
                case Receiver::Mongo:
                    appendFilteredEntries(mongoMemberEntries(), ctx.prefix, items);
                    break;
                case Receiver::Collection:
                    appendFilteredEntries(collectionMemberEntries(), ctx.prefix, items);
                    break;
                case Receiver::QueryCursor:
                    appendFilteredEntries(queryCursorEntries(), ctx.prefix, items);
                    break;
                case Receiver::AggCursor:
                    appendFilteredEntries(aggCursorEntries(), ctx.prefix, items);
                    break;
                case Receiver::Explainable:
                    appendFilteredEntries(explainableEntries(), ctx.prefix, items);
                    break;
                case Receiver::ReplicaSet:
                    appendFilteredEntries(replicaSetEntries(), ctx.prefix, items);
                    break;
                case Receiver::Shard:
                    appendFilteredEntries(shardEntries(), ctx.prefix, items);
                    break;
                case Receiver::Ejson:
                    appendFilteredEntries(ejsonEntries(), ctx.prefix, items);
                    break;
                case Receiver::Console:
                    appendFilteredEntries(consoleEntries(), ctx.prefix, items);
                    break;
                case Receiver::None:
                    break;
                }
                break;
            }

            case Scope::PipelineStageKey:
                appendOperators(pipelineStageOperators(), ctx.prefix, "stage", false, items);
                break;

            case Scope::UpdateOperatorKey:
                appendOperators(updateOperators(), ctx.prefix, "update", false, items);
                break;

            case Scope::FieldConditionKey:
                appendOperators(fieldConditionOperators(), ctx.prefix, "query", false, items);
                break;

            case Scope::FilterKey:
                if (ctx.prefix.startsWith(QLatin1Char('$'))) {
                    appendOperators(topFilterOperators(), ctx.prefix, "filter", false, items);
                } else {
                    if (!ctx.collection.isEmpty()) {
                        if (env.fields.empty())
                            res.needsSchema = true;
                        else
                            appendSchemaFields(env.fields, ctx.prefix, FieldCompletion::Kind::Key, items);
                    }
                    if (env.explicitTrigger)
                        appendOperators(topFilterOperators(), ctx.prefix, "filter", false, items);
                }
                break;

            case Scope::FieldsKey:
                if (ctx.prefix.startsWith(QLatin1Char('$'))) {
                    if (QStringLiteral("$meta").startsWith(ctx.prefix, Qt::CaseInsensitive))
                        items.push_back({QStringLiteral("$meta"), QStringLiteral("$meta"), QStringLiteral("projection")});
                } else if (!ctx.collection.isEmpty()) {
                    if (env.fields.empty())
                        res.needsSchema = true;
                    else
                        appendSchemaFields(env.fields, ctx.prefix, FieldCompletion::Kind::Key, items);
                }
                break;

            case Scope::GroupOutputKey:
                if (ctx.prefix.isEmpty() || QStringLiteral("_id").startsWith(ctx.prefix, Qt::CaseInsensitive))
                    items.push_back({QStringLiteral("_id"), QStringLiteral("_id"), QStringLiteral("group key")});
                break;

            case Scope::GroupFieldValue:
                appendOperators(groupAccumulatorOperators(), ctx.prefix, "accumulator", false, items);
                break;

            case Scope::AggExpressionKey:
                appendOperators(aggExpressionOperators(), ctx.prefix, "expression", false, items);
                break;

            case Scope::SpecKey: {
                if (ctx.helperKind == QLatin1String("lookup")) {
                    static const std::vector<EntryDef> k = {
                        {"from", "from", "lookup"},
                        {"localField", "localField", "lookup"},
                        {"foreignField", "foreignField", "lookup"},
                        {"as", "as", "lookup"},
                        {"let", "let", "lookup"},
                        {"pipeline", "pipeline", "lookup"},
                    };
                    appendFilteredEntries(k, ctx.prefix, items);
                } else if (ctx.helperKind == QLatin1String("findAndModify")) {
                    static const std::vector<EntryDef> k = {
                        {"query", "query", "findAndModify"},
                        {"update", "update", "findAndModify"},
                        {"sort", "sort", "findAndModify"},
                        {"fields", "fields", "findAndModify"},
                        {"new", "new", "findAndModify"},
                        {"upsert", "upsert", "findAndModify"},
                        {"remove", "remove", "findAndModify"},
                    };
                    appendFilteredEntries(k, ctx.prefix, items);
                } else if (ctx.helperKind == QLatin1String("bulkWriteOp")) {
                    static const std::vector<EntryDef> k = {
                        {"insertOne", "insertOne", "bulkWrite"},
                        {"updateOne", "updateOne", "bulkWrite"},
                        {"updateMany", "updateMany", "bulkWrite"},
                        {"replaceOne", "replaceOne", "bulkWrite"},
                        {"deleteOne", "deleteOne", "bulkWrite"},
                        {"deleteMany", "deleteMany", "bulkWrite"},
                    };
                    appendFilteredEntries(k, ctx.prefix, items);
                } else if (ctx.helperKind == QLatin1String("bulkWriteSpec")) {
                    static const std::vector<EntryDef> k = {
                        {"filter", "filter", "bulkWrite"},
                        {"update", "update", "bulkWrite"},
                        {"replacement", "replacement", "bulkWrite"},
                        {"document", "document", "bulkWrite"},
                        {"upsert", "upsert", "bulkWrite"},
                        {"arrayFilters", "arrayFilters", "bulkWrite"},
                        {"collation", "collation", "bulkWrite"},
                        {"hint", "hint", "bulkWrite"},
                    };
                    appendFilteredEntries(k, ctx.prefix, items);
                }
                break;
            }

            case Scope::UnboundOperatorKey: {
                QSet<QString> seen;
                auto addUnique = [&](const std::vector<const char *> &ops, const char *tag) {
                    for (const char *raw : ops) {
                        const QString op = QString::fromLatin1(raw);
                        if (!seen.contains(op) && op.startsWith(ctx.prefix, Qt::CaseInsensitive)) {
                            seen.insert(op);
                            items.push_back({op, op, QString::fromLatin1(tag)});
                        }
                    }
                };
                addUnique(pipelineStageOperators(), "stage");
                addUnique(updateOperators(), "update");
                addUnique(fieldConditionOperators(), "query");
                addUnique(topFilterOperators(), "filter");
                addUnique(groupAccumulatorOperators(), "accumulator");
                addUnique(aggExpressionOperators(), "expression");
                break;
            }

            case Scope::QuotedFieldKey:
                if (!ctx.collection.isEmpty()) {
                    if (env.fields.empty())
                        res.needsSchema = true;
                    else
                        appendSchemaFields(env.fields, ctx.prefix, FieldCompletion::Kind::QuotedKey, items);
                }
                break;

            case Scope::FieldReference:
                if (!ctx.collection.isEmpty()) {
                    if (env.fields.empty())
                        res.needsSchema = true;
                    else
                        appendSchemaFields(env.fields, ctx.prefix, FieldCompletion::Kind::Reference, items);
                }
                break;

            case Scope::DistinctFieldArg:
                if (!ctx.collection.isEmpty()) {
                    if (env.fields.empty())
                        res.needsSchema = true;
                    else
                        appendSchemaFields(env.fields, ctx.prefix, FieldCompletion::Kind::Argument, items);
                }
                break;

            case Scope::CollectionNameArg:
                if (allowCollections) {
                    res.needsLiveCollections = true;
                    QSet<QString> seen;
                    for (const QString &c : env.collections) {
                        if (c.isEmpty() || seen.contains(c))
                            continue;
                        seen.insert(c);
                        if (ctx.prefix.isEmpty() || c.startsWith(ctx.prefix, Qt::CaseInsensitive))
                            items.push_back({c, escapeForQuote(c, ctx.quoteChar), QStringLiteral("collection")});
                    }
                }
                break;

            case Scope::DatabaseNameArg:
            case Scope::UseCommand: {
                QStringList dbs = env.databases;
                for (const char *builtin : {"admin", "local", "config"}) {
                    const QString b = QString::fromLatin1(builtin);
                    if (!dbs.contains(b))
                        dbs << b;
                }
                QSet<QString> seen;
                for (const QString &db : dbs) {
                    if (db.isEmpty() || seen.contains(db))
                        continue;
                    seen.insert(db);
                    if (ctx.prefix.isEmpty() || db.startsWith(ctx.prefix, Qt::CaseInsensitive))
                        items.push_back({db, db, QStringLiteral("database")});
                }
                break;
            }

            case Scope::ExplainVerbosityArg: {
                static const std::vector<EntryDef> verbosities = {
                    {"queryPlanner", "queryPlanner", "verbosity"},
                    {"executionStats", "executionStats", "verbosity"},
                    {"allPlansExecution", "allPlansExecution", "verbosity"},
                };
                appendFilteredEntries(verbosities, ctx.prefix, items);
                break;
            }

            case Scope::ShowCommand: {
                static const std::vector<EntryDef> showTargets = {
                    {"dbs", "dbs", "command"},
                    {"databases", "databases", "command"},
                    {"collections", "collections", "command"},
                    {"tables", "tables", "command"},
                    {"users", "users", "command"},
                    {"roles", "roles", "command"},
                    {"profile", "profile", "command"},
                    {"logs", "logs", "command"},
                    {"log global", "log global", "command"},
                };
                appendFilteredEntries(showTargets, ctx.prefix, items);
                break;
            }

            case Scope::StatementStart:
                if (!ctx.prefix.isEmpty() || env.explicitTrigger)
                    appendFilteredEntries(statementEntries(), ctx.prefix, items);
                break;

            case Scope::Expression:
                if ((!ctx.prefix.isEmpty() && !env.delimiterTrigger) || env.explicitTrigger)
                    appendFilteredEntries(expressionEntries(), ctx.prefix, items);
                break;

            case Scope::None:
                break;
            }

            if (static_cast<int>(items.size()) > maxItems)
                items.resize(static_cast<size_t>(maxItems));
            return res;
        }
}
