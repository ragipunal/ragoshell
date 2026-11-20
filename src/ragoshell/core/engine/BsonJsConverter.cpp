#include "ragoshell/core/engine/BsonJsConverter.h"

#include <cmath>
#include <cstdint>

#include <QByteArray>
#include <QDateTime>
#include <QJSValueIterator>

#include <bsoncxx/decimal128.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

namespace RagoShell
{
    namespace
    {
        // BSON allows 100 levels of nesting; a JavaScript object that refers to itself ends here too.
        constexpr int kMaxDepth = 100;

        using bsoncxx::stdx::string_view;

        string_view view(const QByteArray &bytes)
        {
            return string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        }

        QString text(string_view s)
        {
            return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
        }

        // The classes of `__bson`. shell-runtime.js builds the shell's constructors
        // (ObjectId(), NumberLong(), ...) on them.
        const char *const kBsonTypesScript = R"JS(
(function () {
  'use strict';
  const tag = (C, name) => Object.defineProperty(C.prototype, '_bsontype', { value: name });
  const hex24 = /^[0-9a-fA-F]{24}$/;
  const integerText = /^[+-]?[0-9]+$/;
  const isTagged = (v, name) => v !== null && typeof v === 'object' && v._bsontype === name;

  class ObjectId {
    constructor(id) {
      if (id === undefined) id = __host.newObjectId();
      else if (isTagged(id, 'ObjectId')) id = id.hex;
      if (typeof id !== 'string' || !hex24.test(id))
        throw new Error('ObjectId needs 24 hex characters, got: ' + String(id));
      this.hex = id.toLowerCase();
    }
    toString() { return this.hex; }
    toHexString() { return this.hex; }
    equals(other) { return isTagged(other, 'ObjectId') && other.hex === this.hex; }
    getTimestamp() { return new Date(parseInt(this.hex.substring(0, 8), 16) * 1000); }
  }
  tag(ObjectId, 'ObjectId');

  // 64-bit integers are never JavaScript numbers: the exact value is the decimal text.
  class Long {
    constructor(v) {
      if (v === undefined) v = 0;
      if (isTagged(v, 'Long')) v = v.value;
      if (typeof v === 'number') {
        if (!Number.isSafeInteger(v))
          throw new Error('NumberLong from a number that is not a safe integer loses digits; pass a string: ' + v);
        v = String(v);
      }
      if (typeof v !== 'string' || !integerText.test(v))
        throw new Error('NumberLong needs an integer or its text, got: ' + String(v));
      v = v.replace(/^\+/, '').replace(/^(-?)0+(?=[0-9])/, '$1');
      this.value = v === '-0' ? '0' : v;
    }
    valueOf() { return Number(this.value); }
    toString() { return this.value; }
  }
  tag(Long, 'Long');

  class Decimal128 {
    constructor(v) { this.value = String(v === undefined ? '0' : v).trim(); }
    toString() { return this.value; }
  }
  tag(Decimal128, 'Decimal128');

  class Timestamp {
    constructor(t, i) {
      if (t !== null && typeof t === 'object') { i = t.i; t = t.t; }
      this.t = (t || 0) >>> 0;
      this.i = (i || 0) >>> 0;
    }
  }
  tag(Timestamp, 'Timestamp');

  // Binary data as base64 text.
  class Binary {
    constructor(subtype, base64) {
      this.subtype = (subtype === undefined ? 0 : subtype) | 0;
      this.base64 = String(base64 === undefined ? '' : base64);
    }
  }
  tag(Binary, 'Binary');

  // A BSON regular expression JavaScript's RegExp cannot hold (options x and l, PCRE-only syntax).
  class BsonRegExp {
    constructor(pattern, options) {
      this.pattern = String(pattern);
      this.options = String(options === undefined ? '' : options);
    }
  }
  tag(BsonRegExp, 'BsonRegExp');

  class MinKey {}
  tag(MinKey, 'MinKey');
  class MaxKey {}
  tag(MaxKey, 'MaxKey');

  class Code {
    constructor(code, scope) {
      this.code = String(code);
      this.scope = scope === undefined ? null : scope;
    }
  }
  tag(Code, 'Code');

  class BsonSymbol {
    constructor(value) { this.value = String(value); }
    toString() { return this.value; }
  }
  tag(BsonSymbol, 'BsonSymbol');

  class DBPointer {
    constructor(ns, id) { this.ns = String(ns); this.id = id; }
  }
  tag(DBPointer, 'DBPointer');

  // Explicit int32 / double, for a number whose BSON type must not depend on its value.
  class Int32 {
    constructor(v) { this.value = (v === undefined ? 0 : Number(v)) | 0; }
    valueOf() { return this.value; }
    toString() { return String(this.value); }
  }
  tag(Int32, 'Int32');
  class Double {
    constructor(v) { this.value = v === undefined ? 0 : Number(v); }
    valueOf() { return this.value; }
    toString() { return String(this.value); }
  }
  tag(Double, 'Double');

  return { ObjectId, Long, Decimal128, Timestamp, Binary, BsonRegExp, MinKey, MaxKey, Code,
           BsonSymbol, DBPointer, Int32, Double };
})()
)JS";

        [[noreturn]] void fail(const QString &message)
        {
            throw BsonConversionError(message.toStdString());
        }

        // The options of a BSON regex that a JavaScript RegExp has a flag for.
        bool jsFlagsFor(string_view options, QString *flags)
        {
            flags->clear();
            for (char c : options) {
                if (c != 'i' && c != 'm' && c != 's' && c != 'u')
                    return false;
                flags->append(QLatin1Char(c));
            }
            return true;
        }
    }

    QString BsonJsConverter::bsonTypesScript()
    {
        return QString::fromLatin1(kBsonTypesScript);
    }

    std::string BsonJsConverter::toJson(bsoncxx::document::view doc, bool relaxed)
    {
        return bsoncxx::to_json(doc, relaxed ? bsoncxx::ExtendedJsonMode::k_relaxed
                                             : bsoncxx::ExtendedJsonMode::k_canonical);
    }

    BsonJsConverter::BsonJsConverter(QJSEngine *engine) :
        _engine(engine)
    {
        QJSValue global = engine->globalObject();
        QJSValue types = global.property(QStringLiteral("__bson"));
        if (!types.isObject()) {
            types = engine->evaluate(bsonTypesScript(), QStringLiteral("bson-types.js"));
            if (types.isError())
                throw std::runtime_error("bson-types.js: " + types.toString().toStdString());
            global.setProperty(QStringLiteral("__bson"), types);
        }
        _types = types;
        _objectId = types.property(QStringLiteral("ObjectId"));
        _long = types.property(QStringLiteral("Long"));
        _decimal128 = types.property(QStringLiteral("Decimal128"));
        _timestamp = types.property(QStringLiteral("Timestamp"));
        _binary = types.property(QStringLiteral("Binary"));
        _regExp = types.property(QStringLiteral("BsonRegExp"));
        _minKey = types.property(QStringLiteral("MinKey"));
        _maxKey = types.property(QStringLiteral("MaxKey"));
        _code = types.property(QStringLiteral("Code"));
        _symbol = types.property(QStringLiteral("BsonSymbol"));
        _dbPointer = types.property(QStringLiteral("DBPointer"));
        _dateCtor = global.property(QStringLiteral("Date"));
        _regExpCtor = global.property(QStringLiteral("RegExp"));
    }

    QJSValue BsonJsConverter::make(const QJSValue &constructor, const QJSValueList &args) const
    {
        QJSValue result = constructor.callAsConstructor(args);
        if (result.isError())
            fail(QStringLiteral("cannot create a BSON value in JavaScript: %1").arg(result.toString()));
        return result;
    }

    // ---- BSON -> JavaScript -------------------------------------------------------------------

    QJSValue BsonJsConverter::toJs(bsoncxx::document::view doc) const
    {
        QJSValue object = _engine->newObject();
        for (const auto &element : doc)
            object.setProperty(text(element.key()), toJs(element.get_value()));
        return object;
    }

    QJSValue BsonJsConverter::toJs(bsoncxx::array::view array) const
    {
        QJSValue result = _engine->newArray();
        quint32 index = 0;
        for (const auto &element : array)
            result.setProperty(index++, toJs(element.get_value()));
        return result;
    }

    QJSValue BsonJsConverter::toJs(const bsoncxx::types::bson_value::view &value) const
    {
        using bsoncxx::type;
        switch (value.type()) {
        case type::k_double:
            return QJSValue(value.get_double().value);
        case type::k_string:
            return QJSValue(text(value.get_string().value));
        case type::k_document:
            return toJs(value.get_document().value);
        case type::k_array:
            return toJs(value.get_array().value);
        case type::k_binary: {
            const auto &b = value.get_binary();
            const QByteArray base64 = QByteArray(reinterpret_cast<const char *>(b.bytes), static_cast<qsizetype>(b.size))
                                          .toBase64();
            return make(_binary, { QJSValue(static_cast<int>(b.sub_type)), QJSValue(QString::fromLatin1(base64)) });
        }
        case type::k_undefined:
            return QJSValue(QJSValue::UndefinedValue);
        case type::k_oid:
            return make(_objectId, { QJSValue(QString::fromStdString(value.get_oid().value.to_string())) });
        case type::k_bool:
            return QJSValue(value.get_bool().value);
        case type::k_date:
            return make(_dateCtor, { QJSValue(static_cast<double>(value.get_date().value.count())) });
        case type::k_null:
            return QJSValue(QJSValue::NullValue);
        case type::k_regex: {
            const auto &r = value.get_regex();
            QString flags;
            if (jsFlagsFor(r.options, &flags)) {
                QJSValue native = _regExpCtor.callAsConstructor({ QJSValue(text(r.regex)), QJSValue(flags) });
                if (!native.isError())
                    return native;
            }
            return make(_regExp, { QJSValue(text(r.regex)), QJSValue(text(r.options)) });
        }
        case type::k_dbpointer: {
            const auto &p = value.get_dbpointer();
            QJSValue id = make(_objectId, { QJSValue(QString::fromStdString(p.value.to_string())) });
            return make(_dbPointer, { QJSValue(text(p.collection)), id });
        }
        case type::k_code:
            return make(_code, { QJSValue(text(value.get_code().code)) });
        case type::k_symbol:
            return make(_symbol, { QJSValue(text(value.get_symbol().symbol)) });
        case type::k_codewscope: {
            const auto &c = value.get_codewscope();
            return make(_code, { QJSValue(text(c.code)), toJs(c.scope) });
        }
        case type::k_int32:
            return QJSValue(static_cast<int>(value.get_int32().value));
        case type::k_timestamp: {
            const auto &t = value.get_timestamp();
            return make(_timestamp, { QJSValue(static_cast<double>(t.timestamp)),
                                      QJSValue(static_cast<double>(t.increment)) });
        }
        case type::k_int64:
            return make(_long, { QJSValue(QString::number(static_cast<qlonglong>(value.get_int64().value))) });
        case type::k_decimal128:
            return make(_decimal128, { QJSValue(QString::fromStdString(value.get_decimal128().value.to_string())) });
        case type::k_minkey:
            return make(_minKey, {});
        case type::k_maxkey:
            return make(_maxKey, {});
        }
        fail(QStringLiteral("unknown BSON type %1").arg(static_cast<int>(value.type())));
    }

    // ---- JavaScript -> BSON -------------------------------------------------------------------

    bsoncxx::document::value BsonJsConverter::toDocument(const QJSValue &object) const
    {
        if (!object.isObject() || object.isArray() || object.isCallable() || object.isDate() || object.isRegExp()
            || object.property(QStringLiteral("_bsontype")).isString())
            fail(QStringLiteral("a document must be a plain object, got: %1").arg(object.toString()));
        bsoncxx::builder::core builder(false);
        appendProperties(builder, object, 0);
        return builder.extract_document();
    }

    std::vector<bsoncxx::document::value> BsonJsConverter::toDocuments(const QJSValue &array) const
    {
        if (!array.isArray())
            fail(QStringLiteral("an array is required, got: %1").arg(array.toString()));
        std::vector<bsoncxx::document::value> documents;
        const quint32 length = array.property(QStringLiteral("length")).toUInt();
        for (quint32 i = 0; i < length; ++i)
            documents.push_back(toDocument(array.property(i)));
        return documents;
    }

    bsoncxx::types::bson_value::value BsonJsConverter::toValue(const QJSValue &value) const
    {
        bsoncxx::builder::core builder(false);
        builder.key_view("v");
        appendValue(builder, value, 0);
        bsoncxx::document::value holder = builder.extract_document();
        return bsoncxx::types::bson_value::value(holder.view()["v"].get_value());
    }

    void BsonJsConverter::appendProperties(bsoncxx::builder::core &builder, const QJSValue &object, int depth) const
    {
        QJSValueIterator it(object);
        while (it.hasNext()) {
            it.next();
            const QJSValue value = it.value();
            if (value.isCallable())
                continue; // functions are not data; the Node driver leaves them out as well
            const QString name = it.name();
            if (name.contains(QChar(0)))
                fail(QStringLiteral("a field name cannot contain a null character: %1").arg(name));
            builder.key_owned(name.toStdString());
            appendValue(builder, value, depth);
        }
    }

    void BsonJsConverter::appendValue(bsoncxx::builder::core &builder, const QJSValue &value, int depth) const
    {
        using namespace bsoncxx::types;

        if (depth > kMaxDepth)
            fail(QStringLiteral("the document is nested more than %1 levels deep, or refers to itself").arg(kMaxDepth));

        if (value.isUndefined() || value.isNull()) {
            builder.append(b_null{});
        } else if (value.isBool()) {
            builder.append(b_bool{ value.toBool() });
        } else if (value.isNumber()) {
            const double d = value.toNumber();
            if (std::isfinite(d) && d == std::floor(d) && d >= -2147483648.0 && d <= 2147483647.0
                && !(d == 0 && std::signbit(d)))
                builder.append(b_int32{ static_cast<std::int32_t>(d) });
            else
                builder.append(b_double{ d });
        } else if (value.isString()) {
            const QByteArray utf8 = value.toString().toUtf8();
            builder.append(b_string{ view(utf8) });
        } else if (value.isDate()) {
            const QDateTime when = value.toDateTime();
            if (!when.isValid())
                fail(QStringLiteral("Invalid Date"));
            builder.append(b_date{ std::chrono::milliseconds(when.toMSecsSinceEpoch()) });
        } else if (value.isRegExp()) {
            const QString source = value.property(QStringLiteral("source")).toString();
            const QString flags = value.property(QStringLiteral("flags")).toString();
            QByteArray options;
            for (const QChar c : QStringLiteral("imsu")) {
                if (flags.contains(c))
                    options.append(c.toLatin1());
            }
            for (const QChar c : flags) {
                // g only repeats a search and means nothing to a query; the other flags have no BSON option.
                if (c != QLatin1Char('g') && !QStringLiteral("imsu").contains(c))
                    fail(QStringLiteral("the RegExp flag '%1' cannot be stored in BSON (use i, m, s or u)").arg(c));
            }
            const QByteArray pattern = source.toUtf8();
            builder.append(b_regex{ view(pattern), view(options) });
        } else if (value.isArray()) {
            const quint32 length = value.property(QStringLiteral("length")).toUInt();
            builder.open_array();
            for (quint32 i = 0; i < length; ++i) {
                const QJSValue item = value.property(i);
                if (item.isCallable())
                    builder.append(b_null{});
                else
                    appendValue(builder, item, depth + 1);
            }
            builder.close_array();
        } else if (value.isCallable()) {
            builder.append(b_null{});
        } else if (value.isObject()) {
            const QJSValue tag = value.property(QStringLiteral("_bsontype"));
            if (tag.isString()) {
                appendWrapper(builder, value, tag.toString(), depth);
            } else {
                builder.open_document();
                appendProperties(builder, value, depth + 1);
                builder.close_document();
            }
        } else {
            fail(QStringLiteral("cannot store this value in BSON: %1").arg(value.toString()));
        }
    }

    void BsonJsConverter::appendWrapper(bsoncxx::builder::core &builder, const QJSValue &value, const QString &tag,
                                        int depth) const
    {
        using namespace bsoncxx::types;
        auto prop = [&](const char *name) { return value.property(QString::fromLatin1(name)); };

        try {
            if (tag == QLatin1String("ObjectId")) {
                const QByteArray hex = prop("hex").toString().toLatin1();
                builder.append(b_oid{ bsoncxx::oid(view(hex)) });
            } else if (tag == QLatin1String("Long")) {
                bool ok = false;
                const QString digits = prop("value").toString();
                const qlonglong n = digits.toLongLong(&ok);
                if (!ok)
                    fail(QStringLiteral("NumberLong is outside the 64-bit range: %1").arg(digits));
                builder.append(b_int64{ static_cast<std::int64_t>(n) });
            } else if (tag == QLatin1String("Int32")) {
                const double d = prop("value").toNumber();
                if (!(d >= -2147483648.0 && d <= 2147483647.0) || d != std::floor(d))
                    fail(QStringLiteral("NumberInt needs an integer in the 32-bit range, got: %1").arg(d));
                builder.append(b_int32{ static_cast<std::int32_t>(d) });
            } else if (tag == QLatin1String("Double")) {
                builder.append(b_double{ prop("value").toNumber() });
            } else if (tag == QLatin1String("Decimal128")) {
                const QByteArray digits = prop("value").toString().toLatin1();
                builder.append(b_decimal128{ bsoncxx::decimal128(view(digits)) });
            } else if (tag == QLatin1String("Timestamp")) {
                builder.append(b_timestamp{ prop("i").toUInt(), prop("t").toUInt() });
            } else if (tag == QLatin1String("Binary")) {
                const int subtype = prop("subtype").toInt();
                if (subtype < 0 || subtype > 255)
                    fail(QStringLiteral("a binary subtype is a number from 0 to 255, got: %1").arg(subtype));
                const QByteArray bytes = QByteArray::fromBase64(prop("base64").toString().toLatin1());
                builder.append(b_binary{ static_cast<bsoncxx::binary_sub_type>(subtype),
                                         static_cast<std::uint32_t>(bytes.size()),
                                         reinterpret_cast<const std::uint8_t *>(bytes.constData()) });
            } else if (tag == QLatin1String("BsonRegExp")) {
                const QByteArray pattern = prop("pattern").toString().toUtf8();
                const QByteArray options = prop("options").toString().toLatin1();
                builder.append(b_regex{ view(pattern), view(options) });
            } else if (tag == QLatin1String("MinKey")) {
                builder.append(b_minkey{});
            } else if (tag == QLatin1String("MaxKey")) {
                builder.append(b_maxkey{});
            } else if (tag == QLatin1String("Code")) {
                const QByteArray code = prop("code").toString().toUtf8();
                const QJSValue scope = prop("scope");
                if (scope.isObject()) {
                    const bsoncxx::document::value scopeDoc = toDocument(scope);
                    builder.append(b_codewscope{ view(code), scopeDoc.view() });
                } else {
                    builder.append(b_code{ view(code) });
                }
            } else if (tag == QLatin1String("BsonSymbol")) {
                const QByteArray symbol = prop("value").toString().toUtf8();
                builder.append(b_symbol{ view(symbol) });
            } else if (tag == QLatin1String("DBPointer")) {
                const QByteArray ns = prop("ns").toString().toUtf8();
                const QByteArray hex = prop("id").property(QStringLiteral("hex")).toString().toLatin1();
                builder.append(b_dbpointer{ view(ns), bsoncxx::oid(view(hex)) });
            } else {
                fail(QStringLiteral("unknown BSON type tag '%1'").arg(tag));
            }
        } catch (const bsoncxx::exception &e) {
            fail(QStringLiteral("invalid %1 value: %2").arg(tag, QString::fromUtf8(e.what())));
        }
        (void)depth;
    }
}
