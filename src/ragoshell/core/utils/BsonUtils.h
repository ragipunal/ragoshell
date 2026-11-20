#pragma once

#include <string>
#include <vector>

#include <bsoncxx/document/element.hpp>
#include <bsoncxx/document/view_or_value.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/Enums.h"

class QString;

namespace RagoShell
{
    /**
     * Output formats for JSON serialization of BSON documents.
     * Replaces mongo::JsonStringFormat (Strict/TenGen) from the old
     * embedded MongoDB fork.
     */
    enum JsonStringFormat
    {
        TenGen = 0,  // Shell-style: ObjectId(".."), ISODate(..), NumberLong(..)
        Strict = 1   // Extended JSON: {"$oid": ".."}, {"$date": ..}
    };

    namespace BsonUtils
    {
        namespace detail
        {
            template<typename T>
            T getField(const bsoncxx::document::element &elem);

            template<>
            inline bsoncxx::document::view_or_value getField<bsoncxx::document::view_or_value>(
                const bsoncxx::document::element &elem)
            {
                return elem.get_document().view();
            }

            template<>
            inline bsoncxx::array::view getField<bsoncxx::array::view>(
                const bsoncxx::document::element &elem)
            {
                return elem.get_array().value;
            }

            template<>
            inline bool getField<bool>(const bsoncxx::document::element &elem)
            {
                return elem.get_bool().value;
            }

            template<>
            inline std::string getField<std::string>(const bsoncxx::document::element &elem)
            {
                const auto sv = elem.get_string().value;
                return std::string(sv.data(), sv.size());
            }

            template<>
            inline int getField<int>(const bsoncxx::document::element &elem)
            {
                switch (elem.type()) {
                case bsoncxx::type::k_int32:  return elem.get_int32().value;
                case bsoncxx::type::k_int64:  return static_cast<int>(elem.get_int64().value);
                case bsoncxx::type::k_double: return static_cast<int>(elem.get_double().value);
                default: return 0;
                }
            }

            template<>
            inline double getField<double>(const bsoncxx::document::element &elem)
            {
                return elem.get_double().value;
            }

            template<>
            inline int64_t getField<int64_t>(const bsoncxx::document::element &elem)
            {
                switch (elem.type()) {
                case bsoncxx::type::k_int64:  return elem.get_int64().value;
                case bsoncxx::type::k_int32:  return elem.get_int32().value;
                case bsoncxx::type::k_double: return static_cast<int64_t>(elem.get_double().value);
                default: return 0;
                }
            }
        }

        template<typename T>
        T bsonelement_cast(const bsoncxx::document::element &elem)
        {
            T result = T();
            if (elem)
                result = detail::getField<T>(elem);
            return result;
        }

        template<typename T>
        T getField(const bsoncxx::document::view_or_value &doc, const char *fieldName)
        {
            const auto elem = doc.view()[fieldName];
            return bsonelement_cast<T>(elem);
        }

        std::string jsonString(const bsoncxx::document::view_or_value &obj, JsonStringFormat format,
            int pretty, UUIDEncoding uuidEncoding, SupportedTimes timeFormat, bool isArray = false,
            size_t maxFieldPreviewBytes = 0);

        std::string jsonString(const bsoncxx::document::element &elem, JsonStringFormat format,
            bool includeFieldNames, int pretty, UUIDEncoding uuidEncoding, SupportedTimes timeFormat,
            bool isArray = false, size_t maxFieldPreviewBytes = 0);

        bool isArray(const bsoncxx::document::element &elem);
        bool isArray(bsoncxx::type type);
        bool isDocument(const bsoncxx::document::element &elem);
        bool isDocument(bsoncxx::type type);

        bool isSimpleType(bsoncxx::type type);
        bool isUuidType(bsoncxx::type type, bsoncxx::binary_sub_type binDataType);
        bool isSimpleType(const bsoncxx::document::element &elem);
        bool isUuidType(const bsoncxx::document::element &elem);

        const char* BSONTypeToString(bsoncxx::type type, bsoncxx::binary_sub_type binDataType,
            UUIDEncoding uuidEncoding);

        void buildJsonString(const bsoncxx::document::view_or_value &obj, std::string &con,
            UUIDEncoding uuid, SupportedTimes tz, size_t maxFieldPreviewBytes = 0);
        void buildJsonString(const bsoncxx::document::element &elem, std::string &con,
            UUIDEncoding uuid, SupportedTimes tz, size_t maxFieldPreviewBytes = 0);

        bsoncxx::document::element indexOf(const bsoncxx::document::view &doc, int index);
        int elementsCount(const bsoncxx::document::view &doc);

        std::string reformatDoubleString(QString str, double elemDouble);

        /**
         * Sanitizes a decoded BSON string for single-line or multi-line UI display
         * (Table View, Tree View, headers, and tooltips). Replaces non-whitespace C0
         * (U+0000..U+001F) and DEL/C1 (U+007F..U+009F, e.g. U+009C STRING TERMINATOR)
         * control characters as well as lone UTF-16 surrogates with U+FFFD so platform
         * text shapers (DirectWrite / HarfBuzz) never truncate the remainder of the string,
         * while preserving all valid UTF-8 characters (Turkish, CJK, emoji, etc.).
         */
        QString sanitizeForDisplay(const QString &str, bool singleLine = true);
    }
}
