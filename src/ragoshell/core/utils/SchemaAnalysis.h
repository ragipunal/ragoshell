#pragma once

#include <map>
#include <vector>

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <bsoncxx/document/view.hpp>

#include "ragoshell/core/Enums.h"

namespace RagoShell
{
    class ConnectionSettings;
    class IMongoTransport;

    /**
     * @brief Statistics for one field path across the sampled documents.
     * Fields of embedded documents and of documents inside arrays use dotted
     * paths ("address.city", "grades.score").
     */
    struct SchemaField
    {
        QString path;
        QString name;                 ///< last path segment
        int depth = 0;
        qint64 presentCount = 0;      ///< documents containing the path
        double presence = 0.0;        ///< presentCount / sampled documents
        std::vector<std::pair<QString, qint64>> types;      ///< by count, descending
        std::vector<std::pair<QString, qint64>> itemTypes;  ///< array element types
        std::vector<std::pair<QString, qint64>> topValues;  ///< most frequent scalar values
        bool valuesTruncated = false; ///< more distinct values than tracked
        QString minValue;             ///< numbers and dates only
        QString maxValue;
        int distinctValues = 0;       ///< distinct scalar values seen (capped at kMaxTrackedValues)
        /// A string field with few, repeating values (status, country, ...):
        /// topValues then lists all of them, up to kMaxCategoryValues.
        bool categorical = false;
        QString letterCase;           ///< "upper" / "lower" when every string value is; else empty
        /// Seen only in the newest documents, not in the random sample;
        /// presence is then its share among the newest documents.
        bool onlyRecent = false;
    };

    struct SchemaResult
    {
        bool ok = false;
        QString error;
        qint64 sampledDocuments = 0;
        qint64 recentDocuments = 0;       ///< of sampledDocuments, the newest by _id
        qint64 totalDocuments = -1;       ///< estimated collection size; -1 = unknown
        bool fullScan = false;            ///< every document was analyzed
        QStringList indexes;              ///< index keys as JSON, without the _id index
        QDateTime analyzedAt;             ///< UTC
        std::vector<SchemaField> fields;  ///< sorted by path, parents before children
    };

    /**
     * @brief Accumulates field statistics document by document.
     */
    class SchemaAccumulator
    {
    public:
        static constexpr int kMaxTrackedValues = 50;
        static constexpr int kTopValues = 5;
        static constexpr int kMaxCategoryValues = 25;

        explicit SchemaAccumulator(UUIDEncoding uuidEncoding = DefaultEncoding,
                                   SupportedTimes timeZone = Utc);

        /// recent: the document comes from the newest-by-_id part of the
        /// sample. Statistics come from the other (random) documents; recent
        /// ones only add the fields the random ones lack.
        void add(bsoncxx::document::view doc, bool recent = false);
        SchemaResult result() const;

    private:
        struct FieldStats
        {
            int depth = 0;
            qint64 presentCount = 0;
            qint64 lastDocument = -1;  ///< counts presence once per document
            std::map<QString, qint64> types;
            std::map<QString, qint64> itemTypes;
            std::map<QString, qint64> values;
            bool valuesTruncated = false;
            bool hasRange = false;
            int rangeKind = 0;          ///< numbers or dates, never both
            bool rangeConflict = false;
            double minNumber = 0;
            double maxNumber = 0;
            QString minText;
            QString maxText;
        };

        void walk(bsoncxx::document::view doc, const QString &prefix, int depth,
                  std::map<QString, FieldStats> &fields, qint64 document);
        void record(FieldStats &stats, const bsoncxx::document::element &el);
        static SchemaField fieldFrom(const QString &path, const FieldStats &stats, qint64 documents);

        UUIDEncoding _uuidEncoding;
        SupportedTimes _timeZone;
        qint64 _documents = 0;         ///< random (or all) documents
        qint64 _recentDocuments = 0;
        std::map<QString, FieldStats> _fields;
        std::map<QString, FieldStats> _recentFields;
    };

    namespace SchemaAnalysis
    {
        /// Collections up to this size are analyzed completely.
        constexpr qint64 kFullScanLimit = 5000;
        /// Share of a sample taken from the newest documents (ObjectId _id
        /// only), so fields added lately are not missed.
        constexpr double kRecentShare = 0.3;

        /**
         * @brief Analyzes <db>.<collection> with a standalone client: every
         * document when the collection has at most kFullScanLimit, otherwise
         * up to 'sampleSize' documents, kRecentShare of them the newest by
         * ObjectId _id and the rest random ($sample). Also reads the
         * estimated size and the index keys. Blocking: call from a worker
         * thread.
         */
        SchemaResult analyzeCollection(const ConnectionSettings &connection,
                                       const QString &database, const QString &collection,
                                       int sampleSize, int maxTimeMs,
                                       UUIDEncoding uuidEncoding, SupportedTimes timeZone);
        SchemaResult analyzeCollection(IMongoTransport &transport,
                                       const QString &database, const QString &collection,
                                       int sampleSize, int maxTimeMs,
                                       UUIDEncoding uuidEncoding = DefaultEncoding,
                                       SupportedTimes timeZone = Utc);

        /// Estimated document count (collection metadata, cheap); -1 on error.
        qint64 estimatedDocumentCount(const ConnectionSettings &connection, const QString &database,
                                      const QString &collection, int maxTimeMs);
        qint64 estimatedDocumentCount(IMongoTransport &transport, const QString &database,
                                      const QString &collection, int maxTimeMs);

        struct CollectionEntry
        {
            QString name;
            QString type;            ///< "collection", "view", "timeseries"
            qint64 documents = -1;   ///< estimated; -1 = unknown (views)
        };

        struct CollectionList
        {
            bool ok = false;
            QString error;
            std::vector<CollectionEntry> collections;
        };

        /// The collections of a database with their type and estimated size
        /// (listCollections plus one count per collection). Blocking: call
        /// from a worker thread.
        CollectionList listCollections(const ConnectionSettings &connection, const QString &database,
                                       int maxTimeMs);
        CollectionList listCollections(IMongoTransport &transport, const QString &database,
                                       int maxTimeMs);
    }
}
