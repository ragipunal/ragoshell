#pragma once

#include <QStringList>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>

#include "ragoshell/core/Core.h"

namespace RagoShell
{
    /*
    ** Represents MongoDB object.
    */
    class MongoDocument
    {
        /*
        ** Owned BSON document value
        */
        const bsoncxx::document::value _bsonObj;
        // Marks top-level results that are BSON arrays serialised as
        // documents with numeric keys ("0","1",...). Replaces the old
        // mongo::BSONObj::isArray() marker from the embedded MongoDB fork.
        const bool _isArray = false;
    public:
        /*
        ** Constructs empty Document, i.e. { }
        */
        MongoDocument();
        ~MongoDocument();

        /*
        ** Create MongoDocument from bsoncxx::document::value (takes ownership)
        */
        MongoDocument(bsoncxx::document::value bsonObj, bool isArray = false);

        /*
        ** Create MongoDocument from a document::view. It will take an owned copy.
        */
        static MongoDocumentPtr fromBsonObj(bsoncxx::document::view bsonObj, bool isArray = false);
        static MongoDocumentPtr fromBsonObj(const bsoncxx::document::value &bsonObj,
                                            bool isArray = false);

        /*
        ** Create list of MongoDocuments from a vector of owned values.
        */
        static std::vector<MongoDocumentPtr> fromBsonObj(
            const std::vector<bsoncxx::document::value> &bsonObj);

        /*
        ** Return a view of the owned BSON document
        */
        bsoncxx::document::view bsonObj() const { return _bsonObj.view(); }

        /*
        ** Whether this document represents a top-level BSON array
        */
        bool isArray() const { return _isArray; }
    };
}
