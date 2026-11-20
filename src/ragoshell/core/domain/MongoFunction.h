#pragma once

#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>

namespace RagoShell
{
    class MongoFunction
    {
    public:
        /**
         * @brief Creates function from "system.js" document
         */
        MongoFunction(bsoncxx::document::view_or_value obj);

        /**
         * @brief Creates new function with empty attributes
         */
        MongoFunction() {}

        std::string name() const { return _name; }
        std::string code() const { return _code; }

        void setCode(const std::string &code) { _code = code; }
        void setName(const std::string &name) { _name = name; }

        bsoncxx::document::value toBson() const;

    private:
        std::string _name;
        std::string _code;
    };

}
