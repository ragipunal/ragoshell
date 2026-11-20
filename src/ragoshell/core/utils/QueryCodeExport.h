#pragma once

#include <QString>
#include <QStringList>

namespace RagoShell
{
    /**
     * @brief Turns a shell find()/aggregate() statement into driver code.
     *
     * Python (PyMongo) and Node.js get native literals that keep BSON types
     * (ObjectId, Int64, Decimal128, dates, regexes, binaries…) with the
     * imports they need; Java and C# parse relaxed Extended JSON with
     * Document.parse / BsonDocument.Parse.
     */
    namespace QueryCodeExport
    {
        enum class Language { Python = 0, NodeJs, Java, CSharp };

        /// Display names in Language order.
        QStringList languageNames();

        struct Result
        {
            bool ok = false;
            QString error;
            QString code;
        };

        /**
         * @param script  one find() chain (filter, projection, sort, skip,
         *                limit) or aggregate([...]) statement
         * @param connectionUri URI to put in the generated code (no credentials)
         */
        Result exportScript(const QString &script, const QString &database,
                            const QString &connectionUri, Language language);
    }
}
