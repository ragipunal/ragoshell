#pragma once

#include <atomic>
#include <vector>

#include <QString>
#include <QStringList>

#include <bsoncxx/document/value.hpp>

namespace RagoShell
{
    class ConnectionSettings;

    /**
     * @brief One aggregation stage as edited in the pipeline builder:
     * operator ("$match") plus its body in Mongo Shell JSON ("{ age: { $gt: 3 } }").
     */
    struct PipelineStage
    {
        QString op;
        QString body;
        bool enabled = true;
    };

    namespace AggregationPipeline
    {
        /// Frequently used stage operators, in menu order.
        QStringList commonOperators();
        /// Starter body for a newly chosen operator ("{}" when unknown).
        QString stageTemplate(const QString &op);
        /// $out / $merge write data and are never previewed.
        bool isWriteStage(const QString &op);

        /// "{ <op>: <body> }" — the shell text of one stage.
        QString stageText(const PipelineStage &stage);
        /// Parses one stage into BSON; false with 'error' set when invalid.
        bool parseStage(const PipelineStage &stage, bsoncxx::document::value &out, QString &error);

        /// db.getCollection('<coll>').aggregate([...]) for the enabled stages.
        QString pipelineScript(const QString &collection, const std::vector<PipelineStage> &stages);

        struct PreviewResult
        {
            bool ok = false;
            QString error;
            std::vector<bsoncxx::document::value> documents;
        };

        /**
         * @brief Runs 'stages' + { $limit: limit } on <db>.<collection> with a
         * standalone client (no shell involved), bounded by maxTimeMs.
         * Blocking: call from a worker thread.
         */
        PreviewResult runPreview(const ConnectionSettings &connection,
                                 const QString &database, const QString &collection,
                                 const std::vector<bsoncxx::document::value> &stages,
                                 int limit, int maxTimeMs);
    }
}
