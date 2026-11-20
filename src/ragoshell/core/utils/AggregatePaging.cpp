#include "ragoshell/core/utils/AggregatePaging.h"

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

namespace RagoShell
{
    namespace
    {
        // The text as the inside of a single-quoted JavaScript string.
        std::string jsString(const std::string &text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (const char c : text) {
                switch (c) {
                case '\\': out += "\\\\"; break;
                case '\'': out += "\\'"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                default: out += c;
                }
            }
            return out;
        }

        std::string canonicalJson(const bsoncxx::document::view &doc)
        {
            return bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_canonical);
        }
    }

    std::string aggregatePageScript(const std::string &collectionName,
                                    const bsoncxx::document::view &pipeline,
                                    const bsoncxx::document::view &options,
                                    int skip, int batchSize)
    {
        std::string stages = "[";
        for (auto const &elem : pipeline) {
            if (elem.type() != bsoncxx::type::k_document)
                break;
            stages += canonicalJson(elem.get_document().view()) + ",";
        }
        stages += "{\"$skip\":" + std::to_string(skip) + "},{\"$limit\":" + std::to_string(batchSize) + "}]";

        return "db.getCollection('" + jsString(collectionName) + "').aggregate(EJSON.parse('" + jsString(stages)
               + "'), EJSON.parse('" + jsString(canonicalJson(options)) + "'))";
    }
}
