#pragma once

#include <QString>
#include <QStringList>

namespace RagoShell
{
    /**
     * @brief Cuts a stream of text into one piece per document, a chunk at a
     *        time, so a big import file never has to be in memory as a whole.
     *
     * Accepts what the importer accepts: a JSON array of objects, NDJSON, and
     * objects one after another (mongo shell style). It finds the top-level
     * `{ ... }` pieces by matching braces outside strings; everything between
     * them (brackets, commas, whitespace, a BOM) is skipped. Each piece is
     * handed to the normal parser, so the interpretation of ObjectId(...),
     * ISODate(...) and so on stays in one place.
     */
    class JsonDocumentSplitter
    {
    public:
        /// Adds text; returns the documents that were completed by it.
        QStringList feed(const QString &chunk);

        /// True when the text ended inside a document; *error says so.
        bool finish(QString *error = nullptr);

        /// Characters of a document that is still open.
        qsizetype pendingChars() const { return _current.size(); }

    private:
        QString _current;
        int _depth = 0;
        QChar _quote;            ///< The quote character of the string being read, or null.
        bool _escaped = false;
    };

    /**
     * @brief The same for RFC 4180 CSV: cuts records at line ends that are not
     *        inside a quoted cell. The text of a record is returned as it was
     *        (quotes and embedded line breaks included), without its line end.
     */
    class CsvRecordSplitter
    {
    public:
        QStringList feed(const QString &chunk);
        /// The last record when the text does not end with a line break.
        QStringList finish();

    private:
        void take(QChar ch, QStringList *out);

        QString _record;
        bool _inQuotes = false;
        bool _atCellStart = true;
        bool _quotePending = false;   ///< A quote inside quotes: "" or the end of the cell.
        bool _crPending = false;
    };
}
