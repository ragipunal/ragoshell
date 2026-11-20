#pragma once

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>

namespace RagoShell
{
    struct AggrInfo
    {
        AggrInfo() {}

        AggrInfo(const std::string& collectionName, int skip, int batchSize, 
                 bsoncxx::document::view_or_value pipeline, bsoncxx::document::view_or_value options, int resultIndex) :
            collectionName(collectionName), skip(skip), batchSize(batchSize), pipeline(pipeline.view()), 
            options(options.view()), isValid(true), resultIndex(resultIndex)
        {}

        std::string collectionName = "";
        int skip = 0;
        int batchSize = 0;   
        bsoncxx::document::value pipeline = bsoncxx::from_json("{}");
        bsoncxx::document::value options = bsoncxx::from_json("{}");
        bool isValid = false;
        int resultIndex = -1;
    };
}
