#pragma once

#include <bsoncxx/document/value.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/document/view_or_value.hpp>
#include "ragoshell/core/domain/MongoNamespace.h"

namespace RagoShell
{
    namespace detail
    {
        std::string prepareServerAddress(const std::string &address);
    }

    struct CollectionInfo
    {
        CollectionInfo();
        CollectionInfo(const std::string &server, const std::string &database, const std::string &collection);
        bool isValid() const;

        std::string _serverAddress;
        MongoNamespace _ns;
    };

    struct MongoQueryInfo
    {
        MongoQueryInfo();

        MongoQueryInfo(const CollectionInfo &info,
                  bsoncxx::document::view_or_value query, bsoncxx::document::view_or_value fields, int limit, int skip, int batchSize,
                  int options, bool special);

        CollectionInfo _info;
        bsoncxx::document::value _query = bsoncxx::from_json("{}");
        bsoncxx::document::value _fields = bsoncxx::from_json("{}");
        int _limit = 0;
        int _skip = 0;
        int _batchSize = 0;
        int _options = 0;
        bool _special = false; // flag, indicating that `query` contains special fields on
                      // first level, and query data in `query` field.
        
    };
}
