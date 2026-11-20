#include <gtest/gtest.h>

#include <vector>

#include "ragoshell/core/utils/FieldCompletion.h"

using namespace RagoShell;
using FieldCompletion::Kind;

namespace
{
    struct Case
    {
        const char *text;   ///< editor text up to the caret
        Kind kind;
        const char *collection;
        const char *prefix;
        int replaceLength;
    };

    std::string kindName(Kind k)
    {
        switch (k) {
        case Kind::None:      return "None";
        case Kind::Key:       return "Key";
        case Kind::QuotedKey: return "QuotedKey";
        case Kind::Reference: return "Reference";
        case Kind::Argument:  return "Argument";
        }
        return "?";
    }

    SchemaField field(const char *path, double presence, const char *type = "String")
    {
        SchemaField f;
        f.path = QString::fromLatin1(path);
        f.name = f.path.section(QLatin1Char('.'), -1);
        f.presence = presence;
        f.types = {{QString::fromLatin1(type), 100}};
        return f;
    }
}

TEST(FieldCompletionTest, finds_the_field_positions_of_the_plan)
{
    const std::vector<Case> cases{
        // Filter keys, also nested in $and/$or and $elemMatch.
        {"db.orders.find({ ", Kind::Key, "orders", "", 0},
        {"db.orders.find({ cust", Kind::Key, "orders", "cust", 4},
        {"db.orders.find({ status: \"A\", tot", Kind::Key, "orders", "tot", 3},
        {"db.orders.find({ $or: [ { st", Kind::Key, "orders", "st", 2},
        {"db.orders.find({ items: { $elemMatch: { q", Kind::Key, "orders", "q", 1},
        {"db.orders.findOne({ ", Kind::Key, "orders", "", 0},
        {"db.orders.countDocuments({ ", Kind::Key, "orders", "", 0},
        {"db.orders.deleteMany({ ", Kind::Key, "orders", "", 0},
        // Quoted and dotted keys.
        {"db.orders.find({ \"address.", Kind::QuotedKey, "orders", "address.", 8},
        {"db.orders.find({ 'cu", Kind::QuotedKey, "orders", "cu", 2},
        // Projection and sort.
        {"db.orders.find({}, { ", Kind::Key, "orders", "", 0},
        {"db.orders.find({ a: 1 }).sort({ cr", Kind::Key, "orders", "cr", 2},
        {"db.orders.find({}).limit(5).sort({ ", Kind::Key, "orders", "", 0},
        {"db.orders.createIndex({ ", Kind::Key, "orders", "", 0},
        // Update operator bodies.
        {"db.orders.updateOne({ _id: 1 }, { $set: { st", Kind::Key, "orders", "st", 2},
        {"db.orders.updateMany({}, { $inc: { ", Kind::Key, "orders", "", 0},
        // Aggregation: $match / $sort / $project keys, "$field" references.
        {"db.orders.aggregate([{ $match: { st", Kind::Key, "orders", "st", 2},
        {"db.orders.aggregate([{ $match: {} }, { $sort: { ", Kind::Key, "orders", "", 0},
        {"db.orders.aggregate([{ $group: { _id: \"$st", Kind::Reference, "orders", "st", 3},
        {"db.orders.aggregate([{ $project: { total: { $sum: \"$items.pr", Kind::Reference, "orders", "items.pr", 9},
        {"db.orders.aggregate([{ $project: { city: \"$", Kind::Reference, "orders", "", 1},
        // distinct("field").
        {"db.orders.distinct(\"st", Kind::Argument, "orders", "st", 2},
        {"db.orders.distinct(\"status\", { ", Kind::Key, "orders", "", 0},
        // getCollection and multi-line text.
        {"db.getCollection('order items').find({ q", Kind::Key, "order items", "q", 1},
        {"db.orders.find({\n  status: \"A\",\n  ", Kind::Key, "orders", "", 0},
    };
    for (const Case &c : cases) {
        const FieldCompletion::Context ctx = FieldCompletion::analyze(QString::fromUtf8(c.text));
        EXPECT_EQ(kindName(ctx.kind), kindName(c.kind)) << c.text;
        EXPECT_EQ(ctx.collection.toStdString(), std::string(c.kind == Kind::None ? "" : c.collection)) << c.text;
        EXPECT_EQ(ctx.prefix.toStdString(), std::string(c.prefix)) << c.text;
        EXPECT_EQ(ctx.replaceLength, c.replaceLength) << c.text;
    }
}

TEST(FieldCompletionTest, no_completion_where_no_field_name_goes)
{
    const std::vector<const char *> texts{
        "db.orders.find({ status: ",                       // a value
        "db.orders.find({ status: \"A",                     // a string value
        "db.orders.find({ status: { ",                      // operators of a field
        "db.orders.find({ $",                               // an operator: the shell's list
        "db.orders.updateOne({}, { ",                       // update operators
        "db.orders.aggregate([{ ",                          // stage names
        "db.orders.aggregate([{ $group: { ",                // output names
        "db.orders.find({ \"status\" ",                     // after a complete key
        "db.orders.find({ a: \"$st",                        // "$" is a literal in a filter
        "db.orders.find({}).forEach(function (d) { ",       // a function body
        "orders.find({ ",                                   // no db.
        "db.getSiblingDB('x').orders.find({ ",              // another database
        "db.orders.find({ // note ",                        // in a comment
        "db.orders.find({ /* { */ s",                       // after a block comment...
        "var x = { ",                                       // not a query
        "",
    };
    for (const char *text : texts) {
        const FieldCompletion::Context ctx = FieldCompletion::analyze(QString::fromUtf8(text));
        if (std::string(text) == "db.orders.find({ /* { */ s") {
            // ...a key again: the comment is skipped.
            EXPECT_EQ(kindName(ctx.kind), "Key") << text;
            continue;
        }
        EXPECT_EQ(kindName(ctx.kind), "None") << text;
    }
}

TEST(FieldCompletionTest, strings_comments_and_regexes_do_not_confuse_the_scan)
{
    const char *texts[] = {
        "db.orders.find({ note: \"a } ) ] {\", ",
        "db.orders.find({ re: /^(a|b)\\/[}{]/i, ",
        "db.orders.find({ // }\n  ",
        "db.orders.find({ s: 'it\\'s', ",
        "db.orders.find({}).sort({ a: 1 }).limit(2) ; db.orders.find({ ",
    };
    for (const char *text : texts) {
        const FieldCompletion::Context ctx = FieldCompletion::analyze(QString::fromUtf8(text));
        EXPECT_EQ(kindName(ctx.kind), "Key") << text;
        EXPECT_EQ(ctx.collection, QStringLiteral("orders")) << text;
    }
}

TEST(FieldCompletionTest, completions_quote_dotted_keys_and_put_rare_fields_last)
{
    const std::vector<SchemaField> fields{field("_id", 1.0, "ObjectId"), field("address", 1.0, "Object"),
                                          field("address.city", 0.98), field("legacyCode", 0.01),
                                          field("status", 0.99)};
    FieldCompletion::Context key;
    key.kind = Kind::Key;
    key.collection = "orders";
    std::vector<FieldCompletion::Item> items = FieldCompletion::complete(key, fields);
    ASSERT_EQ(items.size(), 5u);
    EXPECT_EQ(items[2].insert, QStringLiteral("\"address.city\""));
    EXPECT_EQ(items[3].label, QStringLiteral("status"));
    EXPECT_EQ(items[4].label, QStringLiteral("legacyCode"));   // 1 %
    EXPECT_EQ(items[3].detail, QStringLiteral("String · 99%"));
    EXPECT_EQ(items[4].detail, QStringLiteral("String · 1%"));

    key.prefix = "addr";
    items = FieldCompletion::complete(key, fields);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].insert, QStringLiteral("address"));

    FieldCompletion::Context quoted = key;
    quoted.kind = Kind::QuotedKey;
    quoted.prefix = "address.";
    items = FieldCompletion::complete(quoted, fields);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].insert, QStringLiteral("address.city"));

    FieldCompletion::Context reference = key;
    reference.kind = Kind::Reference;
    reference.prefix = "ST";   // case-insensitive
    items = FieldCompletion::complete(reference, fields);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].insert, QStringLiteral("$status"));

    EXPECT_TRUE(FieldCompletion::complete(FieldCompletion::Context(), fields).empty());
    EXPECT_EQ(FieldCompletion::complete(key, fields, 1).size(), 1u);
}

TEST(FieldCompletionTest, plain_keys)
{
    EXPECT_TRUE(FieldCompletion::isPlainKey("status"));
    EXPECT_TRUE(FieldCompletion::isPlainKey("_id"));
    EXPECT_TRUE(FieldCompletion::isPlainKey("$x"));
    EXPECT_FALSE(FieldCompletion::isPlainKey("address.city"));
    EXPECT_FALSE(FieldCompletion::isPlainKey("first name"));
    EXPECT_FALSE(FieldCompletion::isPlainKey("1st"));
}
