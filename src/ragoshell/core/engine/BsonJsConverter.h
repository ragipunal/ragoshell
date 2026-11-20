#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include <QJSEngine>
#include <QJSValue>

#include <bsoncxx/array/view.hpp>
#include <bsoncxx/builder/core.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types/bson_value/value.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

namespace RagoShell
{
    /// A JavaScript value that cannot be stored in BSON (docs/Shell.md).
    class BsonConversionError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// BSON <-> QJSEngine values, in both directions, without losing a type.
    ///
    /// JavaScript has no int64, decimal128, ObjectId and so on, so the engine gets small classes
    /// for them (`__bson.ObjectId`, `__bson.Long`, ..., see bsonTypesScript()). Each carries its exact
    /// value (`Long.value` is the decimal string) and a `_bsontype` tag the converter recognises.
    /// Everything else maps to the JavaScript type of the same name:
    ///
    ///   BSON                      JavaScript
    ///   double, int32             number          (a number written back is int32 when it is an
    ///                                              integer in range, else double)
    ///   int64                     __bson.Long     (never a number; valueOf() gives one for arithmetic)
    ///   string, bool, null        string, boolean, null   (undefined is written as null)
    ///   date                      Date
    ///   regex                     RegExp          (__bson.BsonRegExp when JavaScript cannot hold it)
    ///   document, array           object, Array   (field order is kept)
    ///   everything else           the __bson class of the type
    ///
    /// All calls must be made on the thread of the engine.
    class BsonJsConverter
    {
    public:
        /// Installs the `__bson` namespace into the engine (once per engine).
        explicit BsonJsConverter(QJSEngine *engine);

        QJSEngine *engine() const { return _engine; }

        QJSValue toJs(bsoncxx::document::view doc) const;
        QJSValue toJs(bsoncxx::array::view array) const;
        QJSValue toJs(const bsoncxx::types::bson_value::view &value) const;

        /// Throws BsonConversionError.
        bsoncxx::document::value toDocument(const QJSValue &object) const;
        /// A JavaScript array of objects, e.g. an aggregation pipeline. Throws BsonConversionError.
        std::vector<bsoncxx::document::value> toDocuments(const QJSValue &array) const;
        bsoncxx::types::bson_value::value toValue(const QJSValue &value) const;

        /// Extended JSON text of a document ("relaxed" or "canonical").
        static std::string toJson(bsoncxx::document::view doc, bool relaxed);
        /// The JavaScript that defines the BSON classes; evaluates to the `__bson` object.
        static QString bsonTypesScript();

    private:
        void appendProperties(bsoncxx::builder::core &builder, const QJSValue &object, int depth) const;
        void appendValue(bsoncxx::builder::core &builder, const QJSValue &value, int depth) const;
        void appendWrapper(bsoncxx::builder::core &builder, const QJSValue &value, const QString &tag,
                           int depth) const;
        QJSValue make(const QJSValue &constructor, const QJSValueList &args) const;

        QJSEngine *_engine;
        QJSValue _types;
        QJSValue _objectId, _long, _decimal128, _timestamp, _binary, _regExp, _minKey, _maxKey,
                 _code, _symbol, _dbPointer;
        QJSValue _dateCtor, _regExpCtor;
    };
}
