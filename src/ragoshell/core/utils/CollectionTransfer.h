#pragma once

#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <vector>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>

namespace RagoShell
{
    class ConnectionSettings;

    namespace CollectionTransfer
    {
        enum class Format {
            JsonArray = 0,
            Ndjson = 1,
            Csv = 2
        };

        /// Format a vector of BSON documents as a pretty JSON array.
        QString documentsToJsonArray(const std::vector<bsoncxx::document::value> &docs);

        /// Format a vector of BSON documents as NDJSON (one relaxed EJSON document per line).
        QString documentsToNdjson(const std::vector<bsoncxx::document::value> &docs);

        /// Format a vector of BSON documents as RFC 4180 CSV.
        /// When 'columns' is empty, top-level column names are discovered across 'docs' (_id first).
        QString documentsToCsv(const std::vector<bsoncxx::document::value> &docs,
                               const QStringList &columns = QStringList());

        /// Parse JSON Array, NDJSON, or concatenated Mongo-Shell JSON objects into BSON documents.
        bool parseJsonOrNdjson(const QString &content,
                               std::vector<bsoncxx::document::value> &outDocs,
                               QString *errorOut = nullptr);

        /// Parse RFC 4180 CSV (first row = header names) into BSON documents.
        bool parseCsv(const QString &content,
                      std::vector<bsoncxx::document::value> &outDocs,
                      QString *errorOut = nullptr);

        /// Reads an import file (JSON array, NDJSON, objects one after another, or CSV) a chunk at a
        /// time and hands the documents to onBatch in batches of at most 500, so the memory used is one
        /// batch whatever the size of the file. onBatch returns false to stop (it must then have set
        /// *errorOut itself). Returns false on a read, parse or cancel error.
        bool forEachDocumentBatch(const QString &filePath, Format format,
                                  const std::atomic<bool> *cancelFlag,
                                  const std::function<bool(std::vector<bsoncxx::document::value> &)> &onBatch,
                                  QString *errorOut = nullptr);

        /// Stream export a collection directly via mongocxx (no external process required).
        /// Returns number of exported documents, or -1 on error / cancellation.
        int exportCollectionToFile(const ConnectionSettings &conn,
                                   const QString &dbName,
                                   const QString &collName,
                                   const QString &filePath,
                                   Format format,
                                   const QString &filterJson = QString(),
                                   int limit = 0,
                                   const std::atomic<bool> *cancelFlag = nullptr,
                                   const std::function<void(int)> &onProgress = nullptr,
                                   QString *errorOut = nullptr);

        /// Import JSON Array, NDJSON, or CSV file directly into a collection via mongocxx.
        /// Returns number of inserted documents, or -1 on error / cancellation.
        int importFileToCollection(const ConnectionSettings &conn,
                                   const QString &dbName,
                                   const QString &collName,
                                   const QString &filePath,
                                   Format format,
                                   bool dropBeforeImport = false,
                                   bool stopOnError = false,
                                   const std::atomic<bool> *cancelFlag = nullptr,
                                   const std::function<void(int)> &onProgress = nullptr,
                                   QString *errorOut = nullptr);
    }
}
