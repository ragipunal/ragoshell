#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "ragoshell/core/utils/ShellCompletion.h"

using namespace RagoShell;
using ShellCompletion::Context;
using ShellCompletion::Environment;
using ShellCompletion::Item;
using ShellCompletion::Receiver;
using ShellCompletion::Result;
using ShellCompletion::Scope;

namespace
{
    SchemaField makeField(const char *path, double presence, const char *type = "String")
    {
        SchemaField f;
        f.path = QString::fromUtf8(path);
        f.name = f.path.section(QLatin1Char('.'), -1);
        f.presence = presence;
        f.types = {{QString::fromUtf8(type), 100}};
        return f;
    }

    Environment sampleEnv()
    {
        Environment env;
        env.collections = {
            QStringLiteral("orders"),
            QStringLiteral("customers"),
            QStringLiteral("my-coll"),
            QStringLiteral("_internal"),
            QStringLiteral("stats"),
            QStringLiteral("it's"),
        };
        env.databases = {QStringLiteral("shop"), QStringLiteral("analytics")};
        env.fields = {
            makeField("_id", 1.0, "ObjectId"),
            makeField("status", 0.99, "String"),
            makeField("amount", 0.95, "Double"),
            makeField("shipping.city", 0.85, "String"),
            makeField("rareFlag", 0.02, "Boolean"),
        };
        return env;
    }

    QStringList insertsOf(const Result &res)
    {
        QStringList out;
        for (const Item &it : res.items)
            out << it.insert;
        return out;
    }

    QStringList labelsOf(const Result &res)
    {
        QStringList out;
        for (const Item &it : res.items)
            out << it.label;
        return out;
    }
}

TEST(ShellCompletionTest, chained_query_cursor_methods_after_find_and_modifiers)
{
    const Environment env = sampleEnv();

    // Exact user bug #1: after db.orders.find({}). must suggest sort({}), limit(, skip(, toArray(), etc.
    const Result r1 = ShellCompletion::complete(QStringLiteral("db.orders.find({})."), env);
    EXPECT_EQ(r1.context.scope, Scope::MemberAccess);
    EXPECT_EQ(r1.context.receiver, Receiver::QueryCursor);
    EXPECT_EQ(r1.context.collection, QStringLiteral("orders"));
    EXPECT_EQ(r1.context.replaceLength, 0);

    const QStringList ins1 = insertsOf(r1);
    EXPECT_TRUE(ins1.contains(QStringLiteral("sort({})")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("limit(")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("skip(")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("projection({})")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("hint(")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("collation({})")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("batchSize(")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("maxTimeMS(")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("pretty()")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("toArray()")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("count()")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("itcount()")));
    EXPECT_TRUE(ins1.contains(QStringLiteral("explain()")));
    // Must NEVER leak db.<collection> or collection-only methods onto a cursor
    EXPECT_FALSE(ins1.contains(QStringLiteral("orders")));
    EXPECT_FALSE(ins1.contains(QStringLiteral("db.orders")));
    EXPECT_FALSE(ins1.contains(QStringLiteral("find({})")));
    EXPECT_FALSE(ins1.contains(QStringLiteral("insertOne({})")));

    // Multi-step chain with getCollection, multi-line layout, and comments
    const Result r2 = ShellCompletion::complete(
        QStringLiteral("db.getCollection('my-coll')\n"
                       "  .find({ status: 'paid' }) // active filter\n"
                       "  /* sort stage */ .sort({ amount: -1 })\n"
                       "  .li"),
        env);
    EXPECT_EQ(r2.context.scope, Scope::MemberAccess);
    EXPECT_EQ(r2.context.receiver, Receiver::QueryCursor);
    EXPECT_EQ(r2.context.collection, QStringLiteral("my-coll"));
    EXPECT_EQ(r2.context.prefix, QStringLiteral("li"));
    EXPECT_EQ(r2.context.replaceLength, 2);
    EXPECT_EQ(insertsOf(r2), QStringList({QStringLiteral("limit(")}));

    // Bracket collection notation also chains cleanly
    const Result r3 = ShellCompletion::complete(QStringLiteral("db['orders'].find().skip(10).so"), env);
    EXPECT_EQ(r3.context.receiver, Receiver::QueryCursor);
    EXPECT_EQ(insertsOf(r3), QStringList({QStringLiteral("sort({})")}));

    // Terminal calls do NOT keep offering cursor methods or db.<collection>
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.find({}).toArray()."), env).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.findOne({})."), env).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.countDocuments({})."), env).items.empty());
}

TEST(ShellCompletionTest, aggregate_explain_system_mongo_and_namespace_receivers)
{
    const Environment env = sampleEnv();

    // AggCursor has cursor iteration/explain methods, NOT QueryCursor's sort()/limit()
    const Result agg = ShellCompletion::complete(QStringLiteral("db.orders.aggregate([{ $match: {} }])."), env);
    EXPECT_EQ(agg.context.receiver, Receiver::AggCursor);
    const QStringList aggIns = insertsOf(agg);
    EXPECT_TRUE(aggIns.contains(QStringLiteral("toArray()")));
    EXPECT_TRUE(aggIns.contains(QStringLiteral("itcount()")));
    EXPECT_TRUE(aggIns.contains(QStringLiteral("explain()")));
    EXPECT_FALSE(aggIns.contains(QStringLiteral("sort({})")));

    // Explainable resolves inner filter fields on the collection and terminates after `.explain().find({})`
    const Result exp = ShellCompletion::complete(QStringLiteral("db.orders.explain('executionStats')."), env);
    EXPECT_EQ(exp.context.receiver, Receiver::Explainable);
    EXPECT_TRUE(insertsOf(exp).contains(QStringLiteral("find({})")));
    EXPECT_TRUE(insertsOf(exp).contains(QStringLiteral("aggregate([])")));

    const Result expFilter = ShellCompletion::complete(QStringLiteral("db.orders.explain().find({ st"), env);
    EXPECT_EQ(expFilter.context.scope, Scope::FilterKey);
    EXPECT_EQ(expFilter.context.collection, QStringLiteral("orders"));
    EXPECT_EQ(insertsOf(expFilter), QStringList({QStringLiteral("status")}));
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.explain().find({})."), env).items.empty());

    // db.system.* namespace -> collection
    const Result sys = ShellCompletion::complete(QStringLiteral("db.system."), env);
    EXPECT_EQ(sys.context.receiver, Receiver::SystemNamespace);
    EXPECT_TRUE(insertsOf(sys).contains(QStringLiteral("users")));
    EXPECT_TRUE(insertsOf(sys).contains(QStringLiteral("js")));

    const Result sysColl = ShellCompletion::complete(QStringLiteral("db.system.users.fi"), env);
    EXPECT_EQ(sysColl.context.receiver, Receiver::Collection);
    EXPECT_EQ(sysColl.context.collection, QStringLiteral("system.users"));
    EXPECT_TRUE(insertsOf(sysColl).contains(QStringLiteral("find({})")));

    // db.getMongo().
    const Result mongo = ShellCompletion::complete(QStringLiteral("db.getMongo()."), env);
    EXPECT_EQ(mongo.context.receiver, Receiver::Mongo);
    EXPECT_TRUE(insertsOf(mongo).contains(QStringLiteral("getDBNames()")));
    EXPECT_TRUE(insertsOf(mongo).contains(QStringLiteral("setReadPref(")));

    // db.getSiblingDB('analytics') does not leak active-DB collections or schema fields
    const Result sib = ShellCompletion::complete(QStringLiteral("db.getSiblingDB('analytics')."), env);
    EXPECT_EQ(sib.context.receiver, Receiver::SiblingDatabase);
    EXPECT_FALSE(sib.needsLiveCollections);
    EXPECT_FALSE(insertsOf(sib).contains(QStringLiteral("orders")));
    EXPECT_TRUE(insertsOf(sib).contains(QStringLiteral("getCollection(")));

    const Result sibColl = ShellCompletion::complete(QStringLiteral("db.getSiblingDB('analytics').events.find({ "), env);
    EXPECT_TRUE(sibColl.context.collection.isEmpty());
    EXPECT_FALSE(sibColl.needsSchema);

    // rs., sh., EJSON., console.
    EXPECT_TRUE(insertsOf(ShellCompletion::complete(QStringLiteral("rs."), env)).contains(QStringLiteral("status()")));
    EXPECT_TRUE(insertsOf(ShellCompletion::complete(QStringLiteral("sh."), env)).contains(QStringLiteral("getBalancerState()")));
    EXPECT_TRUE(insertsOf(ShellCompletion::complete(QStringLiteral("EJSON."), env)).contains(QStringLiteral("stringify(")));
    EXPECT_TRUE(insertsOf(ShellCompletion::complete(QStringLiteral("console."), env)).contains(QStringLiteral("log(")));
}

TEST(ShellCompletionTest, local_variable_receiver_inference_and_reassignment)
{
    const Environment env = sampleEnv();

    const Result curDot = ShellCompletion::complete(
        QStringLiteral("const cur = db.orders.find({ status: 'open' });\ncur."), env);
    EXPECT_EQ(curDot.context.receiver, Receiver::QueryCursor);
    EXPECT_EQ(curDot.context.collection, QStringLiteral("orders"));
    EXPECT_TRUE(insertsOf(curDot).contains(QStringLiteral("sort({})")));

    // Schema fields still work inside methods called on the cursor variable
    const Result curSort = ShellCompletion::complete(
        QStringLiteral("const cur = db.orders.find({ status: 'open' });\ncur.sort({ am"), env);
    EXPECT_EQ(curSort.context.scope, Scope::FieldsKey);
    EXPECT_EQ(curSort.context.collection, QStringLiteral("orders"));
    EXPECT_EQ(insertsOf(curSort), QStringList({QStringLiteral("amount")}));

    // Variable reassignment updates the inferred receiver
    const Result reassigned = ShellCompletion::complete(
        QStringLiteral("let x = db.orders.find({});\nx = db.customers;\nx.ag"), env);
    EXPECT_EQ(reassigned.context.receiver, Receiver::Collection);
    EXPECT_EQ(reassigned.context.collection, QStringLiteral("customers"));
    EXPECT_EQ(insertsOf(reassigned), QStringList({QStringLiteral("aggregate([])")}));
}

TEST(ShellCompletionTest, never_spams_db_collections_in_irrelevant_cursor_positions)
{
    const Environment env = sampleEnv();

    // Bare empty, bare 'd', bare 'db' without '.' must never list db.<collection>
    EXPECT_TRUE(ShellCompletion::complete(QString(), env).items.empty());

    const QStringList bareD = insertsOf(ShellCompletion::complete(QStringLiteral("d"), env));
    EXPECT_TRUE(bareD.contains(QStringLiteral("db")));
    EXPECT_FALSE(bareD.contains(QStringLiteral("orders")));
    EXPECT_FALSE(bareD.contains(QStringLiteral("db.orders")));

    const QStringList bareDb = insertsOf(ShellCompletion::complete(QStringLiteral("db"), env));
    EXPECT_EQ(bareDb.front(), QStringLiteral("db"));
    EXPECT_FALSE(bareDb.contains(QStringLiteral("orders")));
    EXPECT_FALSE(bareDb.contains(QStringLiteral("db.orders")));

    // Numbers, comments, arbitrary strings, regex literals -> zero suggestions
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("12."), env).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.find({ amount: 3.14."), env).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("// db."), env).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("/* db.orders."), env).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.find({ status: 'db."), env).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.find({ status: /^db."), env).items.empty());

    // Delimiter triggers (' ', ',', '{') in value/statement positions do not open popups
    Environment delimEnv = env;
    delimEnv.delimiterTrigger = true;
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.find({ status: "), delimEnv).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db.orders.find({ status: 'A' }, "), delimEnv).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("function helper() { "), delimEnv).items.empty());
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db. "), delimEnv).items.empty());
}

TEST(ShellCompletionTest, db_dot_formats_special_and_colliding_collection_names_safely)
{
    Environment env = sampleEnv();
    const Result dbDot = ShellCompletion::complete(QStringLiteral("db."), env);
    EXPECT_TRUE(dbDot.needsLiveCollections);
    const QStringList ins = insertsOf(dbDot);
    EXPECT_TRUE(ins.contains(QStringLiteral("orders")));
    EXPECT_TRUE(ins.contains(QStringLiteral("getCollection('my-coll')")));
    EXPECT_TRUE(ins.contains(QStringLiteral("getCollection('_internal')")));
    EXPECT_TRUE(ins.contains(QStringLiteral("getCollection('stats')")));
    EXPECT_TRUE(ins.contains(QStringLiteral("getCollection('it\\'s')")));
    EXPECT_TRUE(ins.contains(QStringLiteral("stats()")));

    // Typing `db.my` still matches `my-coll` and inserts `getCollection('my-coll')`
    const Result dbMy = ShellCompletion::complete(QStringLiteral("db.my"), env);
    EXPECT_EQ(dbMy.context.replaceLength, 2);
    EXPECT_EQ(insertsOf(dbMy), QStringList({QStringLiteral("getCollection('my-coll')")}));

    // AutocompleteNoCollectionNames suppresses collection names while keeping db methods
    env.mode = AutocompleteNoCollectionNames;
    const QStringList noColls = insertsOf(ShellCompletion::complete(QStringLiteral("db."), env));
    EXPECT_FALSE(noColls.contains(QStringLiteral("orders")));
    EXPECT_FALSE(noColls.contains(QStringLiteral("getCollection('my-coll')")));
    EXPECT_TRUE(noColls.contains(QStringLiteral("stats()")));

    // AutocompleteNone disables all completions
    env.mode = AutocompleteNone;
    EXPECT_TRUE(ShellCompletion::complete(QStringLiteral("db."), env).items.empty());
}

TEST(ShellCompletionTest, contextual_mql_operators_and_schema_fields)
{
    const Environment env = sampleEnv();

    // 1. Aggregation pipeline stages
    const QStringList stages = insertsOf(ShellCompletion::complete(
        QStringLiteral("db.orders.aggregate([{ "), env));
    EXPECT_TRUE(stages.contains(QStringLiteral("$match")));
    EXPECT_TRUE(stages.contains(QStringLiteral("$group")));
    EXPECT_TRUE(stages.contains(QStringLiteral("$lookup")));
    EXPECT_FALSE(stages.contains(QStringLiteral("$eq")));
    EXPECT_FALSE(stages.contains(QStringLiteral("status")));

    // 2. Update document operators
    const QStringList updates = insertsOf(ShellCompletion::complete(
        QStringLiteral("db.orders.updateOne({ _id: 1 }, { "), env));
    EXPECT_TRUE(updates.contains(QStringLiteral("$set")));
    EXPECT_TRUE(updates.contains(QStringLiteral("$inc")));
    EXPECT_TRUE(updates.contains(QStringLiteral("$push")));
    EXPECT_FALSE(updates.contains(QStringLiteral("$match")));
    EXPECT_FALSE(updates.contains(QStringLiteral("$gt")));

    // 3. Field condition operators
    const QStringList conds = insertsOf(ShellCompletion::complete(
        QStringLiteral("db.orders.find({ amount: { "), env));
    EXPECT_TRUE(conds.contains(QStringLiteral("$gte")));
    EXPECT_TRUE(conds.contains(QStringLiteral("$in")));
    EXPECT_TRUE(conds.contains(QStringLiteral("$exists")));
    EXPECT_FALSE(conds.contains(QStringLiteral("$set")));
    EXPECT_FALSE(conds.contains(QStringLiteral("$group")));

    // 4. Filter keys & top-level filter '$' operators
    const Result filterKeys = ShellCompletion::complete(QStringLiteral("db.orders.find({ "), env);
    const QStringList fkIns = insertsOf(filterKeys);
    EXPECT_EQ(fkIns.front(), QStringLiteral("_id"));
    EXPECT_TRUE(fkIns.contains(QStringLiteral("status")));
    EXPECT_TRUE(fkIns.contains(QStringLiteral("\"shipping.city\"")));
    EXPECT_EQ(fkIns.back(), QStringLiteral("rareFlag")); // < 5% presence ranked last

    const QStringList topOps = insertsOf(ShellCompletion::complete(QStringLiteral("db.orders.find({ $"), env));
    EXPECT_TRUE(topOps.contains(QStringLiteral("$or")));
    EXPECT_TRUE(topOps.contains(QStringLiteral("$and")));
    EXPECT_TRUE(topOps.contains(QStringLiteral("$expr")));
    EXPECT_FALSE(topOps.contains(QStringLiteral("$group")));

    // 5. $group output key vs accumulator vs "$field" reference
    EXPECT_EQ(insertsOf(ShellCompletion::complete(
                  QStringLiteral("db.orders.aggregate([{ $group: { "), env)),
              QStringList({QStringLiteral("_id")}));

    const QStringList accs = insertsOf(ShellCompletion::complete(
        QStringLiteral("db.orders.aggregate([{ $group: { _id: '$status', total: { "), env));
    EXPECT_TRUE(accs.contains(QStringLiteral("$sum")));
    EXPECT_TRUE(accs.contains(QStringLiteral("$avg")));
    EXPECT_FALSE(accs.contains(QStringLiteral("$match")));

    const Result refRes = ShellCompletion::complete(
        QStringLiteral("db.orders.aggregate([{ $group: { _id: \"$ship"), env);
    EXPECT_EQ(refRes.context.scope, Scope::FieldReference);
    EXPECT_EQ(refRes.context.replaceLength, 5); // "$ship" replaced by "$shipping.city"
    EXPECT_EQ(insertsOf(refRes), QStringList({QStringLiteral("$shipping.city")}));

    // 6. $project computed expression operators
    const QStringList exprOps = insertsOf(ShellCompletion::complete(
        QStringLiteral("db.orders.aggregate([{ $project: { label: { "), env));
    EXPECT_TRUE(exprOps.contains(QStringLiteral("$concat")));
    EXPECT_TRUE(exprOps.contains(QStringLiteral("$cond")));
    EXPECT_TRUE(exprOps.contains(QStringLiteral("$ifNull")));
}

TEST(ShellCompletionTest, lookup_stage_switches_collection_and_string_args_escape_quotes)
{
    const Environment env = sampleEnv();

    // $lookup spec keys
    const QStringList lookupKeys = labelsOf(ShellCompletion::complete(
        QStringLiteral("db.orders.aggregate([{ $lookup: { "), env));
    EXPECT_TRUE(lookupKeys.contains(QStringLiteral("from")));
    EXPECT_TRUE(lookupKeys.contains(QStringLiteral("localField")));
    EXPECT_TRUE(lookupKeys.contains(QStringLiteral("foreignField")));
    EXPECT_TRUE(lookupKeys.contains(QStringLiteral("pipeline")));

    // localField uses outer collection ("orders"), foreignField & nested pipeline switch to `from` ("customers")
    const Context localCtx = ShellCompletion::analyze(
        QStringLiteral("db.orders.aggregate([{ $lookup: { from: 'customers', localField: '"));
    EXPECT_EQ(localCtx.scope, Scope::DistinctFieldArg);
    EXPECT_EQ(localCtx.collection, QStringLiteral("orders"));

    const Context foreignCtx = ShellCompletion::analyze(
        QStringLiteral("db.orders.aggregate([{ $lookup: { from: 'customers', localField: 'cid', foreignField: '"));
    EXPECT_EQ(foreignCtx.scope, Scope::DistinctFieldArg);
    EXPECT_EQ(foreignCtx.collection, QStringLiteral("customers"));

    const Context subPipeCtx = ShellCompletion::analyze(
        QStringLiteral("db.orders.aggregate([{ $lookup: { from: 'customers', pipeline: [{ $match: { "));
    EXPECT_EQ(subPipeCtx.scope, Scope::FilterKey);
    EXPECT_EQ(subPipeCtx.collection, QStringLiteral("customers"));

    // Quote-aware escaping in getCollection('...') vs getCollection("...")
    const Result singleQ = ShellCompletion::complete(QStringLiteral("db.getCollection('it"), env);
    EXPECT_EQ(singleQ.context.scope, Scope::CollectionNameArg);
    EXPECT_EQ(insertsOf(singleQ), QStringList({QStringLiteral("it\\'s")}));

    const Result doubleQ = ShellCompletion::complete(QStringLiteral("db.getCollection(\"it"), env);
    EXPECT_EQ(insertsOf(doubleQ), QStringList({QStringLiteral("it's")}));

    // Database names, explain verbosities, show & use commands
    const QStringList dbs = insertsOf(ShellCompletion::complete(QStringLiteral("db.getSiblingDB(\""), env));
    EXPECT_TRUE(dbs.contains(QStringLiteral("shop")));
    EXPECT_TRUE(dbs.contains(QStringLiteral("admin")));

    const QStringList verbs = insertsOf(ShellCompletion::complete(QStringLiteral("db.orders.explain('"), env));
    EXPECT_TRUE(verbs.contains(QStringLiteral("executionStats")));
    EXPECT_TRUE(verbs.contains(QStringLiteral("allPlansExecution")));

    EXPECT_TRUE(insertsOf(ShellCompletion::complete(QStringLiteral("show "), env)).contains(QStringLiteral("collections")));
    EXPECT_TRUE(insertsOf(ShellCompletion::complete(QStringLiteral("use an"), env)).contains(QStringLiteral("analytics")));
}
