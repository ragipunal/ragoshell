#pragma once

#include <QString>
#include <QStringList>
#include <vector>

#include <bsoncxx/document/view.hpp>

namespace RagoShell
{
    /**
     * @brief One plan stage, flattened depth-first (children follow their parent).
     * Counters are -1 when the explain verbosity did not report them.
     */
    struct ExplainStage
    {
        int depth = 0;
        QString stage;          ///< e.g. IXSCAN, FETCH, COLLSCAN, SORT, SHARD_MERGE
        QString detail;         ///< index name + key pattern, filter, sort pattern...
        qint64 nReturned = -1;
        qint64 docsExamined = -1;
        qint64 keysExamined = -1;
        qint64 executionTimeMs = -1;
        bool warning = false;   ///< COLLSCAN / in-memory SORT
    };

    /**
     * @brief Parsed result of find/aggregate/count `.explain()` output.
     */
    struct ExplainPlan
    {
        bool valid = false;
        bool hasExecutionStats = false;
        QString ns;
        qint64 nReturned = -1;
        qint64 totalDocsExamined = -1;
        qint64 totalKeysExamined = -1;
        qint64 executionTimeMs = -1;
        QStringList indexesUsed;
        QStringList warnings;
        std::vector<ExplainStage> stages;
    };

    namespace ExplainPlanParser
    {
        /// True when the document looks like explain output.
        bool isExplain(bsoncxx::document::view doc);

        /// Supports classic and SBE (MongoDB 5+) plans, aggregate `$cursor`
        /// stages and sharded `shards` plans. Invalid when not explain output.
        ExplainPlan parse(bsoncxx::document::view doc);
    }
}
