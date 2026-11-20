#include "ragoshell/core/utils/RecordSplitter.h"

namespace RagoShell
{
    QStringList JsonDocumentSplitter::feed(const QString &chunk)
    {
        QStringList out;
        for (const QChar ch : chunk) {
            if (_depth == 0) {
                if (ch == QLatin1Char('{')) {
                    _depth = 1;
                    _current = ch;
                }
                continue;                         // brackets, commas, spaces, BOM between documents
            }

            _current.append(ch);
            if (!_quote.isNull()) {
                if (_escaped)
                    _escaped = false;
                else if (ch == QLatin1Char('\\'))
                    _escaped = true;
                else if (ch == _quote)
                    _quote = QChar();
                continue;
            }
            if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
                _quote = ch;
            } else if (ch == QLatin1Char('{')) {
                ++_depth;
            } else if (ch == QLatin1Char('}')) {
                if (--_depth == 0) {
                    out.append(_current);
                    _current.clear();
                }
            }
        }
        return out;
    }

    bool JsonDocumentSplitter::finish(QString *error)
    {
        if (_depth == 0)
            return true;
        if (error)
            *error = QStringLiteral("Unexpected end of input: a document is not closed.");
        return false;
    }

    void CsvRecordSplitter::take(QChar ch, QStringList *out)
    {
        if (_crPending) {
            _crPending = false;
            if (ch == QLatin1Char('\n'))
                return;                           // the \n of a \r\n already ended the record
        }
        if (_quotePending) {
            _quotePending = false;
            if (ch == QLatin1Char('"')) {         // "" inside quotes: an escaped quote
                _record.append(ch);
                return;
            }
            _inQuotes = false;                    // the quote ended the cell; handle ch normally
        }

        if (_inQuotes) {
            _record.append(ch);
            if (ch == QLatin1Char('"'))
                _quotePending = true;
            return;
        }

        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            if (ch == QLatin1Char('\r'))
                _crPending = true;
            out->append(_record);
            _record.clear();
            _atCellStart = true;
            return;
        }
        _record.append(ch);
        if (ch == QLatin1Char('"') && _atCellStart) {
            _inQuotes = true;
            _atCellStart = false;
        } else {
            _atCellStart = (ch == QLatin1Char(','));
        }
    }

    QStringList CsvRecordSplitter::feed(const QString &chunk)
    {
        QStringList out;
        for (const QChar ch : chunk)
            take(ch, &out);
        return out;
    }

    QStringList CsvRecordSplitter::finish()
    {
        QStringList out;
        _quotePending = false;
        _crPending = false;
        if (!_record.isEmpty())
            out.append(_record);
        _record.clear();
        _inQuotes = false;
        _atCellStart = true;
        return out;
    }
}
