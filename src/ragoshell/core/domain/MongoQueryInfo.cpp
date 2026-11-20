#include "ragoshell/core/domain/MongoQueryInfo.h"


namespace RagoShell
{
    namespace detail
    {
        std::string prepareServerAddress(const std::string &address)
        {
            size_t pos = address.find_first_of("{");
            if (pos != std::string::npos) {
                return address.substr(0, pos);
            }       
            return address;
        }
    }

    CollectionInfo::CollectionInfo() {}

    CollectionInfo::CollectionInfo(const std::string &server, const std::string &database, const std::string &collection)
        :_serverAddress(server),
        _ns(database, collection)
    {}

    bool CollectionInfo::isValid() const
    {
        return !_serverAddress.empty() && _ns.isValid();
    }

    MongoQueryInfo::MongoQueryInfo() {}

    MongoQueryInfo::MongoQueryInfo(const CollectionInfo &info,
              bsoncxx::document::view_or_value query, bsoncxx::document::view_or_value fields,
              int limit, int skip, int batchSize, int options, bool special) :
        _info(info),
        _query(query.view()),
        _fields(fields.view()),
        _limit(limit),
        _skip(skip),
        _batchSize(batchSize),
        _options(options),
        _special(special)
        {}
}
