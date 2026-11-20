#pragma once

#include <vector>

#include <QString>

#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    /**
     * @brief Field-name autocomplete for the shell editor
     * (docs/SchemaIntelligence.md): finds where a field path is
     * expected from the text before the caret, and turns the schema
     * catalog's fields into completions.
     *
     * The scan skips strings, comments, template and regex literals and
     * follows calls, objects and arrays, so it knows the collection
     * (db.orders..., db.getCollection('orders')..., cursor chains) and the
     * role of the object the caret is in (filter, projection, sort, update
     * fields, aggregation stage). It is a pragmatic scanner, not a JS parser.
     */
    namespace FieldCompletion
    {
        enum class Kind
        {
            None,
            Key,         ///< object key, unquoted: { st▮
            QuotedKey,   ///< object key in quotes: { "address.▮
            Reference,   ///< "$field" in an aggregation expression: "$st▮
            Argument,    ///< field name argument: distinct("st▮
        };

        struct Context
        {
            Kind kind = Kind::None;
            QString collection;
            QString prefix;          ///< typed so far (without the '$' of a reference)
            int replaceLength = 0;   ///< characters before the caret a completion replaces
        };

        Context analyze(const QString &textBeforeCaret);

        struct Item
        {
            QString label;    ///< field path
            QString insert;   ///< text that replaces replaceLength characters
            QString detail;   ///< "String · 98%"
        };

        /// Completions for a context, matching paths first by the catalog's
        /// order, fields seen in fewer than 5 % of the documents last.
        std::vector<Item> complete(const Context &context, const std::vector<SchemaField> &fields,
                                   int maxItems = 200);

        /// A path usable as an unquoted JavaScript object key.
        bool isPlainKey(const QString &path);
    }
}
