#pragma once

#include <vector>

#include <QString>
#include <QStringList>

#include "ragoshell/core/Enums.h"
#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    /**
     * @brief Cursor-position-aware IntelliSense engine for the MongoDB shell editor.
     *
     * Models two orthogonal layers inspired by `@mongosh/autocomplete` +
     * `@mongosh/shell-api` and `@mongodb-js/compass-editor`:
     *   1. Receiver return-type chain resolution (`db` -> `Database`, `db.coll` /
     *      `db.getCollection('coll')` / `db['coll']` -> `Collection`, `.find(...)` /
     *      `.sort(...)` / `.limit(...)` -> `QueryCursor`, `.aggregate(...)` ->
     *      `AggCursor`, `.explain(...)` -> `Explainable`, `db.system.` ->
     *      `SystemNamespace`, `db.getMongo().` -> `Mongo`, `rs.` -> `ReplicaSet`,
     *      `sh.` -> `Shard`, `EJSON.` -> `Ejson`, `console.` -> `Console`, plus
     *      local variable bindings `const cur = db.orders.find({}); cur.`).
     *   2. MQL nested document, array, stage, and call-argument role resolution
     *      (`PipelineStageKey`, `UpdateOperatorKey`, `FieldConditionKey`, `FilterKey`,
     *      `FieldsKey`, `GroupOutputKey`, `GroupFieldValue`, `AggExpressionKey`,
     *      `QuotedFieldKey`, `FieldReference`, `DistinctFieldArg`,
     *      `CollectionNameArg`, `DatabaseNameArg`, `ExplainVerbosityArg`,
     *      `ShowCommand`, `UseCommand`, `StatementStart`, `Expression`).
     */
    namespace ShellCompletion
    {
        enum class Scope
        {
            None,               ///< Inside comment, arbitrary string, after number `12.`, terminal call, etc.
            MemberAccess,       ///< After `<receiver>.<prefix>`
            PipelineStageKey,   ///< `{ | }` inside `aggregate([...])`, `$lookup.pipeline`, `$facet` array
            UpdateOperatorKey,  ///< `{ | }` in update doc (`updateOne({}, { | })`, `findAndModify({ update: { | } })`)
            FieldConditionKey,  ///< `{ field: { | } }` or `$not: { | }`
            FilterKey,          ///< `{ | }` in query filter (`find({ | })`, `$match: { | }`, `$or: [{ | }]`, `$elemMatch: { | }`)
            FieldsKey,          ///< `{ | }` in `sort`, `projection`, `hint`, `createIndex`, `$set`/`$inc`, `$sort`, `$project`, `insertOne`
            GroupOutputKey,     ///< `{ | }` directly inside `$group: { | }` (`_id` first)
            GroupFieldValue,    ///< `{ | }` in `$group: { outField: { | } }` (`$sum`, `$avg`, ...)
            AggExpressionKey,   ///< `{ | }` in `$expr: { | }`, `$project: { f: { | } }`, `$addFields: { f: { | } }`
            SpecKey,            ///< `{ | }` in structured helper specs (`$lookup`, `findAndModify`, `bulkWrite`)
            UnboundOperatorKey, ///< `{ $ma|` in a standalone JS object literal (`const stage = { $ma`)
            QuotedFieldKey,     ///< `{ "address.|` in a filter or fields object
            FieldReference,     ///< `"$field` in an aggregation expression / projection / `$group._id`
            DistinctFieldArg,   ///< `distinct("st|` or `$lookup` `localField: "st|`
            CollectionNameArg,  ///< `db.getCollection("ord|`, `db['ord|`, `$lookup` `from: "ord|`
            DatabaseNameArg,    ///< `db.getSiblingDB("adm|` / `use("adm|`
            ExplainVerbosityArg,///< `explain("exec|`
            ShowCommand,        ///< `show |`
            UseCommand,         ///< `use |`
            StatementStart,     ///< Top-level or JS block statement start
            Expression,         ///< Value position `{ a: | }` or call argument `fn(|)`
        };

        enum class Receiver
        {
            None,
            Database,
            SiblingDatabase,
            SystemNamespace,
            Mongo,
            Collection,
            QueryCursor,
            AggCursor,
            Explainable,
            ReplicaSet,
            Shard,
            Ejson,
            Console,
        };

        struct Context
        {
            Scope scope = Scope::None;
            Receiver receiver = Receiver::None;
            QString collection;       ///< Inferred target collection in active DB (empty if unknown or sibling DB)
            QString helperKind;       ///< Spec subtype for Scope::SpecKey ("lookup", "findAndModify", "bulkWriteOp", "bulkWriteSpec")
            QString prefix;           ///< Typed token/prefix being completed (without leading '$' for FieldReference)
            QChar quoteChar;          ///< Enclosing quote ('\'' or '"') for string-argument scopes
            int replaceLength = 0;    ///< UTF-16 code units before caret replaced by `Item::insert`
        };

        struct Item
        {
            QString label;
            QString insert;
            QString detail;
        };

        struct Environment
        {
            QStringList collections;              ///< Known collections in active DB
            QStringList databases;                ///< Known databases on server
            std::vector<SchemaField> fields;      ///< Schema fields for `context.collection` (if analyzed)
            AutocompletionMode mode = AutocompleteAll;
            bool explicitTrigger = false;         ///< Ctrl+Space / Meta+Space
            bool delimiterTrigger = false;        ///< Triggered by '{', ',', '"', '\'', ' '
        };

        struct Result
        {
            Context context;
            std::vector<Item> items;
            bool needsLiveCollections = false;    ///< Active DB collections can enrich this context
            bool needsSchema = false;             ///< `context.collection` schema fields can enrich this context
        };

        Context analyze(const QString &textBeforeCaret);
        Result complete(const QString &textBeforeCaret, const Environment &env, int maxItems = 200);

        /// Format a collection name for property access after `db.` (`orders` vs `getCollection('my-coll')`).
        QString formatDbCollectionMember(const QString &collectionName);
        /// True when `collectionName` can be accessed as `db.<collectionName>` without `getCollection(...)`.
        bool isDotAccessibleCollection(const QString &collectionName);
    }
}
