#pragma once

#include <string>

#include <bsoncxx/document/view.hpp>

namespace RagoShell
{
    /**
     * @brief The script that fetches one page of an aggregate again (next / previous page, Refresh).
     *
     * The page is the recorded pipeline plus `{$skip}` and `{$limit}`. The stages and the options are written as
     * canonical Extended JSON and read back by the shell's own `EJSON.parse`, so an ObjectId, a date, a regular
     * expression or a 64-bit integer in the pipeline reaches the server as the type it was. (`bsoncxx::to_json` in its
     * default mode writes `{"$oid": ...}`, which the shell takes for a plain document: the server then refuses the
     * pipeline with "unknown operator: $oid".)
     *
     * @param pipeline the recorded pipeline: a document whose elements are the stages (non-document elements stop it)
     * @param options  the recorded aggregate options ({} when there are none)
     */
    std::string aggregatePageScript(const std::string &collectionName,
                                    const bsoncxx::document::view &pipeline,
                                    const bsoncxx::document::view &options,
                                    int skip, int batchSize);
}
