#include "ragoshell/core/domain/MongoFunction.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include <bsoncxx/builder/stream/document.hpp>

namespace RagoShell
{
    MongoFunction::MongoFunction(bsoncxx::document::view_or_value obj)
    {
        _name = BsonUtils::getField<std::string>(obj, "_id");
        const auto e = obj.view()["value"];
        if (e) {
            if (e.type() == bsoncxx::type::k_code) {
                const auto c = e.get_code().code;
                _code.assign(c.data(), c.size());
            } else if (e.type() == bsoncxx::type::k_codewscope) {
                const auto c = e.get_codewscope().code;
                _code.assign(c.data(), c.size());
            } else if (e.type() == bsoncxx::type::k_string) {
                const auto s = e.get_string().value;
                _code.assign(s.data(), s.size());
            }
        }
    }

    bsoncxx::document::value MongoFunction::toBson() const
    {
        return bsoncxx::builder::stream::document{}
            << "_id" << _name
            << "value" << bsoncxx::types::b_code{_code}
            << bsoncxx::builder::stream::finalize;
    }
}
