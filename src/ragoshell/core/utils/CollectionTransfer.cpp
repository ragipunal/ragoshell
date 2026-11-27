#include "ragoshell/core/utils/CollectionTransfer.h"
#include <QCoreApplication>

#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QTextStream>
#include <QTimeZone>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/client.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/options/tls.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"
#include "ragoshell/core/utils/RecordSplitter.h"

namespace RagoShell
{
    namespace CollectionTransfer
    {
        namespace
        {
            QString escapeCsvCell(const QString &value)
            {
                if (value.contains(QLatin1Char(',')) ||
                    value.contains(QLatin1Char('"')) ||
                    value.contains(QLatin1Char('\n')) ||
                    value.contains(QLatin1Char('\r'))) {
                    QString escaped = value;
                    escaped.replace(QLatin1String("\""), QLatin1String("\"\""));
                    return QLatin1Char('"') + escaped + QLatin1Char('"');
                }
                return value;
            }

            QString bsonElementToCsvString(const bsoncxx::document::element &el)
            {
                if (!el)
                    return QString();

                switch (el.type()) {
                case bsoncxx::type::k_null:
                case bsoncxx::type::k_undefined:
                    return QString();
                case bsoncxx::type::k_bool:
                    return el.get_bool().value ? QStringLiteral("true") : QStringLiteral("false");
                case bsoncxx::type::k_int32:
                    return QString::number(el.get_int32().value);
                case bsoncxx::type::k_int64:
                    return QString::number(el.get_int64().value);
                case bsoncxx::type::k_double:
                    return QString::number(el.get_double().value, 'g', 15);
                case bsoncxx::type::k_decimal128:
                    return QString::fromStdString(el.get_decimal128().value.to_string());
                case bsoncxx::type::k_string: {
                    const auto sv = el.get_string().value;
                    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
                }
                case bsoncxx::type::k_oid:
                    return QString::fromStdString(el.get_oid().value.to_string());
                case bsoncxx::type::k_date: {
                    const qint64 ms = el.get_date().to_int64();
                    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
                }
                case bsoncxx::type::k_document:
                    return QString::fromUtf8(
                        bsoncxx::to_json(el.get_document().view(), bsoncxx::ExtendedJsonMode::k_relaxed));
                case bsoncxx::type::k_array: {
                    bsoncxx::builder::basic::document wrapper;
                    wrapper.append(bsoncxx::builder::basic::kvp("v", el.get_array().value));
                    const QString wrapped = QString::fromUtf8(
                        bsoncxx::to_json(wrapper.view(), bsoncxx::ExtendedJsonMode::k_relaxed));
                    const qsizetype colon = wrapped.indexOf(QLatin1Char(':'));
                    const qsizetype lastBrace = wrapped.lastIndexOf(QLatin1Char('}'));
                    if (colon > 0 && lastBrace > colon)
                        return wrapped.mid(colon + 1, lastBrace - colon - 1).trimmed();
                    return wrapped;
                }
                default: {
                    bsoncxx::builder::basic::document wrapper;
                    wrapper.append(bsoncxx::builder::basic::kvp("v", el.get_value()));
                    return QString::fromUtf8(
                        bsoncxx::to_json(wrapper.view(), bsoncxx::ExtendedJsonMode::k_relaxed));
                }
                }
            }

            QString csvHeaderLine(const QStringList &cols)
            {
                QStringList cells;
                cells.reserve(cols.size());
                for (const QString &col : cols)
                    cells.append(escapeCsvCell(col));
                return cells.join(QLatin1Char(',')) + QLatin1Char('\n');
            }

            QString csvRowLine(const bsoncxx::document::view &view, const QStringList &cols)
            {
                QStringList cells;
                cells.reserve(cols.size());
                for (const QString &col : cols) {
                    const std::string key = col.toStdString();
                    cells.append(escapeCsvCell(bsonElementToCsvString(view[key])));
                }
                return cells.join(QLatin1Char(',')) + QLatin1Char('\n');
            }
            QStringList discoverColumns(const std::vector<bsoncxx::document::value> &docs)
            {
                QStringList cols;
                QSet<QString> seen;
                bool hasId = false;

                for (const auto &doc : docs) {
                    for (const auto &el : doc.view()) {
                        const QString key = QString::fromUtf8(
                            el.key().data(), static_cast<qsizetype>(el.key().size()));
                        if (key == QLatin1String("_id")) {
                            hasId = true;
                        } else if (!seen.contains(key)) {
                            seen.insert(key);
                            cols.append(key);
                        }
                    }
                }
                if (hasId)
                    cols.prepend(QStringLiteral("_id"));
                return cols;
            }

            QVector<QStringList> splitCsvRecords(const QString &raw)
            {
                QString text = raw;
                if (text.startsWith(QChar(0xFEFF)))
                    text.remove(0, 1);

                QVector<QStringList> rows;
                QStringList currentRow;
                QString cell;
                bool inQuotes = false;

                for (int i = 0; i < text.size(); ++i) {
                    const QChar ch = text.at(i);
                    if (inQuotes) {
                        if (ch == QLatin1Char('"')) {
                            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                                cell.append(QLatin1Char('"'));
                                ++i;
                            } else {
                                inQuotes = false;
                            }
                        } else {
                            cell.append(ch);
                        }
                    } else {
                        if (ch == QLatin1Char('"') && cell.isEmpty()) {
                            inQuotes = true;
                        } else if (ch == QLatin1Char(',')) {
                            currentRow.append(cell);
                            cell.clear();
                        } else if (ch == QLatin1Char('\r')) {
                            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                                ++i;
                            currentRow.append(cell);
                            cell.clear();
                            rows.append(currentRow);
                            currentRow.clear();
                        } else if (ch == QLatin1Char('\n')) {
                            currentRow.append(cell);
                            cell.clear();
                            rows.append(currentRow);
                            currentRow.clear();
                        } else {
                            cell.append(ch);
                        }
                    }
                }

                if (!cell.isEmpty() || !currentRow.isEmpty()) {
                    currentRow.append(cell);
                    if (!(currentRow.size() == 1 && currentRow.first().isEmpty() && !rows.isEmpty()))
                        rows.append(currentRow);
                }

                return rows;
            }

            void appendCsvCellToBuilder(bsoncxx::builder::basic::document &builder,
                                        const QString &key,
                                        const QString &rawVal)
            {
                namespace basic = bsoncxx::builder::basic;
                const std::string stdKey = key.toStdString();
                const QString trimmed = rawVal.trimmed();

                if (rawVal.isEmpty()) {
                    if (key == QLatin1String("_id"))
                        return; // Let MongoDB auto-generate _id if _id cell is blank
                    builder.append(basic::kvp(stdKey, std::string()));
                    return;
                }

                if (key == QLatin1String("_id")) {
                    static const QRegularExpression reHex24(QStringLiteral("^[0-9a-fA-F]{24}$"));
                    if (reHex24.match(trimmed).hasMatch()) {
                        builder.append(basic::kvp(stdKey, bsoncxx::oid{trimmed.toStdString()}));
                        return;
                    }
                }

                if (trimmed.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
                    builder.append(basic::kvp(stdKey, true));
                    return;
                }
                if (trimmed.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) {
                    builder.append(basic::kvp(stdKey, false));
                    return;
                }
                if (trimmed.compare(QLatin1String("null"), Qt::CaseInsensitive) == 0) {
                    builder.append(basic::kvp(stdKey, bsoncxx::types::b_null{}));
                    return;
                }

                // Preserve leading-zero strings like zip codes ("00123") as strings
                const bool hasLeadingZero =
                    (trimmed.size() > 1 && trimmed.startsWith(QLatin1Char('0')) &&
                     trimmed.at(1).isDigit()) ||
                    (trimmed.size() > 2 && trimmed.startsWith(QLatin1String("-0")) &&
                     trimmed.at(2).isDigit());

                if (!hasLeadingZero) {
                    bool intOk = false;
                    const qint64 iVal = trimmed.toLongLong(&intOk, 10);
                    if (intOk) {
                        if (iVal >= INT32_MIN && iVal <= INT32_MAX)
                            builder.append(basic::kvp(stdKey, static_cast<std::int32_t>(iVal)));
                        else
                            builder.append(basic::kvp(stdKey, static_cast<std::int64_t>(iVal)));
                        return;
                    }

                    bool dblOk = false;
                    const double dVal = trimmed.toDouble(&dblOk);
                    if (dblOk && (trimmed.contains(QLatin1Char('.')) ||
                                  trimmed.contains(QLatin1Char('e'), Qt::CaseInsensitive))) {
                        builder.append(basic::kvp(stdKey, dVal));
                        return;
                    }
                }

                if ((trimmed.startsWith(QLatin1Char('{')) && trimmed.endsWith(QLatin1Char('}'))) ||
                    (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']')))) {
                    try {
                        const QString wrapped = QStringLiteral("{\"v\":") +
                            MongoJsonUtils::normalizeShellJsonToExtendedJson(trimmed) +
                            QLatin1Char('}');
                        auto parsed = bsoncxx::from_json(wrapped.toUtf8().constData());
                        auto el = parsed.view()["v"];
                        if (el) {
                            builder.append(basic::kvp(stdKey, el.get_value()));
                            return;
                        }
                    } catch (const std::exception &) {
                        // Fall through to plain string
                    }
                }

                builder.append(basic::kvp(stdKey, rawVal.toStdString()));
            }
        }

        QString documentsToJsonArray(const std::vector<bsoncxx::document::value> &docs)
        {
            if (docs.empty())
                return QStringLiteral("[]\n");

            QString out;
            out.append(QLatin1String("[\n"));
            for (std::size_t i = 0; i < docs.size(); ++i) {
                const QString relaxed = QString::fromUtf8(
                    bsoncxx::to_json(docs[i].view(), bsoncxx::ExtendedJsonMode::k_relaxed));
                const QString pretty = MongoJsonUtils::formatPrettyJson(relaxed, 2);
                const QStringList lines = pretty.split(QLatin1Char('\n'));
                for (int ln = 0; ln < lines.size(); ++ln) {
                    out.append(QLatin1String("  "));
                    out.append(lines.at(ln));
                    if (ln + 1 < lines.size())
                        out.append(QLatin1Char('\n'));
                }
                if (i + 1 < docs.size())
                    out.append(QLatin1Char(','));
                out.append(QLatin1Char('\n'));
            }
            out.append(QLatin1String("]\n"));
            return out;
        }

        QString documentsToNdjson(const std::vector<bsoncxx::document::value> &docs)
        {
            QString out;
            for (const auto &doc : docs) {
                out.append(QString::fromUtf8(
                    bsoncxx::to_json(doc.view(), bsoncxx::ExtendedJsonMode::k_relaxed)));
                out.append(QLatin1Char('\n'));
            }
            return out;
        }

        QString documentsToCsv(const std::vector<bsoncxx::document::value> &docs,
                               const QStringList &columns)
        {
            const QStringList cols = columns.isEmpty() ? discoverColumns(docs) : columns;
            if (cols.isEmpty())
                return QString();

            QString out = csvHeaderLine(cols);
            for (const auto &doc : docs)
                out.append(csvRowLine(doc.view(), cols));
            return out;
        }

        bool parseJsonOrNdjson(const QString &content,
                               std::vector<bsoncxx::document::value> &outDocs,
                               QString *errorOut)
        {
            QString clean = content;
            if (clean.startsWith(QChar(0xFEFF)))
                clean.remove(0, 1);

            if (clean.trimmed().isEmpty()) {
                outDocs.clear();
                return true;
            }
            if (clean.trimmed() == QLatin1String("[]")) {
                outDocs.clear();
                return true;
            }

            return MongoJsonUtils::parseDocuments(clean, outDocs, errorOut);
        }

        bool parseCsv(const QString &content,
                      std::vector<bsoncxx::document::value> &outDocs,
                      QString *errorOut)
        {
            outDocs.clear();
            const QVector<QStringList> rows = splitCsvRecords(content);
            if (rows.isEmpty()) {
                return true;
            }

            QStringList headers = rows.first();
            for (QString &h : headers)
                h = h.trimmed();

            bool anyHeader = false;
            for (const QString &h : headers) {
                if (!h.isEmpty()) {
                    anyHeader = true;
                    break;
                }
            }
            if (!anyHeader) {
                if (errorOut)
                    *errorOut = QStringLiteral("CSV header row is empty.");
                return false;
            }

            outDocs.reserve(static_cast<std::size_t>(std::max<qsizetype>(0, rows.size() - 1)));
            for (qsizetype r = 1; r < rows.size(); ++r) {
                const QStringList &cells = rows.at(r);
                bool allEmpty = true;
                for (const QString &c : cells) {
                    if (!c.trimmed().isEmpty()) {
                        allEmpty = false;
                        break;
                    }
                }
                if (allEmpty)
                    continue;

                bsoncxx::builder::basic::document docBuilder;
                for (qsizetype c = 0; c < headers.size(); ++c) {
                    const QString &colName = headers.at(c);
                    if (colName.isEmpty())
                        continue;
                    const QString val = (c < cells.size()) ? cells.at(c) : QString();
                    appendCsvCellToBuilder(docBuilder, colName, val);
                }
                outDocs.push_back(docBuilder.extract());
            }

            return true;
        }

        int exportCollectionToFile(const ConnectionSettings &conn,
                                   const QString &dbName,
                                   const QString &collName,
                                   const QString &filePath,
                                   Format format,
                                   const QString &filterJson,
                                   int limit,
                                   const std::atomic<bool> *cancelFlag,
                                   const std::function<void(int)> &onProgress,
                                   QString *errorOut)
        {
            try {
                bsoncxx::document::value filterDoc = bsoncxx::from_json("{}");
                if (!filterJson.trimmed().isEmpty() && filterJson.trimmed() != QLatin1String("{}")) {
                    QString parseErr;
                    if (!MongoJsonUtils::parseSingleDocument(filterJson, filterDoc, &parseErr)) {
                        if (errorOut)
                            *errorOut = QCoreApplication::translate("CollectionTransfer", "Invalid filter JSON: %1").arg(parseErr);
                        return -1;
                    }
                }

                if (conn.hasTransport()) {
                    if (errorOut) {
                        *errorOut = QCoreApplication::translate(
                            "CollectionTransfer",
                            "Collection export is not supported over gateway or demo connection channels.");
                    }
                    return -1;
                }

                QFile file(filePath);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                    if (errorOut)
                        *errorOut = QCoreApplication::translate("CollectionTransfer", "Cannot open output file for writing: %1").arg(filePath);
                    return -1;
                }

                mongocxx::client client{mongocxx::uri{buildMongoUri(conn)}, buildMongoClientOptions(conn)};
                auto coll = client.database(dbName.toStdString()).collection(collName.toStdString());

                mongocxx::options::find findOpts;
                if (limit > 0)
                    findOpts.limit(limit);

                QTextStream out(&file);
                out.setEncoding(QStringConverter::Utf8);

                int count = 0;

                if (format == Format::Csv) {
                    // The header is the union of the field names of all documents, so the collection is
                    // read twice: once for the names (nothing else is kept), once to write the rows.
                    // Memory stays flat however big the collection is.
                    QStringList columns;
                    QSet<QString> seen;
                    bool hasId = false;
                    {
                        auto scan = coll.find(filterDoc.view(), findOpts);
                        for (const auto &doc : scan) {
                            if (cancelFlag && cancelFlag->load()) {
                                if (errorOut)
                                    *errorOut = QCoreApplication::translate("CollectionTransfer", "Export cancelled.");
                                return -1;
                            }
                            for (const auto &el : doc) {
                                const QString key = QString::fromUtf8(el.key().data(), static_cast<qsizetype>(el.key().size()));
                                if (key == QLatin1String("_id"))
                                    hasId = true;
                                else if (!seen.contains(key)) {
                                    seen.insert(key);
                                    columns.append(key);
                                }
                            }
                        }
                    }
                    if (hasId)
                        columns.prepend(QStringLiteral("_id"));
                    if (columns.isEmpty())
                        return 0;

                    out << csvHeaderLine(columns);
                    auto cursor = coll.find(filterDoc.view(), findOpts);
                    for (const auto &doc : cursor) {
                        if (cancelFlag && cancelFlag->load()) {
                            if (errorOut)
                                *errorOut = QCoreApplication::translate("CollectionTransfer", "Export cancelled.");
                            return -1;
                        }
                        out << csvRowLine(doc, columns);
                        ++count;
                        if (onProgress && (count % 250 == 0))
                            onProgress(count);
                    }
                    out.flush();
                    if (onProgress)
                        onProgress(count);
                    return count;
                }

                if (format == Format::JsonArray)
                    out << "[\n";

                auto cursor = coll.find(filterDoc.view(), findOpts);
                bool first = true;
                for (const auto &doc : cursor) {
                    if (cancelFlag && cancelFlag->load()) {
                        if (errorOut)
                            *errorOut = QCoreApplication::translate("CollectionTransfer", "Export cancelled.");
                        return -1;
                    }
                    const QString relaxed = QString::fromUtf8(
                        bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_relaxed));

                    if (format == Format::JsonArray) {
                        if (!first)
                            out << ",\n";
                        first = false;
                        const QString pretty = MongoJsonUtils::formatPrettyJson(relaxed, 2);
                        const QStringList lines = pretty.split(QLatin1Char('\n'));
                        for (int ln = 0; ln < lines.size(); ++ln) {
                            out << "  " << lines.at(ln);
                            if (ln + 1 < lines.size())
                                out << "\n";
                        }
                    } else {
                        out << relaxed << "\n";
                    }

                    ++count;
                    if (onProgress && (count % 250 == 0))
                        onProgress(count);
                }

                if (format == Format::JsonArray)
                    out << (first ? "]\n" : "\n]\n");

                out.flush();
                if (onProgress)
                    onProgress(count);
                return count;
            } catch (const std::exception &ex) {
                if (errorOut)
                    *errorOut = QString::fromUtf8(ex.what());
                return -1;
            }
        }

        namespace
        {
            constexpr qsizetype kReadChunkBytes = 1 << 20;        // 1 MB of file per read
            constexpr std::size_t kImportBatchDocs = 500;
            constexpr qsizetype kImportBatchChars = 4 * 1000 * 1000;

            /// Reads a UTF-8 file a chunk at a time; a character cut by a chunk end is kept for the next one.
            class Utf8ChunkReader
            {
            public:
                explicit Utf8ChunkReader(QFile *file) : _file(file), _decoder(QStringConverter::Utf8) {}
                bool atEnd() const { return _file->atEnd(); }
                QString next()
                {
                    QString text = _decoder(_file->read(kReadChunkBytes));
                    if (_first) {
                        _first = false;
                        if (text.startsWith(QChar(0xFEFF)))
                            text.remove(0, 1);
                    }
                    return text;
                }
            private:
                QFile *_file;
                QStringDecoder _decoder;
                bool _first = true;
            };

            /// Reads the import file in batches of parsed documents and hands each batch to onBatch
            /// (which returns false to stop; it must then have set *errorOut itself). Memory use is
            /// one batch, not the file. Returns false on a read, parse or cancel error.
            bool streamDocuments(const QString &filePath, Format format,
                                 const std::atomic<bool> *cancelFlag,
                                 const std::function<bool(std::vector<bsoncxx::document::value> &)> &onBatch,
                                 QString *errorOut)
            {
                auto fail = [errorOut](const QString &message) {
                    if (errorOut)
                        *errorOut = message;
                    return false;
                };
                const QString cancelled = QCoreApplication::translate("CollectionTransfer", "Import cancelled.");

                QFile file(filePath);
                if (!file.open(QIODevice::ReadOnly))
                    return fail(QCoreApplication::translate("CollectionTransfer", "Cannot open input file for reading: %1").arg(filePath));

                const bool csv = format == Format::Csv;
                JsonDocumentSplitter jsonSplitter;
                CsvRecordSplitter csvSplitter;
                QString csvHeader;
                QStringList pending;
                qsizetype pendingChars = 0;

                auto flush = [&]() -> bool {
                    if (pending.isEmpty())
                        return true;
                    std::vector<bsoncxx::document::value> docs;
                    QString parseErr;
                    const QString text = csv ? csvHeader + QLatin1Char('\n') + pending.join(QLatin1Char('\n'))
                                             : pending.join(QLatin1Char('\n'));
                    pending.clear();
                    pendingChars = 0;
                    const bool parsed = csv ? parseCsv(text, docs, &parseErr)
                                            : parseJsonOrNdjson(text, docs, &parseErr);
                    if (!parsed)
                        return fail(parseErr);
                    return docs.empty() || onBatch(docs);
                };

                auto add = [&](const QStringList &pieces) -> bool {
                    for (const QString &piece : pieces) {
                        if (csv) {
                            if (piece.trimmed().isEmpty())
                                continue;
                            if (csvHeader.isNull()) {
                                csvHeader = piece;
                                continue;
                            }
                        }
                        pending.append(piece);
                        pendingChars += piece.size();
                        if (pending.size() >= static_cast<qsizetype>(kImportBatchDocs) || pendingChars >= kImportBatchChars) {
                            if (cancelFlag && cancelFlag->load())
                                return fail(cancelled);
                            if (!flush())
                                return false;
                        }
                    }
                    return true;
                };

                Utf8ChunkReader reader(&file);
                while (!reader.atEnd()) {
                    if (cancelFlag && cancelFlag->load())
                        return fail(cancelled);
                    const QString chunk = reader.next();
                    if (!add(csv ? csvSplitter.feed(chunk) : jsonSplitter.feed(chunk)))
                        return false;
                }
                if (csv) {
                    if (!add(csvSplitter.finish()))
                        return false;
                } else {
                    QString splitError;
                    if (!jsonSplitter.finish(&splitError))
                        return fail(splitError);
                }
                if (cancelFlag && cancelFlag->load())
                    return fail(cancelled);
                return flush();
            }
        }

        bool forEachDocumentBatch(const QString &filePath, Format format,
                                  const std::atomic<bool> *cancelFlag,
                                  const std::function<bool(std::vector<bsoncxx::document::value> &)> &onBatch,
                                  QString *errorOut)
        {
            return streamDocuments(filePath, format, cancelFlag, onBatch, errorOut);
        }

        int importFileToCollection(const ConnectionSettings &conn,
                                   const QString &dbName,
                                   const QString &collName,
                                   const QString &filePath,
                                   Format format,
                                   bool dropBeforeImport,
                                   bool stopOnError,
                                   const std::atomic<bool> *cancelFlag,
                                   const std::function<void(int)> &onProgress,
                                   QString *errorOut)
        {
            try {
                if (conn.hasTransport()) {
                    if (errorOut) {
                        *errorOut = QCoreApplication::translate(
                            "CollectionTransfer",
                            "Collection import is not supported over gateway or demo connection channels.");
                    }
                    return -1;
                }

                // Replacing a collection is destructive: read the whole file once first (batch by batch,
                // nothing kept) so that a broken file is reported before anything is dropped.
                if (dropBeforeImport
                    && !streamDocuments(filePath, format, cancelFlag,
                                        [](std::vector<bsoncxx::document::value> &) { return true; }, errorOut))
                    return -1;

                mongocxx::client client{mongocxx::uri{buildMongoUri(conn)}, buildMongoClientOptions(conn)};
                auto coll = client.database(dbName.toStdString()).collection(collName.toStdString());

                if (dropBeforeImport)
                    coll.drop();

                int inserted = 0;
                mongocxx::options::insert insertOpts;
                insertOpts.ordered(stopOnError);

                const bool ok = streamDocuments(filePath, format, cancelFlag,
                    [&](std::vector<bsoncxx::document::value> &docs) {
                        std::vector<bsoncxx::document::view> batch;
                        batch.reserve(docs.size());
                        for (const auto &doc : docs)
                            batch.push_back(doc.view());
                        try {
                            auto res = coll.insert_many(batch, insertOpts);
                            inserted += res ? res->inserted_count() : static_cast<int>(batch.size());
                        } catch (const std::exception &) {
                            if (stopOnError)
                                throw;
                            // Unordered continue-on-error: count non-duplicate inserts best-effort
                            inserted += static_cast<int>(batch.size());
                        }
                        if (onProgress)
                            onProgress(inserted);
                        return true;
                    }, errorOut);
                return ok ? inserted : -1;
            } catch (const std::exception &ex) {
                if (errorOut)
                    *errorOut = QString::fromUtf8(ex.what());
                return -1;
            }
        }
    }
}
