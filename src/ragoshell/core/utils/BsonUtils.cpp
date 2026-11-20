#include "ragoshell/core/utils/BsonUtils.h"

#include <cmath>
#include <limits>
#include <sstream>

#include <QByteArray>
#include <QString>

#include <bsoncxx/json.hpp>

#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/HexUtils.h"

// v0.9
#include "ragoshell/shell/db/ptimeutil.h"

namespace RagoShell
{
    namespace
    {
        // Replacement for mongo::str::escape(): escapes characters that are
        // unsafe or invisible/control-coded inside a JSON/shell string literal,
        // including ASCII C0 (0x00..0x1F), DEL (0x7F), UTF-8 C1 control codes
        // (U+0080..U+009F = 0xC2 0x80..0x9F, such as U+009C STRING TERMINATOR),
        // and JS line/paragraph separators (U+2028 / U+2029).
        std::string escapeJsonString(const bsoncxx::stdx::string_view &str,
                                     bool singleQuotes = false)
        {
            std::string out;
            out.reserve(str.size() + 8);
            const size_t n = str.size();
            for (size_t i = 0; i < n; ++i) {
                const char c = str[i];
                const unsigned char uc = static_cast<unsigned char>(c);
                switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\'': if (singleQuotes) out += "\\'"; else out += c; break;
                default:
                    if (uc < 0x20 || uc == 0x7F) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(uc));
                        out += buf;
                    } else if (uc == 0xC2 && i + 1 < n) {
                        const unsigned char next = static_cast<unsigned char>(str[i + 1]);
                        if (next >= 0x80 && next <= 0x9F) {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(next));
                            out += buf;
                            ++i;
                        } else {
                            out += c;
                        }
                    } else if (uc == 0xE2 && i + 2 < n &&
                               static_cast<unsigned char>(str[i + 1]) == 0x80 &&
                               (static_cast<unsigned char>(str[i + 2]) == 0xA8 ||
                                static_cast<unsigned char>(str[i + 2]) == 0xA9)) {
                        out += (static_cast<unsigned char>(str[i + 2]) == 0xA8)
                            ? "\\u2028"
                            : "\\u2029";
                        i += 2;
                    } else {
                        out += c;
                    }
                }
            }
            return out;
        }

        std::string base64Encode(const char *data, int len)
        {
            return QByteArray(data, len).toBase64().toStdString();
        }

        // Returns the largest prefix length <= maxBytes that does not split a multi-byte UTF-8 sequence.
        size_t utf8SafePrefixLen(const bsoncxx::stdx::string_view &sv, size_t maxBytes)
        {
            if (sv.size() <= maxBytes)
                return sv.size();
            size_t len = maxBytes;
            while (len > 0 && (static_cast<unsigned char>(sv[len]) & 0xC0) == 0x80)
                --len;
            return len;
        }
    }

    namespace BsonUtils
    {
        std::string jsonString(const bsoncxx::document::view_or_value &obj, JsonStringFormat format,
                               int pretty, UUIDEncoding uuidEncoding, SupportedTimes timeFormat,
                               bool isArray, size_t maxFieldPreviewBytes)
        {
            using namespace std;
            const bsoncxx::document::view doc = obj.view();

            if (doc.empty()) {
                return isArray ? "[]" : "{}";
            }

            std::string out;
            const size_t reserveCap = maxFieldPreviewBytes > 0
                ? static_cast<size_t>(65536)
                : static_cast<size_t>(4 * 1024 * 1024);
            out.reserve(std::min<size_t>(static_cast<size_t>(doc.length()) * 2, reserveCap));
            out += (isArray ? "[" : "{");

            auto it = doc.begin();
            if (it != doc.end()) {
                while (true) {
                    const bsoncxx::document::element e = *it;
                    if (pretty) {
                        out += '\n';
                        for (int x = 0; x < pretty; x++)
                            out += "    ";
                    } else {
                        out += ' ';
                    }

                    out += jsonString(e, format, true, pretty ? pretty + 1 : 0,
                                      uuidEncoding, timeFormat, isArray, maxFieldPreviewBytes);

                    ++it;
                    if (it == doc.end()) {
                        if (pretty) {
                            out += '\n';
                            for (int x = 0; x < pretty - 1; x++)
                                out += "    ";
                        } else {
                            out += ' ';
                        }
                        out += (isArray ? "]" : "}");
                        break;
                    }
                    out += ",";
                }
            }
            return out;
        }

        std::string jsonString(const bsoncxx::document::element &elem, JsonStringFormat format,
                               bool includeFieldNames, int pretty, UUIDEncoding uuidEncoding,
                               SupportedTimes timeFormat, bool isArray, size_t maxFieldPreviewBytes);

        // Templated on element type because bsoncxx::array::element privately
        // derives from bsoncxx::document::element and cannot be converted.
        template<typename E>
        std::string jsonStringImpl(const E &elem, JsonStringFormat format,
                                   bool includeFieldNames, int pretty, UUIDEncoding uuidEncoding,
                                   SupportedTimes timeFormat, bool isArray,
                                   size_t maxFieldPreviewBytes)
        {
            using namespace std;
            const bsoncxx::type t = elem.type();

            std::stringstream s;
            if (includeFieldNames && !isArray) {
                const auto key = elem.key();
                s << '"' << escapeJsonString(key) << "\" : ";
            }

            switch (t) {
            case bsoncxx::type::k_undefined:
                s << "undefined";
                break;
            case bsoncxx::type::k_string: {
                const auto sv = elem.get_string().value;
                if (maxFieldPreviewBytes > 0 && sv.size() > maxFieldPreviewBytes) {
                    const size_t safeLen = utf8SafePrefixLen(sv, maxFieldPreviewBytes);
                    s << '"' << escapeJsonString(sv.substr(0, safeLen))
                      << "...\" /* " << sv.size() << " bytes total, truncated in viewer */";
                } else {
                    s << '"' << escapeJsonString(sv) << '"';
                }
                break;
            }
            case bsoncxx::type::k_symbol: {
                const auto sv = elem.get_symbol().symbol;
                if (maxFieldPreviewBytes > 0 && sv.size() > maxFieldPreviewBytes) {
                    const size_t safeLen = utf8SafePrefixLen(sv, maxFieldPreviewBytes);
                    s << '"' << escapeJsonString(sv.substr(0, safeLen)) << "...\"";
                } else {
                    s << '"' << escapeJsonString(sv) << '"';
                }
                break;
            }
            case bsoncxx::type::k_int64:
                s << "NumberLong(" << elem.get_int64().value << ")";
                break;
            case bsoncxx::type::k_int32:
                s << elem.get_int32().value;
                break;
            case bsoncxx::type::k_double: {
                const double number = elem.get_double().value;
                if (number >= -std::numeric_limits<double>::max() &&
                    number <= std::numeric_limits<double>::max()) {
                    std::stringstream ss;
                    ss.precision(std::numeric_limits<double>::digits10);
                    ss << number;
                    s << reformatDoubleString(QString::fromStdString(ss.str()), number);
                }
                else if (std::isnan(number)) {
                    s << "NaN";
                }
                else if (std::isinf(number)) {
                    s << (std::to_string(number) == "inf" ? "Infinity" : "-Infinity");
                }
                else {
                    LOG_MSG("BsonUtils::jsonString(): Number " + std::to_string(number) +
                            " cannot be represented in JSON", RagoShell::LogSeverity::Error);
                }
                break;
            }
            case bsoncxx::type::k_decimal128:
                s << "NumberDecimal(\"" << elem.get_decimal128().value.to_string() << "\")";
                break;
            case bsoncxx::type::k_bool:
                s << (elem.get_bool().value ? "true" : "false");
                break;
            case bsoncxx::type::k_null:
                s << "null";
                break;
            case bsoncxx::type::k_document:
                s << jsonString(elem.get_document().view(), format, pretty,
                                uuidEncoding, timeFormat, false, maxFieldPreviewBytes);
                break;
            case bsoncxx::type::k_array: {
                const auto arr = elem.get_array().value;
                if (arr.empty()) {
                    s << "[]";
                    break;
                }
                s << (pretty ? "[" : "[ ");
                auto it = arr.begin();
                if (it != arr.end()) {
                    int count = 0;
                    while (true) {
                        const bsoncxx::array::element e = *it;
                        if (pretty) {
                            s << '\n';
                            for (int x = 0; x < pretty; x++)
                                s << "    ";
                        }

                        const auto ekey = e.key();
                        if (strtol(std::string(ekey.data(), ekey.size()).c_str(), nullptr, 10) > count) {
                            s << "undefined";
                        }
                        else {
                            s << jsonStringImpl(e, format, false, pretty ? pretty + 1 : 0,
                                                uuidEncoding, timeFormat, true, maxFieldPreviewBytes);
                            ++it;
                        }
                        ++count;
                        if (it == arr.end()) {
                            if (pretty) {
                                s << '\n';
                                for (int x = 0; x < pretty - 1; x++)
                                    s << "    ";
                            } else {
                                s << ' ';
                            }
                            s << "]";
                            break;
                        }
                        s << (pretty ? "," : ", ");
                    }
                }
                break;
            }
            case bsoncxx::type::k_dbpointer: {
                const auto dbp = elem.get_dbpointer();
                const bsoncxx::oid &x = dbp.value;
                if (format == TenGen)
                    s << "DBRef(";
                else
                    s << "{ \"$ref\" : ";
                s << '"' << std::string(dbp.collection.data(), dbp.collection.size()) << "\", ";
                if (format != TenGen)
                    s << "\"$id\" : ";
                s << '"' << x.to_string() << "\"";
                if (format == TenGen)
                    s << ')';
                else
                    s << '}';
                break;
            }
            case bsoncxx::type::k_oid:
                s << (format == TenGen ? "ObjectId(" : "{ \"$oid\" : ");
                s << '"' << elem.get_oid().value.to_string() << '"';
                s << (format == TenGen ? ")" : " }");
                break;
            case bsoncxx::type::k_binary: {
                const auto bin = elem.get_binary();
                if (bin.sub_type == bsoncxx::binary_sub_type::k_uuid_deprecated ||
                    bin.sub_type == bsoncxx::binary_sub_type::k_uuid) {
                    s << HexUtils::formatUuid(bin, uuidEncoding);
                    break;
                }

                const bool truncatedBin = (maxFieldPreviewBytes > 0 && bin.size > maxFieldPreviewBytes);
                const int encodeBytes = truncatedBin
                    ? static_cast<int>(maxFieldPreviewBytes)
                    : static_cast<int>(bin.size);

                s << "{ \"$binary\" : \""
                  << base64Encode(reinterpret_cast<const char*>(bin.bytes), encodeBytes)
                  << (truncatedBin ? "..." : "")
                  << "\", \"$type\" : \"" << std::hex;
                s.width(2);
                s.fill('0');
                s << static_cast<int>(bin.sub_type) << std::dec << "\"";
                if (truncatedBin)
                    s << " /* " << bin.size << " bytes total, truncated in viewer */";
                s << " }";
                break;
            }
            case bsoncxx::type::k_date: {
                const int64_t ms = elem.get_date().value.count();
                const bool isSupportedDate = miutil::minDate < ms && ms < miutil::maxDate;

                if (format == Strict)
                    s << "{ \"$date\" : ";
                else
                    s << (isSupportedDate ? "ISODate(" : "Date(");

                if (pretty && isSupportedDate)
                    s << '"' << miutil::isotimeString(ms, true, timeFormat == LocalTime) << '"';
                else
                    s << ms;

                s << (format == Strict ? " }" : ")");
                break;
            }
            case bsoncxx::type::k_regex: {
                const auto re = elem.get_regex();
                if (format == Strict) {
                    s << "{ \"$regex\" : \"" << escapeJsonString(re.regex)
                      << "\", \"$options\" : \"" << std::string(re.options.data(), re.options.size())
                      << "\" }";
                }
                else {
                    s << "/" << escapeJsonString(re.regex, true) << "/";
                    const std::string flags(re.options.data(), re.options.size());
                    for (const char f : flags) {
                        switch (f) {
                        case 'g':
                        case 'i':
                        case 'm':
                            s << f;
                        default:
                            break;
                        }
                    }
                }
                break;
            }
            case bsoncxx::type::k_codewscope: {
                const auto cws = elem.get_codewscope();
                const auto scope = cws.scope;
                if (!scope.empty()) {
                    s << "{ \"$code\" : \"" << escapeJsonString(cws.code) << "\", "
                      << "\"$scope\" : " << bsoncxx::to_json(scope) << " }";
                    break;
                }
                s << "{ \"$code\" : \"" << escapeJsonString(cws.code) << "\" }";
                break;
            }
            case bsoncxx::type::k_code:
                s << "{ \"$code\" : \"" << escapeJsonString(elem.get_code().code) << "\" }";
                break;
            case bsoncxx::type::k_timestamp: {
                const auto ts = elem.get_timestamp();
                if (format == TenGen)
                    s << "Timestamp(" << ts.timestamp << ", " << ts.increment << ")";
                else
                    s << "{ \"$timestamp\" : { \"t\" : " << ts.timestamp
                      << ", \"i\" : " << ts.increment << " } }";
                break;
            }
            case bsoncxx::type::k_minkey:
                s << "{ \"$minKey\" : 1 }";
                break;
            case bsoncxx::type::k_maxkey:
                s << "{ \"$maxKey\" : 1 }";
                break;
            default: {
                LOG_MSG("BsonUtils::jsonString(): Cannot create a properly formatted JSON string "
                        "with element of type: " + std::to_string(static_cast<int>(t)),
                        RagoShell::LogSeverity::Error);
            }
            }
            return s.str();
        }

        std::string jsonString(const bsoncxx::document::element &elem, JsonStringFormat format,
                               bool includeFieldNames, int pretty, UUIDEncoding uuidEncoding,
                               SupportedTimes timeFormat, bool isArray, size_t maxFieldPreviewBytes)
        {
            return jsonStringImpl(elem, format, includeFieldNames, pretty,
                                  uuidEncoding, timeFormat, isArray, maxFieldPreviewBytes);
        }

        bool isArray(const bsoncxx::document::element &elem)
        {
            return isArray(elem.type());
        }

        bool isArray(bsoncxx::type type)
        {
            return type == bsoncxx::type::k_array;
        }

        bool isDocument(const bsoncxx::document::element &elem)
        {
            return isDocument(elem.type());
        }

        bool isDocument(bsoncxx::type type)
        {
            return type == bsoncxx::type::k_document || type == bsoncxx::type::k_array;
        }

        bool isSimpleType(bsoncxx::type type)
        {
            switch (type) {
            case bsoncxx::type::k_int64:
            case bsoncxx::type::k_double:
            case bsoncxx::type::k_decimal128:
            case bsoncxx::type::k_int32:
            case bsoncxx::type::k_string:
            case bsoncxx::type::k_bool:
            case bsoncxx::type::k_date:
            case bsoncxx::type::k_oid:
                return true;
            default:
                return false;
            }
        }

        bool isUuidType(bsoncxx::type type, bsoncxx::binary_sub_type binDataType)
        {
            if (type != bsoncxx::type::k_binary)
                return false;

            return binDataType == bsoncxx::binary_sub_type::k_uuid ||
                   binDataType == bsoncxx::binary_sub_type::k_uuid_deprecated;
        }

        bool isSimpleType(const bsoncxx::document::element &elem)
        {
            return isSimpleType(elem.type());
        }

        bool isUuidType(const bsoncxx::document::element &elem)
        {
            if (elem.type() != bsoncxx::type::k_binary)
                return false;

            const bsoncxx::binary_sub_type binType = elem.get_binary().sub_type;
            return binType == bsoncxx::binary_sub_type::k_uuid ||
                   binType == bsoncxx::binary_sub_type::k_uuid_deprecated;
        }

        const char* BSONTypeToString(bsoncxx::type type, bsoncxx::binary_sub_type binDataType,
                                     UUIDEncoding uuidEncoding)
        {
            switch (type) {
            case bsoncxx::type::k_double:     return "Double";
            case bsoncxx::type::k_decimal128: return "Decimal128";
            case bsoncxx::type::k_string:     return "String";
            case bsoncxx::type::k_document:   return "Object";
            case bsoncxx::type::k_array:      return "Array";
            case bsoncxx::type::k_binary:
                if (binDataType == bsoncxx::binary_sub_type::k_uuid)
                    return "UUID";
                if (binDataType == bsoncxx::binary_sub_type::k_uuid_deprecated) {
                    switch (uuidEncoding) {
                    case DefaultEncoding: return "Legacy UUID";
                    case JavaLegacy:      return "Java UUID (Legacy)";
                    case CSharpLegacy:    return ".NET UUID (Legacy)";
                    case PythonLegacy:    return "Python UUID (Legacy)";
                    default:              return "Legacy UUID";
                    }
                }
                return "Binary";
            case bsoncxx::type::k_undefined:  return "Undefined";
            case bsoncxx::type::k_oid:        return "ObjectId";
            case bsoncxx::type::k_bool:       return "Boolean";
            case bsoncxx::type::k_date:       return "Date";
            case bsoncxx::type::k_null:       return "Null";
            case bsoncxx::type::k_regex:      return "Regular Expression";
            case bsoncxx::type::k_dbpointer:  return "DBRef";
            case bsoncxx::type::k_code:       return "Code";
            case bsoncxx::type::k_symbol:     return "Symbol";
            case bsoncxx::type::k_codewscope: return "CodeWScope";
            case bsoncxx::type::k_int32:      return "Int32";
            case bsoncxx::type::k_timestamp:  return "Timestamp";
            case bsoncxx::type::k_int64:      return "Int64";
            default:                          return "Type is not supported";
            }
        }

        void buildJsonString(const bsoncxx::document::view_or_value &obj, std::string &con,
                             UUIDEncoding uuid, SupportedTimes tz, size_t maxFieldPreviewBytes)
        {
            const bsoncxx::document::view doc = obj.view();
            con.append("{ \n");
            for (const auto &e : doc) {
                con.append("\"");
                const auto key = e.key();
                con.append(key.data(), key.size());
                con.append("\"");
                con.append(" : ");
                buildJsonString(e, con, uuid, tz, maxFieldPreviewBytes);
                con.append(", \n");
            }
            con.append("\n}\n\n");
        }

        void buildJsonString(const bsoncxx::document::element &elem, std::string &con,
                             UUIDEncoding uuid, SupportedTimes tz, size_t maxFieldPreviewBytes)
        {
            using bsoncxx::type;
            switch (elem.type()) {
            case type::k_double: {
                const double number = elem.get_double().value;
                if (number >= -std::numeric_limits<double>::max() &&
                    number <= std::numeric_limits<double>::max()) {
                    std::stringstream ss;
                    ss.precision(std::numeric_limits<double>::digits10);
                    ss << number;
                    con.append(reformatDoubleString(QString::fromStdString(ss.str()), number));
                }
                else if (std::isnan(number)) {
                    con.append("NaN");
                }
                else if (std::isinf(number)) {
                    con.append(std::to_string(number) == "inf" ? "Infinity" : "-Infinity");
                }
                else {
                    LOG_MSG("BsonUtils::buildJsonString(): Number " + std::to_string(number) +
                            " cannot be represented in JSON", RagoShell::LogSeverity::Error);
                }
                break;
            }
            case type::k_string: {
                const auto sv = elem.get_string().value;
                const size_t n = maxFieldPreviewBytes > 0
                    ? utf8SafePrefixLen(sv, maxFieldPreviewBytes)
                    : sv.size();
                con.append(sv.data(), n);
                break;
            }
            case type::k_document:
                buildJsonString(elem.get_document().view(), con, uuid, tz, maxFieldPreviewBytes);
                break;
            case type::k_array: {
                const auto arr = elem.get_array().value;
                buildJsonString(bsoncxx::document::view(arr.data(), arr.length()), con, uuid, tz, maxFieldPreviewBytes);
                break;
            }
            case type::k_binary: {
                const bsoncxx::binary_sub_type binType = elem.get_binary().sub_type;
                if (binType == bsoncxx::binary_sub_type::k_uuid ||
                    binType == bsoncxx::binary_sub_type::k_uuid_deprecated) {
                    con.append(HexUtils::formatUuid(elem.get_binary(), uuid));
                    break;
                }
                con.append("<binary>");
                break;
            }
            case type::k_undefined:
                con.append("undefined");
                break;
            case type::k_oid: {
                const std::string idValue = elem.get_oid().value.to_string();
                con.append("ObjectId(\"" + idValue + "\")");
                break;
            }
            case type::k_bool:
                con.append(elem.get_bool().value ? "true" : "false");
                break;
            case type::k_date: {
                const int64_t ms = elem.get_date().value.count();
                const bool isSupportedDate = miutil::minDate < ms && ms < miutil::maxDate;
                if (isSupportedDate)
                    con.append(miutil::isotimeString(ms, false, tz == LocalTime));
                else
                    con.append(std::to_string(ms));
                break;
            }
            case type::k_null:
                con.append("null");
                break;
            case type::k_regex: {
                const auto re = elem.get_regex();
                con.append("/" + std::string(re.regex.data(), re.regex.size()) + "/");
                const std::string flags(re.options.data(), re.options.size());
                for (const char f : flags) {
                    switch (f) {
                    case 'g':
                    case 'i':
                    case 'm':
                        con += f;
                    default:
                        break;
                    }
                }
                break;
            }
            case type::k_dbpointer:
                break;
            case type::k_code: {
                const auto c = elem.get_code().code;
                const size_t n = maxFieldPreviewBytes > 0
                    ? utf8SafePrefixLen(c, maxFieldPreviewBytes)
                    : c.size();
                con.append(c.data(), n);
                break;
            }
            case type::k_symbol: {
                const auto sv = elem.get_symbol().symbol;
                const size_t n = maxFieldPreviewBytes > 0
                    ? utf8SafePrefixLen(sv, maxFieldPreviewBytes)
                    : sv.size();
                con.append(sv.data(), n);
                break;
            }
            case type::k_codewscope: {
                const auto cws = elem.get_codewscope();
                const size_t n = maxFieldPreviewBytes > 0
                    ? utf8SafePrefixLen(cws.code, maxFieldPreviewBytes)
                    : cws.code.size();
                con.append(cws.code.data(), n);
                break;
            }
            case type::k_int32:
                con.append(std::to_string(elem.get_int32().value));
                break;
            case type::k_timestamp: {
                const auto ts = elem.get_timestamp();
                con.append(std::to_string(ts.timestamp) + "." + std::to_string(ts.increment));
                break;
            }
            case type::k_int64:
                con.append(std::to_string(elem.get_int64().value));
                break;
            case type::k_decimal128:
                con.append(elem.get_decimal128().value.to_string());
                break;
            default:
                con.append("<unsupported>");
                break;
            }
        }

        bsoncxx::document::element indexOf(const bsoncxx::document::view &doc, int index)
        {
            int i = 0;
            for (const auto &element : doc) {
                if (i++ == index)
                    return element;
            }
            return bsoncxx::document::element();
        }

        int elementsCount(const bsoncxx::document::view &doc)
        {
            return static_cast<int>(std::distance(doc.begin(), doc.end()));
        }

        std::string reformatDoubleString(QString str, double elemDouble)
        {
            // Leave trailing zero if needed
            if (!str.contains("e+", Qt::CaseInsensitive) &&
                !str.contains("e-", Qt::CaseInsensitive) && elemDouble == (long long)elemDouble)
                str.append(".0");
            else if (str.endsWith("e+15", Qt::CaseInsensitive) ||
                     str.endsWith("e+16", Qt::CaseInsensitive)) {
                // Disable scientific format
                std::stringstream ss2;
                ss2.precision(std::numeric_limits<double>::digits10);
                ss2 << std::fixed << elemDouble;
                str = QString::fromStdString(ss2.str());
                while (str.contains('.') && str.endsWith("00"))
                    str.chop(1);
            }

            return str.toStdString();
        }

        QString sanitizeForDisplay(const QString &str, bool singleLine)
        {
            if (str.isEmpty())
                return str;

            QString cleaned;
            cleaned.reserve(str.size());
            const int n = str.size();
            for (int i = 0; i < n; ++i) {
                const QChar ch = str[i];
                const ushort u = ch.unicode();

                // Preserve valid UTF-16 surrogate pairs (emoji / supplementary planes)
                if (ch.isHighSurrogate()) {
                    if (i + 1 < n && str[i + 1].isLowSurrogate()) {
                        cleaned += ch;
                        cleaned += str[i + 1];
                        ++i;
                    } else {
                        cleaned += QChar::ReplacementCharacter;
                    }
                    continue;
                }
                if (ch.isLowSurrogate()) {
                    cleaned += QChar::ReplacementCharacter;
                    continue;
                }

                // Standard ASCII whitespace
                if (u == 0x09 || u == 0x0A || u == 0x0D) {
                    cleaned += singleLine ? QLatin1Char(' ') : ch;
                    continue;
                }

                // Non-whitespace C0 controls (0x00..0x1F) and DEL + C1 controls (0x7F..0x9F,
                // e.g. U+009C STRING TERMINATOR from Latin-1 mojibake of UTF-8 'Ü' = 0xC3 0x9C)
                if (u < 0x20 || (u >= 0x7F && u <= 0x9F)) {
                    cleaned += QChar::ReplacementCharacter;
                    continue;
                }

                cleaned += ch;
            }

            return singleLine ? cleaned.simplified() : cleaned;
        }

    } // BsonUtils
} // RagoShell
