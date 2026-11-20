#include "ragoshell/core/utils/ExplainPlan.h"
#include <QCoreApplication>

#include <algorithm>

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

namespace RagoShell
{
    namespace
    {
        using bsoncxx::document::view;

        qint64 number(view d, const char *key)
        {
            const auto el = d[key];
            if (!el)
                return -1;
            switch (el.type()) {
            case bsoncxx::type::k_int32:  return el.get_int32().value;
            case bsoncxx::type::k_int64:  return el.get_int64().value;
            case bsoncxx::type::k_double: return static_cast<qint64>(el.get_double().value);
            default:                      return -1;
            }
        }

        QString string(view d, const char *key)
        {
            const auto el = d[key];
            if (!el || el.type() != bsoncxx::type::k_string)
                return QString();
            const auto s = el.get_string().value;
            return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
        }

        bool subdocument(view d, const char *key, view *out)
        {
            const auto el = d[key];
            if (!el || el.type() != bsoncxx::type::k_document)
                return false;
            *out = el.get_document().value;
            return true;
        }

        QString compactJson(view d, int maxChars = 120)
        {
            QString json = QString::fromStdString(
                bsoncxx::to_json(d, bsoncxx::ExtendedJsonMode::k_relaxed)).simplified();
            if (json.size() > maxChars)
                json = json.left(maxChars - 1) + QStringLiteral("…");
            return json;
        }

        QString stageDetail(view s)
        {
            view sub;
            const QString index = string(s, "indexName");
            if (!index.isEmpty())
                return subdocument(s, "keyPattern", &sub) ? index + QLatin1Char(' ') + compactJson(sub) : index;
            if (subdocument(s, "filter", &sub) && !sub.empty())
                return QStringLiteral("filter ") + compactJson(sub);
            if (subdocument(s, "sortPattern", &sub))
                return QStringLiteral("sort ") + compactJson(sub);
            if (const qint64 n = number(s, "limitAmount"); n >= 0)
                return QStringLiteral("limit %1").arg(n);
            if (const qint64 n = number(s, "skipAmount"); n >= 0)
                return QStringLiteral("skip %1").arg(n);
            return QString();
        }

        void walk(view s, int depth, ExplainPlan &plan);

        void walkShards(view s, int depth, ExplainPlan &plan)
        {
            const auto shards = s["shards"];
            if (!shards || shards.type() != bsoncxx::type::k_array)
                return;
            for (const auto &item : shards.get_array().value) {
                if (item.type() != bsoncxx::type::k_document)
                    continue;
                const view shard = item.get_document().value;
                ExplainStage header;
                header.depth = depth;
                header.stage = QStringLiteral("SHARD");
                header.detail = string(shard, "shardName");
                header.nReturned = number(shard, "nReturned");
                header.docsExamined = number(shard, "totalDocsExamined");
                header.keysExamined = number(shard, "totalKeysExamined");
                header.executionTimeMs = number(shard, "executionTimeMillis");
                plan.stages.push_back(header);

                view child;
                if (subdocument(shard, "executionStages", &child) ||
                    subdocument(shard, "winningPlan", &child)) {
                    view sbe;
                    walk(subdocument(child, "queryPlan", &sbe) ? sbe : child, depth + 1, plan);
                }
            }
        }

        void walk(view s, int depth, ExplainPlan &plan)
        {
            ExplainStage st;
            st.depth = depth;
            st.stage = string(s, "stage");
            st.detail = stageDetail(s);
            st.nReturned = number(s, "nReturned");
            st.docsExamined = number(s, "docsExamined");
            st.keysExamined = number(s, "keysExamined");
            st.executionTimeMs = number(s, "executionTimeMillisEstimate");
            // SORT is a blocking in-memory sort; SORT_KEY_GENERATOR alone is not.
            st.warning = st.stage == QLatin1String("COLLSCAN") || st.stage == QLatin1String("SORT");
            const QString index = string(s, "indexName");
            if (!index.isEmpty() && !plan.indexesUsed.contains(index))
                plan.indexesUsed.append(index);
            if (!st.stage.isEmpty())
                plan.stages.push_back(st);

            const int childDepth = st.stage.isEmpty() ? depth : depth + 1;
            for (const char *key : {"inputStage", "outerStage", "innerStage", "thenStage", "elseStage"}) {
                view child;
                if (subdocument(s, key, &child))
                    walk(child, childDepth, plan);
            }
            const auto inputs = s["inputStages"];
            if (inputs && inputs.type() == bsoncxx::type::k_array) {
                for (const auto &item : inputs.get_array().value) {
                    if (item.type() == bsoncxx::type::k_document)
                        walk(item.get_document().value, childDepth, plan);
                }
            }
            walkShards(s, childDepth, plan);
        }

        // Aggregate explain wraps the find part in stages[0].$cursor.
        bool cursorStage(view doc, view *out)
        {
            const auto stages = doc["stages"];
            if (!stages || stages.type() != bsoncxx::type::k_array)
                return false;
            const auto arr = stages.get_array().value;
            if (arr.empty() || arr[0].type() != bsoncxx::type::k_document)
                return false;
            return subdocument(arr[0].get_document().value, "$cursor", out);
        }
    }

    bool ExplainPlanParser::isExplain(view doc)
    {
        view tmp;
        return subdocument(doc, "queryPlanner", &tmp) ||
               (cursorStage(doc, &tmp) && subdocument(tmp, "queryPlanner", &tmp));
    }

    ExplainPlan ExplainPlanParser::parse(view doc)
    {
        ExplainPlan plan;
        view root = doc;
        view queryPlanner;
        if (!subdocument(root, "queryPlanner", &queryPlanner)) {
            view cursor;
            if (!cursorStage(doc, &cursor) || !subdocument(cursor, "queryPlanner", &queryPlanner))
                return plan;
            root = cursor;
        }
        plan.ns = string(queryPlanner, "namespace");

        view tree;
        view executionStats;
        if (subdocument(root, "executionStats", &executionStats)) {
            plan.hasExecutionStats = true;
            plan.nReturned = number(executionStats, "nReturned");
            plan.totalDocsExamined = number(executionStats, "totalDocsExamined");
            plan.totalKeysExamined = number(executionStats, "totalKeysExamined");
            plan.executionTimeMs = number(executionStats, "executionTimeMillis");
            if (!subdocument(executionStats, "executionStages", &tree))
                return plan;
        } else {
            if (!subdocument(queryPlanner, "winningPlan", &tree))
                return plan;
            view sbe;
            if (subdocument(tree, "queryPlan", &sbe))
                tree = sbe;
        }

        walk(tree, 0, plan);
        plan.valid = !plan.stages.empty();

        const auto has = [&plan](const char *stage) {
            return std::any_of(plan.stages.begin(), plan.stages.end(),
                               [stage](const ExplainStage &s) { return s.stage == QLatin1String(stage); });
        };
        if (has("COLLSCAN"))
            plan.warnings << QCoreApplication::translate("ExplainPlan", "Collection scan (COLLSCAN): no index was used.");
        if (has("SORT"))
            plan.warnings << QCoreApplication::translate("ExplainPlan", "In-memory sort (SORT): no index provides this sort order.");
        if (plan.hasExecutionStats && plan.nReturned >= 0 &&
            plan.totalDocsExamined > std::max<qint64>(1000, plan.nReturned * 10)) {
            plan.warnings << QCoreApplication::translate("ExplainPlan", "Examined %1 documents to return %2.")
                                 .arg(plan.totalDocsExamined).arg(plan.nReturned);
        }
        return plan;
    }
}
