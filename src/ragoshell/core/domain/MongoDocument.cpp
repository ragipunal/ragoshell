#include "ragoshell/core/domain/MongoDocument.h"

#include <bsoncxx/builder/stream/document.hpp>

#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/utils/BsonUtils.h"

namespace RagoShell
{
    MongoDocument::MongoDocument()
        : _bsonObj(bsoncxx::builder::stream::document{} << bsoncxx::builder::stream::finalize)
    {
    }

    MongoDocument::~MongoDocument()
    {
    }

    /*
    ** Create MongoDocument from bsoncxx::document::value (takes ownership)
    */
    MongoDocument::MongoDocument(bsoncxx::document::value bsonObj, bool isArray)
        : _bsonObj(std::move(bsonObj)), _isArray(isArray)
    {
    }

    /*
    ** Create MongoDocument from a document::view. It will take an owned copy.
    */
    MongoDocumentPtr MongoDocument::fromBsonObj(bsoncxx::document::view bsonObj, bool isArray)
    {
        MongoDocument *doc = new MongoDocument(bsoncxx::document::value(bsonObj), isArray);
        return MongoDocumentPtr(doc);
    }

    MongoDocumentPtr MongoDocument::fromBsonObj(const bsoncxx::document::value &bsonObj,
                                                bool isArray)
    {
        MongoDocument *doc = new MongoDocument(bsoncxx::document::value(bsonObj.view()), isArray);
        return MongoDocumentPtr(doc);
    }

    /*
    ** Create list of MongoDocuments from a vector of owned values.
    */
    std::vector<MongoDocumentPtr> MongoDocument::fromBsonObj(
        const std::vector<bsoncxx::document::value> &bsonObjs)
    {
        std::vector<MongoDocumentPtr> list;
        for (const auto &obj : bsonObjs) {
            list.push_back(fromBsonObj(obj.view()));
        }

        return list;
    }
}
