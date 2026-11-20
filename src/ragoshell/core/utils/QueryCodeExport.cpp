#include "ragoshell/core/utils/QueryCodeExport.h"
#include <QCoreApplication>

#include <charconv>
#include <cmath>
#include <optional>
#include <set>
#include <vector>

#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>

#include <bsoncxx/array/view.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/utils/MongoJsonUtils.h"

namespace RagoShell
{
    namespace
    {
        using Language = QueryCodeExport::Language;

        constexpr int kCompactWidth = 72;

        QString pad(int indent) { return QString(indent * 4, QLatin1Char(' ')); }

        QString fromView(bsoncxx::stdx::string_view s)
        {
            return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
        }

        // Double-quoted string literal valid in Python, JavaScript, Java and C# (non-verbatim).
        QString quoted(const QString &s)
        {
            QString out;
            out.reserve(s.size() + 2);
            out += QLatin1Char('"');
            for (const QChar c : s) {
                switch (c.unicode()) {
                case '"':  out += QLatin1String("\\\""); break;
                case '\\': out += QLatin1String("\\\\"); break;
                case '\n': out += QLatin1String("\\n"); break;
                case '\r': out += QLatin1String("\\r"); break;
                case '\t': out += QLatin1String("\\t"); break;
                default:
                    if (c.unicode() < 0x20)
                        out += QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QLatin1Char('0'));
                    else
                        out += c;
                }
            }
            out += QLatin1Char('"');
            return out;
        }

        QString shortestDouble(double d)
        {
            char buf[64];
            const auto res = std::to_chars(buf, buf + sizeof(buf), d);
            return QString::fromLatin1(buf, static_cast<int>(res.ptr - buf));
        }

        QString isoDate(qint64 ms)
        {
            return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
        }

        QString hex(const uint8_t *bytes, uint32_t size)
        {
            return QString::fromLatin1(
                QByteArray(reinterpret_cast<const char *>(bytes), static_cast<int>(size)).toHex());
        }

        /**
         * Renders BSON as a Python or JavaScript literal, collecting the
         * driver types it needs for the import line.
         */
        class LiteralWriter
        {
        public:
            explicit LiteralWriter(Language lang) : _python(lang == Language::Python) {}

            std::set<QString> imports;

            QString document(bsoncxx::document::view d, int indent)
            {
                std::vector<std::pair<QString, QString>> items;   // key, value (compact)
                for (const auto &el : d)
                    items.emplace_back(key(fromView(el.key())), element(el, indent + 1));
                return container(items, indent, QLatin1Char('{'), QLatin1Char('}'));
            }

            QString array(bsoncxx::array::view a, int indent)
            {
                std::vector<std::pair<QString, QString>> items;
                for (const auto &el : a)
                    items.emplace_back(QString(), element(el, indent + 1));
                return container(items, indent, QLatin1Char('['), QLatin1Char(']'));
            }

            template <typename Element>
            QString element(const Element &el, int indent)
            {
                switch (el.type()) {
                case bsoncxx::type::k_document: return document(el.get_document().value, indent);
                case bsoncxx::type::k_array:    return array(el.get_array().value, indent);
                case bsoncxx::type::k_string:   return quoted(fromView(el.get_string().value));
                case bsoncxx::type::k_int32:    return QString::number(el.get_int32().value);
                case bsoncxx::type::k_int64: {
                    const QString n = QString::number(el.get_int64().value);
                    return _python ? use("Int64", QStringLiteral("Int64(%1)").arg(n))
                                   : use("Long", QStringLiteral("Long.fromString(\"%1\")").arg(n));
                }
                case bsoncxx::type::k_double:   return number(el.get_double().value);
                case bsoncxx::type::k_bool:
                    return el.get_bool().value ? (_python ? QStringLiteral("True") : QStringLiteral("true"))
                                               : (_python ? QStringLiteral("False") : QStringLiteral("false"));
                case bsoncxx::type::k_null:     return _python ? QStringLiteral("None") : QStringLiteral("null");
                case bsoncxx::type::k_oid: {
                    const QString id = quoted(QString::fromStdString(el.get_oid().value.to_string()));
                    return use("ObjectId", (_python ? QStringLiteral("ObjectId(%1)") : QStringLiteral("new ObjectId(%1)")).arg(id));
                }
                case bsoncxx::type::k_date: {
                    const QString iso = isoDate(el.get_date().to_int64());
                    if (_python) {
                        imports.insert(QStringLiteral("datetime"));
                        return QStringLiteral("datetime.fromisoformat(%1)")
                            .arg(quoted(QString(iso).replace(QLatin1Char('Z'), QLatin1String("+00:00"))));
                    }
                    return QStringLiteral("new Date(%1)").arg(quoted(iso));
                }
                case bsoncxx::type::k_decimal128: {
                    const QString v = quoted(QString::fromStdString(el.get_decimal128().value.to_string()));
                    return use("Decimal128", (_python ? QStringLiteral("Decimal128(%1)")
                                                      : QStringLiteral("Decimal128.fromString(%1)")).arg(v));
                }
                case bsoncxx::type::k_regex: {
                    const auto re = el.get_regex();
                    const QString args = quoted(fromView(re.regex)) + QLatin1String(", ") + quoted(fromView(re.options));
                    return _python ? use("Regex", QStringLiteral("Regex(%1)").arg(args))
                                   : use("BSONRegExp", QStringLiteral("new BSONRegExp(%1)").arg(args));
                }
                case bsoncxx::type::k_binary:   return binary(el.get_binary());
                case bsoncxx::type::k_timestamp: {
                    const auto ts = el.get_timestamp();
                    return _python ? use("Timestamp", QStringLiteral("Timestamp(%1, %2)").arg(ts.timestamp).arg(ts.increment))
                                   : use("Timestamp", QStringLiteral("new Timestamp({ t: %1, i: %2 })").arg(ts.timestamp).arg(ts.increment));
                }
                case bsoncxx::type::k_minkey:   return use("MinKey", _python ? QStringLiteral("MinKey()") : QStringLiteral("new MinKey()"));
                case bsoncxx::type::k_maxkey:   return use("MaxKey", _python ? QStringLiteral("MaxKey()") : QStringLiteral("new MaxKey()"));
                case bsoncxx::type::k_code: {
                    const QString code = quoted(fromView(el.get_code().code));
                    return use("Code", (_python ? QStringLiteral("Code(%1)") : QStringLiteral("new Code(%1)")).arg(code));
                }
                default:
                    // undefined, symbol, dbPointer, codeWScope: no faithful literal.
                    return _python ? QStringLiteral("None") : QStringLiteral("null");
                }
            }

        private:
            QString use(const char *type, const QString &literal)
            {
                imports.insert(QString::fromLatin1(type));
                return literal;
            }

            QString key(const QString &k) const
            {
                static const QRegularExpression jsIdent(QStringLiteral("^[A-Za-z_$][A-Za-z0-9_$]*$"));
                if (!_python && jsIdent.match(k).hasMatch())
                    return k;
                return quoted(k);
            }

            QString number(double d) const
            {
                if (std::isnan(d))
                    return _python ? QStringLiteral("float(\"nan\")") : QStringLiteral("NaN");
                if (std::isinf(d)) {
                    if (_python)
                        return d > 0 ? QStringLiteral("float(\"inf\")") : QStringLiteral("float(\"-inf\")");
                    return d > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
                }
                QString s = shortestDouble(d);
                // Keep Python floats floats (5.0, not the int 5).
                if (_python && !s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e')))
                    s += QLatin1String(".0");
                return s;
            }

            QString binary(const bsoncxx::types::b_binary &b)
            {
                if (b.sub_type == bsoncxx::binary_sub_type::k_uuid && b.size == 16) {
                    QString h = hex(b.bytes, b.size);
                    const QString uuid = quoted(h.left(8) + '-' + h.mid(8, 4) + '-' + h.mid(12, 4) + '-' +
                                                h.mid(16, 4) + '-' + h.mid(20));
                    if (_python) {
                        imports.insert(QStringLiteral("Binary"));
                        imports.insert(QStringLiteral("UUID"));
                        return QStringLiteral("Binary.from_uuid(UUID(%1))").arg(uuid);
                    }
                    return use("UUID", QStringLiteral("new UUID(%1)").arg(uuid));
                }
                const int subtype = static_cast<int>(b.sub_type);
                const QString data = quoted(hex(b.bytes, b.size));
                return use("Binary", (_python ? QStringLiteral("Binary(bytes.fromhex(%1), %2)")
                                              : QStringLiteral("new Binary(Buffer.from(%1, \"hex\"), %2)"))
                                         .arg(data).arg(subtype));
            }

            // One line when short, otherwise one entry per line.
            QString container(const std::vector<std::pair<QString, QString>> &items, int indent,
                              QChar open, QChar close) const
            {
                if (items.empty())
                    return QString(open) + close;
                const QString sep = _python ? QStringLiteral(": ") : QStringLiteral(": ");
                QStringList parts;
                bool multiline = false;
                for (const auto &[k, v] : items) {
                    parts << (k.isEmpty() ? v : k + sep + v);
                    multiline = multiline || v.contains(QLatin1Char('\n'));
                }
                const QString inner = parts.join(QStringLiteral(", "));
                const bool spaced = !_python && open == QLatin1Char('{');
                if (!multiline && inner.size() + indent * 4 <= kCompactWidth)
                    return spaced ? QString(open) + ' ' + inner + ' ' + close : QString(open) + inner + close;
                QString out = QString(open) + QLatin1Char('\n');
                for (const QString &p : parts)
                    out += pad(indent + 1) + p + QLatin1String(",\n");
                return out + pad(indent) + close;
            }

            bool _python;
        };

        // ---- Parsed query --------------------------------------------------

        struct Query
        {
            bool aggregate = false;
            QString collection;
            bsoncxx::document::value filter = bsoncxx::from_json("{}");
            std::optional<bsoncxx::document::value> projection;
            std::optional<bsoncxx::document::value> sort;
            int skip = 0;
            int limit = 0;
            std::vector<bsoncxx::document::value> pipeline;
        };

        bool parseDoc(const QString &text, std::optional<bsoncxx::document::value> &out,
                      const char *what, QString &error)
        {
            if (text.trimmed().isEmpty())
                return true;
            bsoncxx::document::value doc = bsoncxx::from_json("{}");
            QString err;
            if (!MongoJsonUtils::parseSingleDocument(text, doc, err)) {
                error = QCoreApplication::translate("QueryCodeExport", "Cannot parse the %1: %2").arg(QLatin1String(what), err);
                return false;
            }
            out = std::move(doc);
            return true;
        }

        bool parseQuery(const QString &script, Query &q, QString &error)
        {
            const auto find = MongoJsonUtils::parseFindQueryChain(script);
            if (find.valid) {
                q.collection = find.collection;
                std::optional<bsoncxx::document::value> filter;
                if (!parseDoc(find.filter, filter, "filter", error) ||
                    !parseDoc(find.projection, q.projection, "projection", error) ||
                    !parseDoc(find.sort, q.sort, "sort", error))
                    return false;
                if (filter)
                    q.filter = std::move(*filter);
                q.skip = find.skip;
                q.limit = find.limit;
                return true;
            }
            const auto agg = MongoJsonUtils::parseAggregateChain(script);
            if (agg.valid) {
                q.aggregate = true;
                q.collection = agg.collection;
                QString err;
                if (!MongoJsonUtils::parseDocuments(agg.pipeline, q.pipeline, err)) {
                    error = QCoreApplication::translate("QueryCodeExport", "Cannot parse the pipeline: %1").arg(err);
                    return false;
                }
                return true;
            }
            error = QCoreApplication::translate("QueryCodeExport", "Only a single find(...) chain or aggregate([...]) statement can be exported.");
            return false;
        }

        // ---- Generators ----------------------------------------------------

        QString pythonCode(const Query &q, const QString &db, const QString &uri)
        {
            LiteralWriter w(Language::Python);
            QString call;
            if (q.aggregate) {
                QStringList stages;
                for (const auto &s : q.pipeline)
                    stages << pad(1) + w.document(s.view(), 1) + QLatin1Char(',');
                call = QStringLiteral("cursor = collection.aggregate([\n%1\n])").arg(stages.join('\n'));
            } else {
                QStringList args{pad(1) + w.document(q.filter.view(), 1)};
                if (q.projection)
                    args << pad(1) + QStringLiteral("projection=") + w.document(q.projection->view(), 1);
                if (q.sort) {
                    // PyMongo takes the sort as a list of (key, direction) pairs.
                    QStringList pairs;
                    for (const auto &el : q.sort->view())
                        pairs << QStringLiteral("(%1, %2)").arg(quoted(fromView(el.key())), w.element(el, 1));
                    args << pad(1) + QStringLiteral("sort=[%1]").arg(pairs.join(QStringLiteral(", ")));
                }
                if (q.skip > 0)
                    args << pad(1) + QStringLiteral("skip=%1").arg(q.skip);
                if (q.limit > 0)
                    args << pad(1) + QStringLiteral("limit=%1").arg(q.limit);
                call = QStringLiteral("cursor = collection.find(\n%1,\n)").arg(args.join(QStringLiteral(",\n")));
            }

            QStringList header{QStringLiteral("from pymongo import MongoClient")};
            QStringList bsonTypes;
            for (const QString &t : w.imports) {
                if (t == QLatin1String("datetime"))
                    header << QStringLiteral("from datetime import datetime");
                else if (t == QLatin1String("UUID"))
                    header << QStringLiteral("from uuid import UUID");
                else
                    bsonTypes << t;
            }
            if (!bsonTypes.isEmpty())
                header << QStringLiteral("from bson import %1").arg(bsonTypes.join(QStringLiteral(", ")));

            return header.join('\n') + QStringLiteral(
                "\n\nclient = MongoClient(%1)\n"
                "collection = client[%2][%3]\n\n"
                "%4\n"
                "for doc in cursor:\n"
                "    print(doc)\n")
                .arg(quoted(uri), quoted(db), quoted(q.collection), call);
        }

        QString nodeCode(const Query &q, const QString &db, const QString &uri)
        {
            LiteralWriter w(Language::NodeJs);
            QString call;
            if (q.aggregate) {
                QStringList stages;
                for (const auto &s : q.pipeline)
                    stages << pad(3) + w.document(s.view(), 3) + QLatin1Char(',');
                call = QStringLiteral("const cursor = collection.aggregate([\n%1\n%2]);")
                           .arg(stages.join('\n'), pad(2));
            } else {
                QStringList opts;
                if (q.projection)
                    opts << pad(3) + QStringLiteral("projection: ") + w.document(q.projection->view(), 3);
                if (q.sort)
                    opts << pad(3) + QStringLiteral("sort: ") + w.document(q.sort->view(), 3);
                if (q.skip > 0)
                    opts << pad(3) + QStringLiteral("skip: %1").arg(q.skip);
                if (q.limit > 0)
                    opts << pad(3) + QStringLiteral("limit: %1").arg(q.limit);
                const QString filter = w.document(q.filter.view(), 2);
                call = opts.isEmpty()
                    ? QStringLiteral("const cursor = collection.find(%1);").arg(filter)
                    : QStringLiteral("const cursor = collection.find(%1, {\n%2,\n%3});")
                          .arg(filter, opts.join(QStringLiteral(",\n")), pad(2));
            }

            QStringList names{QStringLiteral("MongoClient")};
            for (const QString &t : w.imports)
                names << t;
            return QStringLiteral(
                "const { %1 } = require(\"mongodb\");\n\n"
                "async function main() {\n"
                "    const client = new MongoClient(%2);\n"
                "    try {\n"
                "        const collection = client.db(%3).collection(%4);\n"
                "        %5\n"
                "        for await (const doc of cursor) {\n"
                "            console.log(doc);\n"
                "        }\n"
                "    } finally {\n"
                "        await client.close();\n"
                "    }\n"
                "}\n\n"
                "main().catch(console.error);\n")
                .arg(names.join(QStringLiteral(", ")), quoted(uri), quoted(db), quoted(q.collection), call);
        }

        QString relaxedJson(bsoncxx::document::view d)
        {
            return QString::fromStdString(bsoncxx::to_json(d, bsoncxx::ExtendedJsonMode::k_relaxed));
        }

        QString javaCode(const Query &q, const QString &db, const QString &uri)
        {
            const auto parse = [](bsoncxx::document::view d) {
                return QStringLiteral("Document.parse(%1)").arg(quoted(relaxedJson(d)));
            };
            QString call;
            QStringList imports{QStringLiteral("com.mongodb.client.MongoClient"),
                                QStringLiteral("com.mongodb.client.MongoClients"),
                                QStringLiteral("com.mongodb.client.MongoCollection"),
                                QStringLiteral("org.bson.Document")};
            if (q.aggregate) {
                imports << QStringLiteral("com.mongodb.client.AggregateIterable") << QStringLiteral("java.util.Arrays");
                QStringList stages;
                for (const auto &s : q.pipeline)
                    stages << pad(5) + parse(s.view());
                call = QStringLiteral("AggregateIterable<Document> result = collection.aggregate(Arrays.asList(\n%1));")
                           .arg(stages.join(QStringLiteral(",\n")));
            } else {
                imports << QStringLiteral("com.mongodb.client.FindIterable");
                call = QStringLiteral("FindIterable<Document> result = collection.find(%1)").arg(parse(q.filter.view()));
                if (q.projection)
                    call += QLatin1Char('\n') + pad(5) + QStringLiteral(".projection(%1)").arg(parse(q.projection->view()));
                if (q.sort)
                    call += QLatin1Char('\n') + pad(5) + QStringLiteral(".sort(%1)").arg(parse(q.sort->view()));
                if (q.skip > 0)
                    call += QLatin1Char('\n') + pad(5) + QStringLiteral(".skip(%1)").arg(q.skip);
                if (q.limit > 0)
                    call += QLatin1Char('\n') + pad(5) + QStringLiteral(".limit(%1)").arg(q.limit);
                call += QLatin1Char(';');
            }
            imports.sort();
            QString importLines;
            for (const QString &i : imports)
                importLines += QStringLiteral("import %1;\n").arg(i);

            return importLines + QStringLiteral(
                "\npublic class QueryExport {\n"
                "    public static void main(String[] args) {\n"
                "        try (MongoClient client = MongoClients.create(%1)) {\n"
                "            MongoCollection<Document> collection = client.getDatabase(%2).getCollection(%3);\n"
                "            %4\n"
                "            for (Document doc : result) {\n"
                "                System.out.println(doc.toJson());\n"
                "            }\n"
                "        }\n"
                "    }\n"
                "}\n")
                .arg(quoted(uri), quoted(db), quoted(q.collection), call);
        }

        QString csharpCode(const Query &q, const QString &db, const QString &uri)
        {
            const auto parse = [](bsoncxx::document::view d) {
                // Verbatim string: only quotes need doubling.
                return QStringLiteral("BsonDocument.Parse(@\"%1\")").arg(relaxedJson(d).replace('"', QLatin1String("\"\"")));
            };
            QString call;
            if (q.aggregate) {
                QStringList stages;
                for (const auto &s : q.pipeline)
                    stages << pad(1) + parse(s.view()) + QLatin1Char(',');
                call = QStringLiteral("var pipeline = new[]\n{\n%1\n};\n"
                                      "var results = collection.Aggregate<BsonDocument>(pipeline).ToList();")
                           .arg(stages.join('\n'));
            } else {
                call = QStringLiteral("var results = collection.Find(%1)").arg(parse(q.filter.view()));
                if (q.projection)
                    call += QLatin1Char('\n') + pad(1) + QStringLiteral(".Project(%1)").arg(parse(q.projection->view()));
                if (q.sort)
                    call += QLatin1Char('\n') + pad(1) + QStringLiteral(".Sort(%1)").arg(parse(q.sort->view()));
                if (q.skip > 0)
                    call += QLatin1Char('\n') + pad(1) + QStringLiteral(".Skip(%1)").arg(q.skip);
                if (q.limit > 0)
                    call += QLatin1Char('\n') + pad(1) + QStringLiteral(".Limit(%1)").arg(q.limit);
                call += QLatin1Char('\n') + pad(1) + QStringLiteral(".ToList();");
            }
            return QStringLiteral(
                "using MongoDB.Bson;\n"
                "using MongoDB.Driver;\n\n"
                "var client = new MongoClient(%1);\n"
                "var collection = client.GetDatabase(%2).GetCollection<BsonDocument>(%3);\n\n"
                "%4\n\n"
                "foreach (var doc in results)\n"
                "{\n"
                "    Console.WriteLine(doc);\n"
                "}\n")
                .arg(quoted(uri), quoted(db), quoted(q.collection), call);
        }
    }

    QStringList QueryCodeExport::languageNames()
    {
        return {QStringLiteral("Python"), QStringLiteral("Node.js"), QStringLiteral("Java"), QStringLiteral("C#")};
    }

    QueryCodeExport::Result QueryCodeExport::exportScript(const QString &script, const QString &database,
                                                          const QString &connectionUri, Language language)
    {
        Result result;
        Query q;
        if (!parseQuery(script, q, result.error))
            return result;
        const QString uri = connectionUri.isEmpty() ? QStringLiteral("mongodb://localhost:27017") : connectionUri;
        switch (language) {
        case Language::Python: result.code = pythonCode(q, database, uri); break;
        case Language::NodeJs: result.code = nodeCode(q, database, uri); break;
        case Language::Java:   result.code = javaCode(q, database, uri); break;
        case Language::CSharp: result.code = csharpCode(q, database, uri); break;
        }
        result.ok = true;
        return result;
    }
}
