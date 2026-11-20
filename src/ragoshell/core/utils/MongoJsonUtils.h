#pragma once

#include <string>
#include <vector>
#include <QString>
#include <QStringList>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include "ragoshell/core/Enums.h"

namespace RagoShell
{
    namespace MongoJsonUtils
    {
        /**
         * @brief Result of generating an insert template when duplicating an existing BSON document.
         */
        struct DuplicateTemplateResult
        {
            QString json;
            int caretUtf16Offset = -1; ///< UTF-16 offset for initial caret (-1 = default)
        };

        /**
         * @brief Builds the JSON template for duplicating `srcDoc`.
         *        - When `_id` is `ObjectId` (or absent), `_id` is omitted so MongoDB assigns a new one.
         *        - When `_id` is any other BSON type (`String`, `Int32`, `Int64`, `UUID`, etc.),
         *          `"_id" : ""` is placed at the top and `caretUtf16Offset` points between `""`.
         */
        DuplicateTemplateResult buildDuplicateDocumentTemplate(bsoncxx::document::view srcDoc,
                                                               UUIDEncoding uuidEncoding = DefaultEncoding,
                                                               SupportedTimes timeFormat = Utc);

        /**
         * @brief Normalizes Mongo Shell JSON (comments, single quotes, unquoted keys,
         *        trailing commas, ObjectId/ISODate/NumberLong/NumberInt/NumberDecimal/
         *        UUID/LUUID/NUUID/JUUID/PYUUID/Timestamp/BinData/HexData/DBRef/MinKey/MaxKey)
         *        into strict Extended JSON v2 without mutating quoted string literals.
         */
        QString normalizeShellJsonToExtendedJson(const QString &input);

        /// Parses one or more BSON documents from Extended JSON or Mongo Shell JSON.
        /// Accepts a single `{...}`, a JSON array `[{...}, ...]`, or concatenated
        /// top-level `{...}` blocks (e.g. separated by `/* 1 */` headers).
        bool parseDocuments(const QString &input,
                            std::vector<bsoncxx::document::value> &outDocs,
                            QString &outError);

        bool parseDocuments(const QString &input,
                            std::vector<bsoncxx::document::value> &outDocs,
                            QString *outError = nullptr);

        /**
         * @brief Parses a single BSON document from Extended JSON or Mongo Shell JSON.
         */
        bool parseSingleDocument(const QString &input,
                                 bsoncxx::document::value &outDoc,
                                 QString &outError);

        bool parseSingleDocument(const QString &input,
                                 bsoncxx::document::value &outDoc,
                                 QString *outError = nullptr);

        /**
         * @brief Pretty-prints Mongo Shell / Extended JSON using the app's BSON formatter.
         */
        bool formatJsonDocuments(const QString &input,
                                 QString &outFormatted,
                                 QString &outError);

        /**
         * @brief Pretty-prints strict/relaxed JSON with configurable space indentation.
         */
        QString formatPrettyJson(const QString &input, int indent = 2);

        /**
         * @brief Clones `origDoc` while updating a single scalar field at `dottedPath`
         *        (e.g. `"status"` or `"profile.age"`), preserving `_id` and all other BSON types.
         */
        bool updateScalarFieldAtPath(bsoncxx::document::view origDoc,
                                     const QString &dottedPath,
                                     const QString &rawInput,
                                     const QString &typeHint,
                                     bsoncxx::document::value &outDoc,
                                     QString &outError);

        /**
         * @brief Extracts `ISODate("...")` UTC timestamp from an ObjectId string or literal.
         */
        QString extractIsoDateFromObjectId(const QString &oidText);

        /**
         * @brief Structured representation of a single Mongo Shell `find(...)` query chain
         *        used for two-way live synchronization between QueryBar and the script editor.
         */
        struct ParsedFindQuery
        {
            bool valid = false;
            QString collection;
            bool useGetCollection = true;
            QString filter = QStringLiteral("{}");
            QString projection;
            QString sort;
            int limit = 0;
            int skip = 0;
            bool explain = false;
            QString explainArg;
        };

        ParsedFindQuery parseFindQueryChain(const QString &script);
        QString buildFindQueryChain(const ParsedFindQuery &query);

        /**
         * @brief A single `db.<coll>.aggregate([...][, options])` statement.
         */
        struct ParsedAggregateQuery
        {
            bool valid = false;
            QString collection;
            QString pipeline;   ///< the pipeline array text, e.g. "[{ $match: {} }]"
        };

        ParsedAggregateQuery parseAggregateChain(const QString &script);

        /**
         * @brief Inspects a Mongo Shell script (ignoring JS comments, string literals,
         *        and `.explain()` prefixes) and returns human-readable labels of any
         *        write, mutating, or destructive operations found — including Delete/Drop
         *        (`deleteOne()`, `deleteMany()`, `remove()`, `drop()`, `dropDatabase()`),
         *        Update/Modify (`updateOne()`, `updateMany()`, `replaceOne()`, `bulkWrite()`,
         *        `aggregate($merge/$out)`), and Insert/Create (`insertOne()`, `insertMany()`,
         *        `insert()`, `save()`, `createCollection()`, `createIndex()`).
         *        Returns an empty list when the script is purely read-only.
         */
        QStringList detectDestructiveScriptOperations(const QString &script);
    }
}
