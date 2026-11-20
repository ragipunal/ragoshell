#include "ragoshell/core/utils/AiQueryAssistantInternal.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include "ragoshell/core/BrandInfo.h"

namespace RagoShell::AiQueryAssistant
{
    using namespace detail;

    namespace
    {
            /// "customers" → "customer", "categories" → "category", "boxes" → "box".
            QString singular(const QString &name)
            {
                const QString n = name.toLower();
                if (n.endsWith(QLatin1String("ies")) && n.size() > 3)
                    return n.chopped(3) + QLatin1Char('y');
                for (const char *es : {"sses", "xes", "ches", "shes"}) {
                    if (n.endsWith(QLatin1String(es)) && n.size() > 4)
                        return n.chopped(2);
                }
                if (n.endsWith(QLatin1Char('s')) && !n.endsWith(QLatin1String("ss")) && n.size() > 1)
                    return n.chopped(1);
                return n;
            }

            /// A singular word (or a field's base name) names the collection.
            bool namesCollection(const QString &word, const QString &collection)
            {
                const QString w = word.toLower();
                const QString c = collection.toLower();
                return !w.isEmpty()
                       && (w == c || w == singular(c) || singular(w) == singular(c)
                           || w + QLatin1Char('s') == c || w + QLatin1String("es") == c);
            }

            /// A field that points to a collection: customerId, customer_id,
            /// customerID, or customer itself; the referenced name, singular.
            QString referencedName(const SchemaField &f)
            {
                QString name = f.name;
                for (const char *suffix : {"_id", "Id", "ID", "_ids", "Ids", "IDs"}) {
                    const QString s = QString::fromLatin1(suffix);
                    if (name.size() > s.size() && name.endsWith(s)) {
                        name.chop(s.size());
                        return singular(name);
                    }
                }
                // An ObjectId field named like a collection ("customer": ObjectId).
                for (const auto &t : f.types) {
                    if (t.first == QLatin1String("ObjectId") && name != QLatin1String("_id"))
                        return singular(name);
                }
                return {};
            }

            bool mentions(const QString &question, const QString &collection)
            {
                static const QRegularExpression word(QStringLiteral("[\\p{L}\\p{N}_]+"));
                for (auto it = word.globalMatch(question); it.hasNext();) {
                    if (namesCollection(it.next().captured(), collection))
                        return true;
                }
                return false;
            }

            QStringList relatedLines(const RelatedCollection &r)
            {
                QStringList lines;
                lines << (r.documents >= 0 ? QStringLiteral("Collection: %1 (about %2 documents)").arg(r.name).arg(r.documents)
                                           : QStringLiteral("Collection: %1").arg(r.name));
                for (const SchemaField &f : r.fields) {
                    QStringList types;
                    for (const auto &t : f.types)
                        types << t.first;
                    lines << QStringLiteral("  %1: %2, %3%").arg(f.path, types.join(QLatin1Char('/'))).arg(qRound(f.presence * 100));
                }
                if (r.omittedFields > 0)
                    lines << QStringLiteral("  ... %1 more field path(s) not listed").arg(r.omittedFields);
                return lines;
            }

            int relatedSize(const RelatedCollection &r)
            {
                int n = 0;
                for (const QString &line : relatedLines(r))
                    n += line.size() + 1;
                return n;
            }

            QString typesText(const std::vector<std::pair<QString, qint64>> &types)
            {
                qint64 total = 0;
                for (const auto &t : types)
                    total += t.second;
                QStringList parts;
                for (const auto &[name, n] : types) {
                    if (types.size() == 1 || total <= 0) {
                        parts << name;
                        continue;
                    }
                    const int pct = qRound(100.0 * n / total);
                    parts << QStringLiteral("%1 %2%").arg(name, pct == 0 ? QStringLiteral("<1")
                                                                       : QString::number(pct));
                }
                return parts.join(QStringLiteral(" | "));
            }

            QString clipped(const QString &value)
            {
                return value.size() <= kMaxValueLength ? value
                                                       : value.left(kMaxValueLength) + QStringLiteral("…");
            }

    }

        Context contextFrom(const SchemaResult &result, const QString &database, const QString &collection,
                            const QString &serverVersion, bool includeValues)
        {
            Context c;
            c.database = database;
            c.collection = collection;
            c.serverVersion = serverVersion;
            c.sampledDocuments = result.sampledDocuments;
            c.recentDocuments = result.recentDocuments;
            c.totalDocuments = result.totalDocuments;
            c.fullScan = result.fullScan;
            c.indexes = result.indexes;
            c.fields = result.fields;
            c.includeValues = includeValues;
            return c;
        }

        std::vector<RelatedCollection> relatedCollections(const QString &openCollection,
                                                          const std::vector<SchemaField> &openFields,
                                                          const QString &question,
                                                          const std::map<QString, SchemaResult> &others,
                                                          int budget)
        {
            // What the open collection points to (customerId → "customer").
            QStringList pointedTo;
            for (const SchemaField &f : openFields) {
                const QString name = referencedName(f);
                if (!name.isEmpty())
                    pointedTo << name;
            }

            struct Candidate
            {
                int rank;
                int fields;
                const QString *name;
                const SchemaResult *result;
            };
            std::vector<Candidate> candidates;
            for (const auto &[name, result] : others) {
                if (name == openCollection || !result.ok)
                    continue;
                int rank = 3;
                if (mentions(question, name)) {
                    rank = 0;
                } else if (std::any_of(pointedTo.begin(), pointedTo.end(),
                                       [&name](const QString &base) { return namesCollection(base, name); })) {
                    rank = 1;
                } else {
                    for (const SchemaField &f : result.fields) {
                        if (namesCollection(referencedName(f), openCollection)) {
                            rank = 2;
                            break;
                        }
                    }
                }
                candidates.push_back({rank, static_cast<int>(result.fields.size()), &name, &result});
            }
            std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
                if (a.rank != b.rank)
                    return a.rank < b.rank;
                return a.fields < b.fields;   // the rest: small descriptions first
            });

            std::vector<RelatedCollection> related;
            int left = budget;
            for (const Candidate &c : candidates) {
                RelatedCollection r;
                r.name = *c.name;
                r.documents = c.result->totalDocuments;
                r.fields = c.result->fields;
                int size = relatedSize(r);
                if (size > left) {
                    // Keep the most common fields, in their catalog order.
                    std::vector<const SchemaField *> byPresence;
                    for (const SchemaField &f : c.result->fields)
                        byPresence.push_back(&f);
                    std::stable_sort(byPresence.begin(), byPresence.end(),
                                     [](const SchemaField *a, const SchemaField *b) { return a->presence > b->presence; });
                    while (!byPresence.empty() && size > left) {
                        byPresence.pop_back();
                        QSet<const SchemaField *> keep(byPresence.begin(), byPresence.end());
                        r.fields.clear();
                        for (const SchemaField &f : c.result->fields) {
                            if (keep.contains(&f))
                                r.fields.push_back(f);
                        }
                        r.omittedFields = static_cast<int>(c.result->fields.size() - r.fields.size());
                        size = relatedSize(r);
                    }
                    if (r.fields.empty() || size > left)
                        break;   // nothing useful fits any more
                }
                left -= size;
                related.push_back(std::move(r));
            }
            return related;
        }

        QStringList referencedCollections(const QString &script)
        {
            QStringList names;
            auto add = [&names](const QString &name) {
                if (!name.isEmpty() && !names.contains(name))
                    names << name;
            };
            // $lookup / $graphLookup: the from of the stage object that follows.
            static const QRegularExpression lookup(QStringLiteral(R"(\$(?:lookup|graphLookup)\b)"));
            static const QRegularExpression from(QStringLiteral(R"(\bfrom["']?\s*:\s*(["'])(.*?)\1)"));
            for (auto it = lookup.globalMatch(script); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                const QRegularExpressionMatch f = from.match(script, m.capturedEnd());
                if (f.hasMatch())
                    add(f.captured(2));
            }
            // $unionWith: "coll" or { coll: "coll", ... }
            static const QRegularExpression unionWith(QStringLiteral(
                R"(\$unionWith["']?\s*:\s*(?:(["'])(.*?)\1|\{[^}]*?\bcoll["']?\s*:\s*(["'])(.*?)\3))"));
            for (auto it = unionWith.globalMatch(script); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                add(m.captured(2).isEmpty() ? m.captured(4) : m.captured(2));
            }
            return names;
        }

        QString schemaSummary(const Context &context)
        {
            QStringList lines;
            lines << QStringLiteral("Database: %1").arg(context.database);
            lines << QStringLiteral("Collection: %1").arg(context.collection);
            lines << QStringLiteral("MongoDB server version: %1")
                         .arg(context.serverVersion.isEmpty() ? QStringLiteral("unknown")
                                                              : context.serverVersion);
            if (context.fullScan) {
                lines << QStringLiteral("Documents: %1 (all analyzed)").arg(context.sampledDocuments);
            } else if (context.totalDocuments >= 0) {
                QString line = QStringLiteral("Documents: about %1; sampled %2")
                                   .arg(context.totalDocuments).arg(context.sampledDocuments);
                if (context.recentDocuments > 0)
                    line += QStringLiteral(" (%1 newest, the rest random)").arg(context.recentDocuments);
                lines << line;
            } else {
                lines << QStringLiteral("Sampled documents: %1").arg(context.sampledDocuments);
            }
            if (!context.indexes.isEmpty()) {
                QStringList keys;
                for (const QString &key : context.indexes)
                    keys << QString(key).replace(QStringLiteral(" : "), QStringLiteral(": "));
                lines << QStringLiteral("Indexes: %1").arg(keys.join(QStringLiteral("; ")));
            }
            lines << QString();
            lines << QStringLiteral("Fields (path: BSON types, share of sampled documents containing it):");

            int n = 0;
            for (const SchemaField &f : context.fields) {
                if (n++ == kMaxFields) {
                    lines << QStringLiteral("... %1 more field path(s) not listed")
                                 .arg(static_cast<qint64>(context.fields.size()) - kMaxFields);
                    break;
                }
                QString line = QStringLiteral("%1: %2, %3%")
                                   .arg(f.path, typesText(f.types))
                                   .arg(qRound(f.presence * 100));
                if (!f.itemTypes.empty())
                    line += QStringLiteral(", array items: %1").arg(typesText(f.itemTypes));
                if (f.onlyRecent)
                    line += QStringLiteral(", only in the newest documents");
                if (context.includeValues) {
                    if (!f.minValue.isEmpty())
                        line += QStringLiteral(", range %1 .. %2").arg(clipped(f.minValue), clipped(f.maxValue));
                    else if (!f.topValues.empty()) {
                        // A category is listed completely, so the model uses the stored spelling.
                        const int max = f.categorical ? SchemaAccumulator::kMaxCategoryValues : kMaxValuesPerField;
                        QStringList values;
                        for (const auto &v : f.topValues) {
                            if (values.size() == max)
                                break;
                            values << clipped(v.first);
                        }
                        line += f.categorical
                                    ? QStringLiteral(", all values: %1").arg(values.join(QStringLiteral(", ")))
                                    : QStringLiteral(", frequent values: %1").arg(values.join(QStringLiteral(", ")));
                    }
                    if (f.letterCase == QLatin1String("upper"))
                        line += QStringLiteral(", values are UPPERCASE");
                    else if (f.letterCase == QLatin1String("lower"))
                        line += QStringLiteral(", values are lowercase");
                }
                lines << line;
            }
            if (context.fields.empty())
                lines << QStringLiteral("(no documents sampled)");
            if (!context.related.empty()) {
                lines << QString();
                lines << QStringLiteral("Related collections of the same database, for $lookup "
                                        "(field names, types and share of documents; no values):");
                for (const RelatedCollection &r : context.related)
                    lines << relatedLines(r);
            }
            return lines.join('\n');
        }

        QString systemPrompt()
        {
            return QStringLiteral(
                "You turn natural-language requests into MongoDB shell queries for %1, "
                "a MongoDB GUI. Answer only with JSON matching the output schema.\n")
                .arg(QLatin1String(BrandInfo::productName))
                + QStringLiteral(
                "\n"
                "- kind \"find\": one statement db.getCollection('<collection>').find(<filter>, "
                "<projection>), optionally followed by .sort(...), .skip(...) and .limit(...).\n"
                "- kind \"aggregate\": one statement db.getCollection('<collection>').aggregate([...]).\n"
                "- kind \"unsupported\": the request cannot be answered with one read-only query on "
                "this collection (it asks to change data, is unclear, or needs fields that do not "
                "exist). Leave script empty and say why in explanation.\n"
                "\n"
                "Rules:\n"
                "- Query the given collection. Use $lookup (or $unionWith) only when the request needs "
                "data from another collection, and only with the related collections listed after the "
                "schema; join on fields whose names and types match.\n"
                "- Never write data: no $out, $merge, insert, update, delete or drop.\n"
                "- Use field paths from the schema. Match the listed BSON types (for example compare "
                "Date fields with ISODate(...), ObjectId fields with ObjectId(...)).\n"
                "- Compare with values as the schema shows them (spelling, letter case): map the words "
                "of the request, in any language, to the listed values.\n"
                "- Use mongo shell syntax: ObjectId('...'), ISODate('...'), NumberLong(...), "
                "NumberDecimal('...'), /regex/i literals.\n"
                "- The query must run on the given MongoDB server version; on old servers avoid newer "
                "operators (for example 4.0 has no $dateTrunc, $setWindowFields, $function, "
                "$getField or $unionWith).\n"
                "- Prefer find when it is enough; add a .limit(...) only when the request asks for a "
                "number of results.\n"
                "- explanation: one or two sentences, in the language of the request.");
        }

        QString userPrompt(const Context &context, const QString &question)
        {
            return schemaSummary(context) + QStringLiteral("\n\nRequest:\n")
                   + question.trimmed().left(kMaxQuestionLength);
        }

        QByteArray requestBody(const QString &model, const Context &context,
                               const QString &question)
        {
            return requestBody(Provider::Anthropic, model, context, question);
        }

        QByteArray requestBody(Provider provider, const QString &model, const Context &context,
                               const QString &question)
        {
            if (provider == Provider::Gemini) {
                // The model goes in the URL.
                QJsonObject generationConfig{
                    {"responseMimeType", "application/json"},
                    {"responseSchema", geminiOutputSchema()},
                    {"maxOutputTokens", kMaxTokens},
                };
                QJsonObject body{
                    {"systemInstruction", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", systemPrompt()}}}}}},
                    {"contents", QJsonArray{QJsonObject{
                                     {"role", "user"},
                                     {"parts", QJsonArray{QJsonObject{{"text", userPrompt(context, question)}}}}}}},
                    {"generationConfig", generationConfig},
                };
                return QJsonDocument(body).toJson(QJsonDocument::Compact);
            }

            const QString m = model.isEmpty() ? defaultModel(provider) : model;
            QJsonObject outputConfig{
                {"effort", "medium"},
                {"format", QJsonObject{{"type", "json_schema"}, {"schema", outputSchema()}}},
            };
            QJsonObject body{
                {"model", m},
                {"max_tokens", kMaxTokens},
                {"system", systemPrompt()},
                {"output_config", outputConfig},
                {"messages", QJsonArray{QJsonObject{{"role", "user"},
                                                    {"content", userPrompt(context, question)}}}},
            };
            if (supportsFallback(m))
                body.insert("fallbacks", "default");
            return QJsonDocument(body).toJson(QJsonDocument::Compact);
        }

}
