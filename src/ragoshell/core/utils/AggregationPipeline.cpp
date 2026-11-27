#include "ragoshell/core/utils/AggregationPipeline.h"
#include <QCoreApplication>

#include <chrono>
#include <exception>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/aggregate.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/transport/InMemoryDemoTransport.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"

namespace RagoShell
{
    namespace
    {
        struct OperatorTemplate
        {
            const char *op;
            const char *body;
        };

        // Menu order: most used first.
        constexpr OperatorTemplate kTemplates[] = {
            {"$match", "{  }"},
            {"$project", "{ _id: 1 }"},
            {"$group", "{ _id: \"$field\", count: { $sum: 1 } }"},
            {"$sort", "{ _id: 1 }"},
            {"$limit", "10"},
            {"$skip", "0"},
            {"$unwind", "\"$field\""},
            {"$lookup", "{ from: \"collection\", localField: \"field\", foreignField: \"_id\", as: \"joined\" }"},
            {"$addFields", "{ newField: \"$field\" }"},
            {"$set", "{ newField: \"$field\" }"},
            {"$unset", "\"field\""},
            {"$count", "\"count\""},
            {"$sample", "{ size: 10 }"},
            {"$facet", "{ facetName: [ { $match: {} } ] }"},
            {"$bucket", "{ groupBy: \"$field\", boundaries: [ 0, 100 ], default: \"other\" }"},
            {"$bucketAuto", "{ groupBy: \"$field\", buckets: 5 }"},
            {"$sortByCount", "\"$field\""},
            {"$replaceRoot", "{ newRoot: \"$field\" }"},
            {"$geoNear", "{ near: { type: \"Point\", coordinates: [ 0, 0 ] }, distanceField: \"distance\" }"},
            {"$graphLookup", "{ from: \"collection\", startWith: \"$field\", connectFromField: \"field\", connectToField: \"_id\", as: \"graph\" }"},
            {"$out", "\"collection\""},
            {"$merge", "{ into: \"collection\" }"},
        };

        // Shell strings use single quotes; escape for db.getCollection('…').
        QString quoteCollection(const QString &name)
        {
            QString escaped = name;
            escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
            escaped.replace(QLatin1Char('\''), QLatin1String("\\'"));
            return QLatin1Char('\'') + escaped + QLatin1Char('\'');
        }
    }

    QStringList AggregationPipeline::commonOperators()
    {
        QStringList ops;
        for (const auto &t : kTemplates)
            ops << QString::fromLatin1(t.op);
        return ops;
    }

    QString AggregationPipeline::stageTemplate(const QString &op)
    {
        for (const auto &t : kTemplates) {
            if (op == QLatin1String(t.op))
                return QString::fromLatin1(t.body);
        }
        return QStringLiteral("{  }");
    }

    bool AggregationPipeline::isWriteStage(const QString &op)
    {
        return op == QLatin1String("$out") || op == QLatin1String("$merge");
    }

    QString AggregationPipeline::stageText(const PipelineStage &stage)
    {
        return QStringLiteral("{ %1: %2 }").arg(stage.op.trimmed(), stage.body.trimmed());
    }

    bool AggregationPipeline::parseStage(const PipelineStage &stage,
                                         bsoncxx::document::value &out, QString &error)
    {
        const QString op = stage.op.trimmed();
        if (!op.startsWith(QLatin1Char('$')) || op.size() < 2) {
            error = QCoreApplication::translate("AggregationPipeline", "Stage operator must start with '$'.");
            return false;
        }
        if (stage.body.trimmed().isEmpty()) {
            error = QCoreApplication::translate("AggregationPipeline", "Stage body is empty.");
            return false;
        }
        bsoncxx::document::value parsed = bsoncxx::builder::basic::make_document();
        QString parseError;
        if (!MongoJsonUtils::parseSingleDocument(stageText(stage), parsed, parseError)) {
            error = parseError.isEmpty() ? QCoreApplication::translate("AggregationPipeline", "Invalid stage JSON.") : parseError;
            return false;
        }
        // A stray comma or brace can turn the body into several top-level keys.
        const auto view = parsed.view();
        if (std::distance(view.begin(), view.end()) != 1) {
            error = QStringLiteral("A stage must contain exactly one operator.");
            return false;
        }
        out = std::move(parsed);
        return true;
    }

    QString AggregationPipeline::pipelineScript(const QString &collection,
                                                const std::vector<PipelineStage> &stages)
    {
        QStringList lines;
        for (const PipelineStage &s : stages) {
            if (s.enabled)
                lines << QStringLiteral("    ") + stageText(s);
        }
        const QString head = QStringLiteral("db.getCollection(%1).aggregate([").arg(quoteCollection(collection));
        if (lines.isEmpty())
            return head + QStringLiteral("])");
        return head + QLatin1Char('\n') + lines.join(QStringLiteral(",\n")) + QStringLiteral("\n])");
    }

    AggregationPipeline::PreviewResult AggregationPipeline::runPreview(
        const ConnectionSettings &connection, const QString &database, const QString &collection,
        const std::vector<bsoncxx::document::value> &stages, int limit, int maxTimeMs)
    {
        PreviewResult result;
        if (connection.hasTransport()) {
#if defined(Q_OS_WASM)
            if (connection.transportKind() == "memory") {
                InMemoryDemoTransport demo(InMemoryDemoTransport::sharedStore());
                if (demo.connect(QStringLiteral("{}")).ok) {
                    try {
                        namespace basic = bsoncxx::builder::basic;
                        basic::array pipe;
                        for (const auto &stage : stages)
                            pipe.append(stage.view());
                        if (limit > 0)
                            pipe.append(basic::make_document(basic::kvp("$limit", limit)));
                        const auto cmd = basic::make_document(
                            basic::kvp("aggregate", collection.toStdString()),
                            basic::kvp("pipeline", pipe.extract()),
                            basic::kvp("cursor", basic::make_document()),
                            basic::kvp("maxTimeMS", std::max<std::int32_t>(1, maxTimeMs)));
                        const auto res = demo.runCommand(
                            database,
                            QString::fromStdString(
                                bsoncxx::to_json(cmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
                        if (!res.ok) {
                            result.error = res.error;
                            return result;
                        }
                        const auto doc = bsoncxx::from_json(res.ejson.toStdString());
                        if (const auto batch = doc.view()["cursor"]["firstBatch"];
                            batch && batch.type() == bsoncxx::type::k_array) {
                            for (const auto &el : batch.get_array().value) {
                                if (el.type() == bsoncxx::type::k_document)
                                    result.documents.emplace_back(el.get_document().value);
                            }
                        }
                        result.ok = true;
                        return result;
                    } catch (const std::exception &ex) {
                        result.error = QString::fromUtf8(ex.what());
                        return result;
                    }
                }
            }
#endif
            result.error = QStringLiteral("Live preview is not available over gateway channels.");
            return result;
        }
        try {
            mongocxx::client client{mongocxx::uri{buildMongoUri(connection)},
                                    buildMongoClientOptions(connection)};
            auto coll = client.database(database.toStdString()).collection(collection.toStdString());

            mongocxx::pipeline pipeline;
            for (const auto &stage : stages)
                pipeline.append_stage(stage.view());
            pipeline.limit(limit);

            mongocxx::options::aggregate opts;
            opts.max_time(std::chrono::milliseconds(maxTimeMs));
            for (auto &&doc : coll.aggregate(pipeline, opts))
                result.documents.emplace_back(doc);
            result.ok = true;
        } catch (const std::exception &ex) {
            result.error = QString::fromUtf8(ex.what());
        }
        return result;
    }
}
