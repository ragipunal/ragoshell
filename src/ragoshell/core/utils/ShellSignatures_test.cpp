#include <gtest/gtest.h>

#include "ragoshell/core/utils/ShellSignatures.h"

using namespace RagoShell;

namespace
{
    ShellSignatures::CallTip tipFor(const char *text)
    {
        return ShellSignatures::lookup(QString::fromUtf8(text));
    }
}

TEST(ShellSignaturesTest, collection_method_and_active_parameter)
{
    auto t = tipFor("db.orders.find(");
    ASSERT_TRUE(t.found);
    EXPECT_EQ(t.name, QStringLiteral("find"));
    EXPECT_EQ(ShellSignatures::label(t), QStringLiteral("find(filter, projection, options)"));
    EXPECT_EQ(t.activeParam, 0);
    EXPECT_EQ(t.parenOffset, 1);

    t = tipFor("db.orders.find({ a: 1, b: [1, 2] }, ");
    EXPECT_EQ(t.activeParam, 1) << "commas inside {} and [] do not count";

    t = tipFor("db.getCollection('my coll').updateOne({ x: 1 }, { $set: { y: 2 } }, { upsert: tr");
    EXPECT_EQ(t.name, QStringLiteral("updateOne"));
    EXPECT_EQ(t.activeParam, 2);
}

TEST(ShellSignaturesTest, receiver_decides_the_table)
{
    EXPECT_EQ(ShellSignatures::label(tipFor("db.stats(")), QStringLiteral("stats(scale)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.orders.stats(")), QStringLiteral("stats(scaleOrOptions)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.orders.find({}).count(")), QStringLiteral("count(applySkipLimit)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.orders.count(")), QStringLiteral("count(filter, options)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.orders.find({}).sort({ a: 1 }).limit(")), QStringLiteral("limit(n)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.orders.find({}).projection(")), QStringLiteral("projection(projection)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.getCollection('orders').count(")), QStringLiteral("count(filter, options)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.orders.explain('executionStats').find(")), QStringLiteral("find(filter, projection, options)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.getSiblingDB('analytics').stats(")), QStringLiteral("stats(scale)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.getMongo().setReadPref(")), QStringLiteral("setReadPref(mode)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("rs.add(")), QStringLiteral("add(hostOrMember, arbiterOnly)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("sh.status(")), QStringLiteral("status()"));
    EXPECT_EQ(ShellSignatures::label(tipFor("EJSON.stringify(")), QStringLiteral("stringify(value, options, indent)"));
    EXPECT_EQ(ShellSignatures::label(tipFor("db.getSiblingDB(")), QStringLiteral("getSiblingDB(name)"));
}

TEST(ShellSignaturesTest, innermost_known_call_wins)
{
    auto t = tipFor("db.c.find({ _id: ObjectId(");
    EXPECT_EQ(t.name, QStringLiteral("ObjectId"));
    t = tipFor("db.c.find({ _id: ObjectId('abc'), at: ISODate('2024') }, { a: ");
    EXPECT_EQ(t.name, QStringLiteral("find"));
    EXPECT_EQ(t.activeParam, 1);
    t = tipFor("db.c.find({ a: myHelper(1, ");
    EXPECT_EQ(t.name, QStringLiteral("find")) << "unknown functions fall through to the outer call";
}

TEST(ShellSignaturesTest, strings_and_comments_are_skipped)
{
    auto t = tipFor("db.c.find({ name: 'a, b (c' }, ");
    EXPECT_EQ(t.name, QStringLiteral("find"));
    EXPECT_EQ(t.activeParam, 1);
    EXPECT_FALSE(tipFor("db.c.find({ name: 'still typ").found) << "caret inside a string";
    EXPECT_FALSE(tipFor("// db.c.find(").found) << "caret inside a comment";
    t = tipFor("/* find( */ db.c.aggregate([");
    EXPECT_EQ(t.name, QStringLiteral("aggregate"));
}

TEST(ShellSignaturesTest, no_tip_outside_calls)
{
    EXPECT_FALSE(tipFor("").found);
    EXPECT_FALSE(tipFor("db.c.find({})").found);
    EXPECT_FALSE(tipFor("db.c.find({}); db.c.").found);
    EXPECT_FALSE(tipFor("db.c.unknownMethod(").found);
    // "x.db" is a collection named db, not the shell's db object.
    EXPECT_EQ(ShellSignatures::label(tipFor("x.db.stats(")), QStringLiteral("stats(scaleOrOptions)"));
}
