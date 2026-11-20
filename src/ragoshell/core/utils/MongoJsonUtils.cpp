#include "ragoshell/core/utils/MongoJsonUtils.h"
#include <QCoreApplication>

#include <algorithm>

#include <QByteArray>
#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUuid>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/HexUtils.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{
    namespace
    {
        // Converts JS regex literals in value position (/^ab/i after ':', ',', '[' or '(')
        // to {"$regularExpression": ...}. Runs before comment stripping so a '//' inside a
        // regex is not taken for a comment; skips strings and comments itself.
        QString convertRegexLiterals(const QString &src)
        {
            QString out;
            out.reserve(src.size());
            const int n = src.size();
            QChar lastSignificant;   // last non-space char outside strings/comments
            int i = 0;
            while (i < n) {
                const QChar c = src[i];
                if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                    const int start = i++;
                    while (i < n && src[i] != c) {
                        if (src[i] == QLatin1Char('\\'))
                            ++i;
                        ++i;
                    }
                    i = std::min(i + 1, n);
                    out += src.mid(start, i - start);
                    lastSignificant = c;
                    continue;
                }
                if (c == QLatin1Char('/') && i + 1 < n &&
                    (src[i + 1] == QLatin1Char('/') || src[i + 1] == QLatin1Char('*'))) {
                    // Comment: copy through unchanged for the comment stripper.
                    const bool line = src[i + 1] == QLatin1Char('/');
                    int end = line ? src.indexOf(QLatin1Char('\n'), i) : src.indexOf(QLatin1String("*/"), i + 2);
                    end = end < 0 ? n : (line ? end : end + 2);
                    out += src.mid(i, end - i);
                    i = end;
                    continue;
                }
                const bool valuePosition = lastSignificant.isNull() ||
                    QStringLiteral(":,[(").contains(lastSignificant);
                if (c == QLatin1Char('/') && valuePosition) {
                    QString pattern;
                    bool inClass = false;
                    int j = i + 1;
                    for (; j < n; ++j) {
                        const QChar r = src[j];
                        if (r == QLatin1Char('\\') && j + 1 < n) {
                            pattern += r;
                            pattern += src[++j];
                            continue;
                        }
                        if (r == QLatin1Char('\n'))
                            break;
                        if (r == QLatin1Char('['))
                            inClass = true;
                        else if (r == QLatin1Char(']'))
                            inClass = false;
                        else if (r == QLatin1Char('/') && !inClass)
                            break;
                        pattern += r;
                    }
                    if (j < n && src[j] == QLatin1Char('/') && !pattern.isEmpty()) {
                        ++j;
                        QString flags;
                        while (j < n && src[j].isLetter())
                            flags += src[j++];
                        std::sort(flags.begin(), flags.end());   // Extended JSON wants sorted options
                        QString escaped = pattern;
                        escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
                        escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
                        out += QStringLiteral("{\"$regularExpression\":{\"pattern\":\"%1\",\"options\":\"%2\"}}")
                                   .arg(escaped, flags);
                        lastSignificant = QLatin1Char('}');
                        i = j;
                        continue;
                    }
                }
                out += c;
                if (!c.isSpace())
                    lastSignificant = c;
                ++i;
            }
            return out;
        }

        // Strip JS comments and convert single-quoted strings to JSON double-quoted strings.
        QString stripCommentsAndSingleQuotes(const QString &src)
        {
            QString out;
            out.reserve(src.size() + 32);
            const int n = src.size();
            for (int i = 0; i < n; ++i) {
                const QChar ch = src[i];
                const QChar next = (i + 1 < n) ? src[i + 1] : QChar();

                // Line comment //
                if (ch == QLatin1Char('/') && next == QLatin1Char('/')) {
                    i += 2;
                    while (i < n && src[i] != QLatin1Char('\n'))
                        ++i;
                    if (i < n)
                        out += QLatin1Char('\n');
                    continue;
                }
                // Block comment /* ... */
                if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
                    i += 2;
                    while (i + 1 < n && !(src[i] == QLatin1Char('*') && src[i + 1] == QLatin1Char('/'))) {
                        if (src[i] == QLatin1Char('\n'))
                            out += QLatin1Char('\n');
                        ++i;
                    }
                    ++i; // skip '/'
                    continue;
                }
                // Double-quoted string: copy verbatim preserving escapes
                if (ch == QLatin1Char('"')) {
                    out += ch;
                    ++i;
                    while (i < n) {
                        out += src[i];
                        if (src[i] == QLatin1Char('\\') && i + 1 < n) {
                            ++i;
                            out += src[i];
                        } else if (src[i] == QLatin1Char('"')) {
                            break;
                        }
                        ++i;
                    }
                    continue;
                }
                // Single-quoted string: convert to valid JSON double-quoted string
                if (ch == QLatin1Char('\'')) {
                    out += QLatin1Char('"');
                    ++i;
                    while (i < n && src[i] != QLatin1Char('\'')) {
                        if (src[i] == QLatin1Char('\\') && i + 1 < n) {
                            ++i;
                            if (src[i] == QLatin1Char('\''))
                                out += QLatin1Char('\'');
                            else {
                                out += QLatin1Char('\\');
                                out += src[i];
                            }
                        } else if (src[i] == QLatin1Char('"')) {
                            out += QStringLiteral("\\\"");
                        } else {
                            out += src[i];
                        }
                        ++i;
                    }
                    out += QLatin1Char('"');
                    continue;
                }
                out += ch;
            }
            return out;
        }

        // Apply regex replacements only outside double-quoted strings.
        QString transformOutsideStrings(const QString &src,
                                        const std::function<QString(const QString &)> &fn)
        {
            QString out;
            out.reserve(src.size() + 64);
            int segStart = 0;
            const int n = src.size();
            for (int i = 0; i < n; ++i) {
                if (src[i] == QLatin1Char('"')) {
                    if (i > segStart)
                        out += fn(src.mid(segStart, i - segStart));
                    const int strStart = i;
                    ++i;
                    while (i < n) {
                        if (src[i] == QLatin1Char('\\') && i + 1 < n) {
                            ++i;
                        } else if (src[i] == QLatin1Char('"')) {
                            break;
                        }
                        ++i;
                    }
                    const int strLen = std::min(n, i + 1) - strStart;
                    out += src.mid(strStart, strLen);
                    segStart = i + 1;
                }
            }
            if (segStart < n)
                out += fn(src.mid(segStart));
            return out;
        }

        // Split top-level {...} blocks or top-level [...] array elements.
        bool extractTopLevelBlocks(const QString &normalized,
                                   std::vector<QString> &outBlocks,
                                   QString &outError)
        {
            const QString trimmed = normalized.trimmed();
            if (trimmed.isEmpty()) {
                outError = QStringLiteral("JSON input is empty.");
                return false;
            }

            // If wrapped in a top-level array `[ ... ]`, unwrap the outer brackets
            QString work = trimmed;
            if (work.startsWith(QLatin1Char('['))) {
                if (!work.endsWith(QLatin1Char(']'))) {
                    outError = QCoreApplication::translate("MongoJsonUtils", "Unclosed top-level JSON array '['.");
                    return false;
                }
                work = work.mid(1, work.size() - 2).trimmed();
                if (work.isEmpty())
                    return true; // empty array [] -> 0 docs
            }

            const int n = work.size();
            int i = 0;
            while (i < n) {
                while (i < n && (work[i].isSpace() || work[i] == QLatin1Char(',')))
                    ++i;
                if (i >= n)
                    break;
                if (work[i] != QLatin1Char('{')) {
                    outError = QCoreApplication::translate("MongoJsonUtils", "Expected '{' at start of BSON document near: %1")
                                   .arg(work.mid(i, 32));
                    return false;
                }
                const int blockStart = i;
                int depth = 0;
                bool inStr = false;
                for (; i < n; ++i) {
                    const QChar c = work[i];
                    if (inStr) {
                        if (c == QLatin1Char('\\') && i + 1 < n)
                            ++i;
                        else if (c == QLatin1Char('"'))
                            inStr = false;
                        continue;
                    }
                    if (c == QLatin1Char('"')) {
                        inStr = true;
                    } else if (c == QLatin1Char('{')) {
                        ++depth;
                    } else if (c == QLatin1Char('}')) {
                        --depth;
                        if (depth == 0) {
                            ++i;
                            break;
                        }
                    }
                }
                if (depth != 0) {
                    outError = QCoreApplication::translate("MongoJsonUtils", "Unclosed '{' in JSON document.");
                    return false;
                }
                outBlocks.push_back(work.mid(blockStart, i - blockStart));
            }
            return true;
        }

        bool appendParsedScalar(bsoncxx::builder::basic::document &builder,
                                const std::string &key,
                                const QString &rawInput,
                                const QString &typeHint,
                                QString &outError)
        {
            using bsoncxx::builder::basic::kvp;

            // Explicit String cells keep exact text without coercing "null"/"true"/"false"
            // or stripping literal quote/backslash characters that are part of the string.
            if (typeHint == QLatin1String("String")) {
                builder.append(kvp(key, rawInput.toStdString()));
                return true;
            }

            const QString trimmed = rawInput.trimmed();

            if (typeHint == QLatin1String("Null") ||
                (typeHint.isEmpty() && trimmed == QLatin1String("null"))) {
                builder.append(kvp(key, bsoncxx::types::b_null{}));
                return true;
            }
            if (typeHint == QLatin1String("Boolean") ||
                (typeHint.isEmpty() && (trimmed == QLatin1String("true") || trimmed == QLatin1String("false")))) {
                if (trimmed.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 || trimmed == QLatin1String("1")) {
                    builder.append(kvp(key, true));
                    return true;
                }
                if (trimmed.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0 || trimmed == QLatin1String("0")) {
                    builder.append(kvp(key, false));
                    return true;
                }
                outError = QCoreApplication::translate("MongoJsonUtils", "Boolean value must be 'true' or 'false'.");
                return false;
            }
            if (typeHint == QLatin1String("Int32")) {
                QString clean = trimmed;
                static const QRegularExpression reNumInt(QStringLiteral("^NumberInt\\(\\s*\"?([+-]?\\d+)\"?\\s*\\)$"));
                const auto m = reNumInt.match(clean);
                if (m.hasMatch())
                    clean = m.captured(1);
                bool ok = false;
                const int val = clean.toInt(&ok);
                if (!ok) {
                    outError = QCoreApplication::translate("MongoJsonUtils", "Invalid Int32 value: '%1'").arg(rawInput);
                    return false;
                }
                builder.append(kvp(key, static_cast<int32_t>(val)));
                return true;
            }
            if (typeHint == QLatin1String("Int64")) {
                QString clean = trimmed;
                static const QRegularExpression reNumLong(QStringLiteral("^NumberLong\\(\\s*\"?([+-]?\\d+)\"?\\s*\\)$"));
                const auto m = reNumLong.match(clean);
                if (m.hasMatch())
                    clean = m.captured(1);
                bool ok = false;
                const qlonglong val = clean.toLongLong(&ok);
                if (!ok) {
                    outError = QCoreApplication::translate("MongoJsonUtils", "Invalid Int64 value: '%1'").arg(rawInput);
                    return false;
                }
                builder.append(kvp(key, static_cast<int64_t>(val)));
                return true;
            }
            if (typeHint == QLatin1String("Double")) {
                bool ok = false;
                const double val = trimmed.toDouble(&ok);
                if (!ok) {
                    outError = QCoreApplication::translate("MongoJsonUtils", "Invalid Double value: '%1'").arg(rawInput);
                    return false;
                }
                builder.append(kvp(key, val));
                return true;
            }

            // Newly created cell without a typeHint: strip optional surrounding quotes.
            QString strVal = rawInput;
            if (strVal.size() >= 2 &&
                ((strVal.startsWith(QLatin1Char('"')) && strVal.endsWith(QLatin1Char('"'))) ||
                 (strVal.startsWith(QLatin1Char('\'')) && strVal.endsWith(QLatin1Char('\''))))) {
                strVal = strVal.mid(1, strVal.size() - 2);
                strVal.replace(QStringLiteral("\\\""), QStringLiteral("\""));
                strVal.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
                strVal.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
                strVal.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            }
            builder.append(kvp(key, strVal.toStdString()));
            return true;
        }

        bool rebuildDocUpdatingPath(bsoncxx::document::view doc,
                                    const QStringList &pathParts,
                                    int depth,
                                    const QString &rawInput,
                                    const QString &typeHint,
                                    bsoncxx::document::value &outDoc,
                                    QString &outError);

        bool rebuildArrayUpdatingPath(bsoncxx::array::view arr,
                                      const QStringList &pathParts,
                                      int depth,
                                      const QString &rawInput,
                                      const QString &typeHint,
                                      bsoncxx::array::value &outArr,
                                      QString &outError)
        {
            bool idxOk = false;
            const int targetIdx = pathParts[depth].toInt(&idxOk);
            if (!idxOk || targetIdx < 0) {
                outError = QCoreApplication::translate("MongoJsonUtils", "Invalid array index '%1'").arg(pathParts[depth]);
                return false;
            }

            bsoncxx::builder::basic::array arrBuilder;
            int idx = 0;
            bool found = false;
            for (const auto &elem : arr) {
                if (idx == targetIdx) {
                    found = true;
                    if (depth == pathParts.size() - 1) {
                        bsoncxx::builder::basic::document tmp;
                        if (!appendParsedScalar(tmp, "v", rawInput, typeHint, outError))
                            return false;
                        arrBuilder.append(tmp.view()["v"].get_value());
                    } else if (elem.type() == bsoncxx::type::k_document) {
                        bsoncxx::document::value subDoc(bsoncxx::builder::basic::make_document());
                        if (!rebuildDocUpdatingPath(elem.get_document().view(), pathParts, depth + 1,
                                                    rawInput, typeHint, subDoc, outError))
                            return false;
                        arrBuilder.append(subDoc);
                    } else if (elem.type() == bsoncxx::type::k_array) {
                        bsoncxx::array::value subArr(bsoncxx::builder::basic::array{}.extract());
                        if (!rebuildArrayUpdatingPath(elem.get_array().value, pathParts, depth + 1,
                                                      rawInput, typeHint, subArr, outError))
                            return false;
                        arrBuilder.append(subArr);
                    } else {
                        outError = QCoreApplication::translate("MongoJsonUtils", "Cannot traverse scalar array element.");
                        return false;
                    }
                } else {
                    arrBuilder.append(elem.get_value());
                }
                ++idx;
            }
            if (!found) {
                outError = QCoreApplication::translate("MongoJsonUtils", "Array index %1 out of range.").arg(targetIdx);
                return false;
            }
            outArr = arrBuilder.extract();
            return true;
        }

        bool rebuildDocUpdatingPath(bsoncxx::document::view doc,
                                    const QStringList &pathParts,
                                    int depth,
                                    const QString &rawInput,
                                    const QString &typeHint,
                                    bsoncxx::document::value &outDoc,
                                    QString &outError)
        {
            using bsoncxx::builder::basic::kvp;
            const std::string targetKey = pathParts[depth].toStdString();
            bsoncxx::builder::basic::document builder;
            bool found = false;

            for (const auto &elem : doc) {
                const std::string k(elem.key().data(), elem.key().size());
                if (k == targetKey) {
                    found = true;
                    if (depth == pathParts.size() - 1) {
                        if (!appendParsedScalar(builder, k, rawInput, typeHint, outError))
                            return false;
                    } else if (elem.type() == bsoncxx::type::k_document) {
                        bsoncxx::document::value subDoc(bsoncxx::builder::basic::make_document());
                        if (!rebuildDocUpdatingPath(elem.get_document().view(), pathParts, depth + 1,
                                                    rawInput, typeHint, subDoc, outError))
                            return false;
                        builder.append(kvp(k, subDoc));
                    } else if (elem.type() == bsoncxx::type::k_array) {
                        bsoncxx::array::value subArr(bsoncxx::builder::basic::array{}.extract());
                        if (!rebuildArrayUpdatingPath(elem.get_array().value, pathParts, depth + 1,
                                                      rawInput, typeHint, subArr, outError))
                            return false;
                        builder.append(kvp(k, subArr));
                    } else {
                        outError = QCoreApplication::translate("MongoJsonUtils", "Field '%1' is not an object or array.").arg(pathParts[depth]);
                        return false;
                    }
                } else {
                    builder.append(kvp(k, elem.get_value()));
                }
            }

            if (!found) {
                // Allow adding a missing top-level column in Table View
                if (depth == pathParts.size() - 1) {
                    if (!appendParsedScalar(builder, targetKey, rawInput, typeHint, outError))
                        return false;
                } else {
                    outError = QCoreApplication::translate("MongoJsonUtils", "Field '%1' not found.").arg(pathParts[depth]);
                    return false;
                }
            }

            outDoc = builder.extract();
            return true;
        }

        bool isShellIdentChar(QChar ch)
        {
            return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('$');
        }

        // Single-pass scanner that expands Mongo Shell value literals and constructors
        // ONLY when they appear outside JSON double-quoted string literals.
        QString replaceShellConstructorsOutsideStrings(const QString &src)
        {
            static const QRegularExpression reOidArg(QStringLiteral("^\"([0-9a-fA-F]{24})\"$"));
            static const QRegularExpression reStrArg(QStringLiteral("^\"((?:[^\"\\\\]|\\\\.)*)\"$"));
            static const QRegularExpression reIntArg(QStringLiteral("^\"?([+-]?\\d+)\"?$"));
            static const QRegularExpression reSignedIntOnly(QStringLiteral("^([+-]?\\d+)$"));
            static const QRegularExpression reDecArg(QStringLiteral("^\"?([+-]?[0-9.eE+-]+)\"?$"));
            static const QRegularExpression reTsPair(QStringLiteral("^(\\d+)\\s*,\\s*(\\d+)$"));
            static const QRegularExpression reTsObj(
                QStringLiteral("^\\{\\s*\"?t\"?\\s*:\\s*(\\d+)\\s*,\\s*\"?i\"?\\s*:\\s*(\\d+)\\s*\\}$"));
            static const QRegularExpression reBinArg(
                QStringLiteral("^(0x[0-9a-fA-F]+|\\d+)\\s*,\\s*\"([^\"]*)\"$"));
            static const QRegularExpression reHexArg(
                QStringLiteral("^(0x[0-9a-fA-F]+|\\d+)\\s*,\\s*\"([0-9a-fA-F]*)\"$"));
            static const QRegularExpression reUuidArg(QStringLiteral("^\"([0-9a-fA-F-]{32,36})\"$"));
            static const QRegularExpression reDbRefArg(
                QStringLiteral("^\"((?:[^\"\\\\]|\\\\.)*)\"\\s*,\\s*(?:(?:new\\s+)?ObjectI[dD]\\s*\\(\\s*\"([0-9a-fA-F]{24})\"\\s*\\)|\"((?:[^\"\\\\]|\\\\.)*)\")$"));
            static const QRegularExpression reHex24Only(QStringLiteral("^[0-9a-fA-F]{24}$"));

            QString out;
            out.reserve(src.size() + 64);
            const int n = src.size();
            int i = 0;

            while (i < n) {
                const QChar ch = src[i];

                // 1. Copy JSON double-quoted string literals verbatim
                if (ch == QLatin1Char('"')) {
                    const int strStart = i++;
                    while (i < n) {
                        if (src[i] == QLatin1Char('\\') && i + 1 < n) {
                            i += 2;
                            continue;
                        }
                        if (src[i] == QLatin1Char('"')) {
                            ++i;
                            break;
                        }
                        ++i;
                    }
                    out += src.mid(strStart, i - strStart);
                    continue;
                }

                const bool atWordStart = (i == 0 || !isShellIdentChar(src[i - 1]));

                // 2. Signed -Infinity / +Infinity outside strings
                if (atWordStart && (ch == QLatin1Char('-') || ch == QLatin1Char('+')) &&
                    QStringView(src).mid(i + 1).startsWith(QLatin1String("Infinity")) &&
                    (i + 9 >= n || !isShellIdentChar(src[i + 9]))) {
                    int k = i + 9;
                    while (k < n && src[k].isSpace())
                        ++k;
                    if (k >= n || src[k] != QLatin1Char(':')) {
                        out += (ch == QLatin1Char('-'))
                            ? QStringLiteral("{\"$numberDouble\":\"-Infinity\"}")
                            : QStringLiteral("{\"$numberDouble\":\"Infinity\"}");
                        i += 9;
                        continue;
                    }
                }

                // 3. Identifiers / `new Ctor(...)` / bare value keywords outside strings
                if (atWordStart && isShellIdentChar(ch) && !ch.isDigit()) {
                    int identEnd = i + 1;
                    while (identEnd < n && isShellIdentChar(src[identEnd]))
                        ++identEnd;
                    const QString ident = src.mid(i, identEnd - i);

                    int afterIdent = identEnd;
                    while (afterIdent < n && src[afterIdent].isSpace())
                        ++afterIdent;

                    // Bare value keywords (not followed by ':' as an unquoted key or '(')
                    if (afterIdent >= n || (src[afterIdent] != QLatin1Char(':') && src[afterIdent] != QLatin1Char('('))) {
                        if (ident == QLatin1String("undefined")) {
                            out += QStringLiteral("{\"$undefined\":true}");
                            i = identEnd;
                            continue;
                        }
                        if (ident == QLatin1String("NaN")) {
                            out += QStringLiteral("{\"$numberDouble\":\"NaN\"}");
                            i = identEnd;
                            continue;
                        }
                        if (ident == QLatin1String("Infinity")) {
                            out += QStringLiteral("{\"$numberDouble\":\"Infinity\"}");
                            i = identEnd;
                            continue;
                        }
                    }

                    QString ctor = ident;
                    int openParen = afterIdent;
                    if (ident == QLatin1String("new") && afterIdent > identEnd &&
                        afterIdent < n && isShellIdentChar(src[afterIdent]) && !src[afterIdent].isDigit()) {
                        int ctorEnd = afterIdent + 1;
                        while (ctorEnd < n && isShellIdentChar(src[ctorEnd]))
                            ++ctorEnd;
                        ctor = src.mid(afterIdent, ctorEnd - afterIdent);
                        openParen = ctorEnd;
                        while (openParen < n && src[openParen].isSpace())
                            ++openParen;
                    }

                    if (openParen < n && src[openParen] == QLatin1Char('(')) {
                        int closeParen = openParen + 1;
                        int depth = 1;
                        while (closeParen < n && depth > 0) {
                            const QChar pc = src[closeParen];
                            if (pc == QLatin1Char('"')) {
                                ++closeParen;
                                while (closeParen < n) {
                                    if (src[closeParen] == QLatin1Char('\\') && closeParen + 1 < n) {
                                        closeParen += 2;
                                        continue;
                                    }
                                    if (src[closeParen] == QLatin1Char('"'))
                                        break;
                                    ++closeParen;
                                }
                            } else if (pc == QLatin1Char('(')) {
                                ++depth;
                            } else if (pc == QLatin1Char(')')) {
                                --depth;
                                if (depth == 0)
                                    break;
                            }
                            ++closeParen;
                        }

                        if (depth == 0 && closeParen < n) {
                            const QString args = src.mid(openParen + 1, closeParen - openParen - 1).trimmed();
                            QString replacement;

                            if (ctor == QLatin1String("ObjectId") || ctor == QLatin1String("ObjectID")) {
                                if (args.isEmpty()) {
                                    const std::string hex = bsoncxx::oid{}.to_string();
                                    replacement = QStringLiteral("{\"$oid\":\"%1\"}").arg(QString::fromStdString(hex));
                                } else if (const auto m = reOidArg.match(args); m.hasMatch()) {
                                    replacement = QStringLiteral("{\"$oid\":\"%1\"}").arg(m.captured(1));
                                }
                            } else if (ctor == QLatin1String("ISODate") || ctor == QLatin1String("Date")) {
                                if (args.isEmpty()) {
                                    const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                                    replacement = QStringLiteral("{\"$date\":\"%1\"}").arg(nowIso);
                                } else if (const auto m = reStrArg.match(args); m.hasMatch()) {
                                    replacement = QStringLiteral("{\"$date\":\"%1\"}").arg(m.captured(1));
                                } else if (const auto mNum = reSignedIntOnly.match(args); mNum.hasMatch()) {
                                    replacement = QStringLiteral("{\"$date\":{\"$numberLong\":\"%1\"}}").arg(mNum.captured(1));
                                }
                            } else if (ctor == QLatin1String("NumberLong")) {
                                if (args.isEmpty()) {
                                    replacement = QStringLiteral("{\"$numberLong\":\"0\"}");
                                } else if (const auto m = reIntArg.match(args); m.hasMatch()) {
                                    replacement = QStringLiteral("{\"$numberLong\":\"%1\"}").arg(m.captured(1));
                                }
                            } else if (ctor == QLatin1String("NumberInt")) {
                                if (args.isEmpty()) {
                                    replacement = QStringLiteral("{\"$numberInt\":\"0\"}");
                                } else if (const auto m = reIntArg.match(args); m.hasMatch()) {
                                    replacement = QStringLiteral("{\"$numberInt\":\"%1\"}").arg(m.captured(1));
                                }
                            } else if (ctor == QLatin1String("NumberDecimal")) {
                                if (const auto m = reDecArg.match(args); m.hasMatch()) {
                                    replacement = QStringLiteral("{\"$numberDecimal\":\"%1\"}").arg(m.captured(1));
                                }
                            } else if (ctor == QLatin1String("Timestamp")) {
                                if (const auto m = reTsPair.match(args); m.hasMatch()) {
                                    replacement = QStringLiteral("{\"$timestamp\":{\"t\":%1,\"i\":%2}}")
                                                      .arg(m.captured(1), m.captured(2));
                                } else if (const auto mObj = reTsObj.match(args); mObj.hasMatch()) {
                                    replacement = QStringLiteral("{\"$timestamp\":{\"t\":%1,\"i\":%2}}")
                                                      .arg(mObj.captured(1), mObj.captured(2));
                                }
                            } else if (ctor == QLatin1String("BinData")) {
                                if (const auto m = reBinArg.match(args); m.hasMatch()) {
                                    bool ok = false;
                                    const int sub = m.captured(1).toInt(&ok, 0);
                                    if (ok && sub >= 0 && sub <= 255) {
                                        const QString subHex = QStringLiteral("%1").arg(sub, 2, 16, QLatin1Char('0'));
                                        replacement = QStringLiteral("{\"$binary\":{\"base64\":\"%1\",\"subType\":\"%2\"}}")
                                                          .arg(m.captured(2), subHex);
                                    }
                                }
                            } else if (ctor == QLatin1String("HexData")) {
                                if (const auto m = reHexArg.match(args); m.hasMatch() && (m.captured(2).size() % 2 == 0)) {
                                    bool ok = false;
                                    const int sub = m.captured(1).toInt(&ok, 0);
                                    if (ok && sub >= 0 && sub <= 255) {
                                        const QByteArray raw = QByteArray::fromHex(m.captured(2).toLatin1());
                                        const QString b64 = QString::fromLatin1(raw.toBase64());
                                        const QString subHex = QStringLiteral("%1").arg(sub, 2, 16, QLatin1Char('0'));
                                        replacement = QStringLiteral("{\"$binary\":{\"base64\":\"%1\",\"subType\":\"%2\"}}")
                                                          .arg(b64, subHex);
                                    }
                                }
                            } else if (ctor == QLatin1String("UUID") ||
                                       ctor == QLatin1String("LUUID") ||
                                       ctor == QLatin1String("JUUID") ||
                                       ctor == QLatin1String("NUUID") ||
                                       ctor == QLatin1String("CSUUID") ||
                                       ctor == QLatin1String("PYUUID")) {
                                if (ctor == QLatin1String("UUID") && args.isEmpty()) {
                                    const QByteArray raw = QUuid::createUuid().toRfc4122();
                                    const QString b64 = QString::fromLatin1(raw.toBase64());
                                    replacement = QStringLiteral("{\"$binary\":{\"base64\":\"%1\",\"subType\":\"04\"}}").arg(b64);
                                } else if (const auto m = reUuidArg.match(args); m.hasMatch()) {
                                    UUIDEncoding enc = DefaultEncoding;
                                    QString subType = QStringLiteral("03");
                                    if (ctor == QLatin1String("UUID")) {
                                        enc = DefaultEncoding;
                                        subType = QStringLiteral("04");
                                    } else if (ctor == QLatin1String("JUUID")) {
                                        enc = JavaLegacy;
                                    } else if (ctor == QLatin1String("NUUID") || ctor == QLatin1String("CSUUID")) {
                                        enc = CSharpLegacy;
                                    } else if (ctor == QLatin1String("PYUUID")) {
                                        enc = PythonLegacy;
                                    }
                                    const std::string hex = HexUtils::uuidToHex(m.captured(1).toStdString(), enc);
                                    if (hex.size() == 32) {
                                        const QByteArray raw = QByteArray::fromHex(QByteArray::fromStdString(hex));
                                        const QString b64 = QString::fromLatin1(raw.toBase64());
                                        replacement = QStringLiteral("{\"$binary\":{\"base64\":\"%1\",\"subType\":\"%2\"}}")
                                                          .arg(b64, subType);
                                    }
                                }
                            } else if (ctor == QLatin1String("DBRef")) {
                                if (const auto m = reDbRefArg.match(args); m.hasMatch()) {
                                    const QString coll = m.captured(1);
                                    if (!m.captured(2).isEmpty()) {
                                        replacement = QStringLiteral("{\"$ref\":\"%1\",\"$id\":{\"$oid\":\"%2\"}}")
                                                          .arg(coll, m.captured(2));
                                    } else {
                                        const QString idStr = m.captured(3);
                                        if (reHex24Only.match(idStr).hasMatch()) {
                                            replacement = QStringLiteral("{\"$dbPointer\":{\"$ref\":\"%1\",\"$id\":{\"$oid\":\"%2\"}}}")
                                                              .arg(coll, idStr);
                                        } else {
                                            replacement = QStringLiteral("{\"$ref\":\"%1\",\"$id\":\"%2\"}")
                                                              .arg(coll, idStr);
                                        }
                                    }
                                }
                            } else if (ctor == QLatin1String("MinKey") && args.isEmpty()) {
                                replacement = QStringLiteral("{\"$minKey\":1}");
                            } else if (ctor == QLatin1String("MaxKey") && args.isEmpty()) {
                                replacement = QStringLiteral("{\"$maxKey\":1}");
                            }

                            if (!replacement.isEmpty()) {
                                out += replacement;
                                i = closeParen + 1;
                                continue;
                            }
                        }
                    }

                    out += ident;
                    i = identEnd;
                    continue;
                }

                out += ch;
                ++i;
            }
            return out;
        }
    } // anonymous namespace

    namespace MongoJsonUtils
    {
        DuplicateTemplateResult buildDuplicateDocumentTemplate(bsoncxx::document::view srcDoc,
                                                               UUIDEncoding uuidEncoding,
                                                               SupportedTimes timeFormat)
        {
            using bsoncxx::builder::basic::kvp;

            const auto origId = srcDoc["_id"];
            const bool nonObjectId = (origId && origId.type() != bsoncxx::type::k_oid);

            bsoncxx::builder::basic::document copyBuilder;
            if (nonObjectId)
                copyBuilder.append(kvp("_id", ""));

            for (const auto &el : srcDoc) {
                const auto k = el.key();
                if (k == "_id")
                    continue;
                copyBuilder.append(kvp(k, el.get_value()));
            }

            const auto docVal = copyBuilder.extract();
            if (docVal.view().empty()) {
                return DuplicateTemplateResult{ QStringLiteral("{\n    \n}"), 6 };
            }

            const std::string rawJson = BsonUtils::jsonString(
                docVal.view(), TenGen, 1, uuidEncoding, timeFormat);
            const QString json = QtUtils::toQString(rawJson);

            int caretOffset = -1;
            if (nonObjectId) {
                const int keyPos = json.indexOf(QStringLiteral("\"_id\""));
                if (keyPos >= 0) {
                    const int colonPos = json.indexOf(QLatin1Char(':'), keyPos + 5);
                    if (colonPos >= 0) {
                        const int emptyQuotesPos = json.indexOf(QStringLiteral("\"\""), colonPos + 1);
                        if (emptyQuotesPos >= 0)
                            caretOffset = emptyQuotesPos + 1;
                    }
                }
            }

            return DuplicateTemplateResult{ json, caretOffset };
        }

        QString normalizeShellJsonToExtendedJson(const QString &input)
        {
            QString s = stripCommentsAndSingleQuotes(convertRegexLiterals(input));
            s = replaceShellConstructorsOutsideStrings(s);

            // Quote unquoted object keys & strip trailing commas outside string literals
            s = transformOutsideStrings(s, [](const QString &seg) {
                QString r = seg;
                static const QRegularExpression reUnquotedKey(
                    QStringLiteral("([{,]\\s*)([A-Za-z_$][A-Za-z0-9_$]*)(\\s*:)"));
                r.replace(reUnquotedKey, QStringLiteral("\\1\"\\2\"\\3"));
                return r;
            });

            // Strip trailing commas before '}' or ']' (outside strings)
            s = transformOutsideStrings(s, [](const QString &seg) {
                QString r = seg;
                static const QRegularExpression reTrailingComma(QStringLiteral(",(\\s*[}\\]])"));
                r.replace(reTrailingComma, QStringLiteral("\\1"));
                return r;
            });

            return s;
        }

        bool parseDocuments(const QString &input,
                            std::vector<bsoncxx::document::value> &outDocs,
                            QString &outError)
        {
            outDocs.clear();
            outError.clear();

            const QString normalized = normalizeShellJsonToExtendedJson(input);
            std::vector<QString> blocks;
            if (!extractTopLevelBlocks(normalized, blocks, outError))
                return false;

            if (blocks.empty()) {
                outError = QCoreApplication::translate("MongoJsonUtils", "No JSON documents found.");
                return false;
            }

            for (size_t i = 0; i < blocks.size(); ++i) {
                try {
                    outDocs.push_back(bsoncxx::from_json(blocks[i].toUtf8().toStdString()));
                } catch (const std::exception &ex) {
                    outError = blocks.size() > 1
                        ? QCoreApplication::translate("MongoJsonUtils", "Document #%1: %2").arg(i + 1).arg(QString::fromUtf8(ex.what()))
                        : QString::fromUtf8(ex.what());
                    outDocs.clear();
                    return false;
                }
            }
            return true;
        }

        bool parseDocuments(const QString &input,
                            std::vector<bsoncxx::document::value> &outDocs,
                            QString *outError)
        {
            QString err;
            const bool ok = parseDocuments(input, outDocs, err);
            if (outError)
                *outError = err;
            return ok;
        }

        bool parseSingleDocument(const QString &input,
                                 bsoncxx::document::value &outDoc,
                                 QString &outError)
        {
            std::vector<bsoncxx::document::value> docs;
            if (!parseDocuments(input, docs, outError))
                return false;
            if (docs.size() != 1) {
                outError = QCoreApplication::translate("MongoJsonUtils", "Expected exactly 1 JSON document, but found %1.").arg(docs.size());
                return false;
            }
            outDoc = std::move(docs[0]);
            return true;
        }

        bool parseSingleDocument(const QString &input,
                                 bsoncxx::document::value &outDoc,
                                 QString *outError)
        {
            QString err;
            const bool ok = parseSingleDocument(input, outDoc, err);
            if (outError)
                *outError = err;
            return ok;
        }

        bool formatJsonDocuments(const QString &input,
                                 QString &outFormatted,
                                 QString &outError)
        {
            std::vector<bsoncxx::document::value> docs;
            if (!parseDocuments(input, docs, outError))
                return false;

            auto *settings = AppRegistry::instance().settingsManager();
            const UUIDEncoding uuidEnc = settings ? settings->uuidEncoding() : DefaultEncoding;
            const SupportedTimes tz = settings ? settings->timeZone() : Utc;

            QString out;
            for (size_t i = 0; i < docs.size(); ++i) {
                if (docs.size() > 1)
                    out += QStringLiteral("/* %1 */\n").arg(i + 1);
                const std::string s = BsonUtils::jsonString(docs[i].view(), TenGen, 1, uuidEnc, tz);
                out += QtUtils::toQString(s);
                if (i + 1 < docs.size())
                    out += QStringLiteral("\n\n");
            }
            outFormatted = out;
            return true;
        }

        QString formatPrettyJson(const QString &input, int indent)
        {
            const int step = std::max(1, indent);
            const QString src = input.trimmed();
            if (src.isEmpty())
                return QString();

            QString out;
            out.reserve(src.size() + src.size() / 4);
            int depth = 0;
            const int n = src.size();

            auto appendIndent = [&out, step](int d) {
                if (d > 0)
                    out.append(QString(d * step, QLatin1Char(' ')));
            };

            for (int i = 0; i < n; ++i) {
                const QChar ch = src[i];
                if (ch.isSpace())
                    continue;

                if (ch == QLatin1Char('"')) {
                    out += ch;
                    ++i;
                    while (i < n) {
                        out += src[i];
                        if (src[i] == QLatin1Char('\\') && i + 1 < n) {
                            ++i;
                            out += src[i];
                        } else if (src[i] == QLatin1Char('"')) {
                            break;
                        }
                        ++i;
                    }
                    continue;
                }

                if (ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
                    const QChar closer = (ch == QLatin1Char('{')) ? QLatin1Char('}') : QLatin1Char(']');
                    int peek = i + 1;
                    while (peek < n && src[peek].isSpace())
                        ++peek;
                    if (peek < n && src[peek] == closer) {
                        out += ch;
                        out += closer;
                        i = peek;
                        continue;
                    }
                    out += ch;
                    out += QLatin1Char('\n');
                    ++depth;
                    appendIndent(depth);
                    continue;
                }

                if (ch == QLatin1Char('}') || ch == QLatin1Char(']')) {
                    depth = std::max(0, depth - 1);
                    out += QLatin1Char('\n');
                    appendIndent(depth);
                    out += ch;
                    continue;
                }

                if (ch == QLatin1Char(',')) {
                    out += QLatin1Char(',');
                    out += QLatin1Char('\n');
                    appendIndent(depth);
                    continue;
                }

                if (ch == QLatin1Char(':')) {
                    out += QStringLiteral(": ");
                    continue;
                }

                out += ch;
            }
            return out;
        }

        bool updateScalarFieldAtPath(bsoncxx::document::view origDoc,
                                     const QString &dottedPath,
                                     const QString &rawInput,
                                     const QString &typeHint,
                                     bsoncxx::document::value &outDoc,
                                     QString &outError)
        {
            const QStringList parts = dottedPath.split(QLatin1Char('.'), Qt::SkipEmptyParts);
            if (parts.isEmpty()) {
                outError = QCoreApplication::translate("MongoJsonUtils", "Field path is empty.");
                return false;
            }
            if (parts.size() == 1 && parts[0] == QLatin1String("_id")) {
                outError = QCoreApplication::translate("MongoJsonUtils", "The '_id' field is immutable in MongoDB.");
                return false;
            }
            return rebuildDocUpdatingPath(origDoc, parts, 0, rawInput, typeHint, outDoc, outError);
        }

        QString extractIsoDateFromObjectId(const QString &oidText)
        {
            static const QRegularExpression reHex(QStringLiteral("([0-9a-fA-F]{24})"));
            const auto m = reHex.match(oidText);
            if (!m.hasMatch())
                return QString();
            bool ok = false;
            const uint secs = m.captured(1).left(8).toUInt(&ok, 16);
            if (!ok)
                return QString();
            const QDateTime dt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(secs), QTimeZone::UTC);
            return QStringLiteral("ISODate(\"%1\")").arg(dt.toString(Qt::ISODate));
        }

    }
}
