#include "ragoshell/core/utils/MongoJsonUtils.h"

#include <algorithm>
#include <vector>

#include <QRegularExpression>

namespace RagoShell
{
    namespace MongoJsonUtils
    {
        namespace
        {
            QString stripJsCommentsPreservingStrings(const QString &src)
            {
                QString out;
                out.reserve(src.size());
                const int n = src.size();
                int i = 0;
                while (i < n) {
                    const QChar c = src[i];
                    if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
                        const QChar quote = c;
                        out += c;
                        ++i;
                        while (i < n) {
                            out += src[i];
                            if (src[i] == QLatin1Char('\\') && i + 1 < n) {
                                ++i;
                                out += src[i];
                            } else if (src[i] == quote) {
                                ++i;
                                break;
                            }
                            ++i;
                        }
                        continue;
                    }
                    if (c == QLatin1Char('/') && i + 1 < n && src[i + 1] == QLatin1Char('/')) {
                        i += 2;
                        while (i < n && src[i] != QLatin1Char('\n'))
                            ++i;
                        continue;
                    }
                    if (c == QLatin1Char('/') && i + 1 < n && src[i + 1] == QLatin1Char('*')) {
                        i += 2;
                        while (i + 1 < n && !(src[i] == QLatin1Char('*') && src[i + 1] == QLatin1Char('/')))
                            ++i;
                        i = std::min(n, i + 2);
                        out += QLatin1Char(' ');
                        continue;
                    }
                    out += c;
                    ++i;
                }
                return out;
            }

            void skipWs(const QString &s, int &pos)
            {
                while (pos < s.size() && s[pos].isSpace())
                    ++pos;
            }

            bool isIdentStart(QChar c)
            {
                return c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$');
            }

            bool isIdentChar(QChar c)
            {
                return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
            }

            QString readIdent(const QString &s, int &pos)
            {
                if (pos >= s.size() || !isIdentStart(s[pos]))
                    return QString();
                const int start = pos;
                while (pos < s.size() && isIdentChar(s[pos]))
                    ++pos;
                return s.mid(start, pos - start);
            }

            bool parseJsStringLiteral(const QString &s, int &pos, QString &outUnescaped)
            {
                skipWs(s, pos);
                if (pos >= s.size() || (s[pos] != QLatin1Char('\'') && s[pos] != QLatin1Char('"')))
                    return false;
                const QChar quote = s[pos++];
                QString val;
                while (pos < s.size()) {
                    const QChar c = s[pos++];
                    if (c == QLatin1Char('\\')) {
                        if (pos >= s.size())
                            return false;
                        const QChar esc = s[pos++];
                        switch (esc.unicode()) {
                        case 'n':  val += QLatin1Char('\n'); break;
                        case 'r':  val += QLatin1Char('\r'); break;
                        case 't':  val += QLatin1Char('\t'); break;
                        case '\\': val += QLatin1Char('\\'); break;
                        case '\'': val += QLatin1Char('\''); break;
                        case '"':  val += QLatin1Char('"'); break;
                        default:   val += esc; break;
                        }
                    } else if (c == quote) {
                        outUnescaped = val;
                        return true;
                    } else {
                        val += c;
                    }
                }
                return false;
            }

            // Reads balanced (...) starting at '(' at s[pos], advancing pos past closing ')'.
            bool readBalancedParens(const QString &s, int &pos, QString &outInside)
            {
                skipWs(s, pos);
                if (pos >= s.size() || s[pos] != QLatin1Char('('))
                    return false;
                const int contentStart = ++pos;
                int parenDepth = 1;
                int braceDepth = 0;
                int bracketDepth = 0;

                while (pos < s.size()) {
                    const QChar c = s[pos];
                    if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
                        const QChar q = c;
                        ++pos;
                        while (pos < s.size()) {
                            if (s[pos] == QLatin1Char('\\') && pos + 1 < s.size()) {
                                pos += 2;
                                continue;
                            }
                            if (s[pos] == q) {
                                ++pos;
                                break;
                            }
                            ++pos;
                        }
                        continue;
                    }
                    if (c == QLatin1Char('(')) {
                        ++parenDepth;
                    } else if (c == QLatin1Char(')')) {
                        --parenDepth;
                        if (parenDepth == 0) {
                            if (braceDepth != 0 || bracketDepth != 0)
                                return false;
                            outInside = s.mid(contentStart, pos - contentStart);
                            ++pos;
                            return true;
                        }
                    } else if (c == QLatin1Char('{')) {
                        ++braceDepth;
                    } else if (c == QLatin1Char('}')) {
                        if (--braceDepth < 0)
                            return false;
                    } else if (c == QLatin1Char('[')) {
                        ++bracketDepth;
                    } else if (c == QLatin1Char(']')) {
                        if (--bracketDepth < 0)
                            return false;
                    }
                    ++pos;
                }
                return false;
            }

            bool splitTopLevelArgs(const QString &inside, QStringList &outArgs)
            {
                outArgs.clear();
                if (inside.trimmed().isEmpty())
                    return true;

                int parenDepth = 0;
                int braceDepth = 0;
                int bracketDepth = 0;
                int segStart = 0;
                const int n = inside.size();

                for (int i = 0; i < n; ++i) {
                    const QChar c = inside[i];
                    if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
                        const QChar q = c;
                        ++i;
                        while (i < n) {
                            if (inside[i] == QLatin1Char('\\') && i + 1 < n) {
                                ++i;
                            } else if (inside[i] == q) {
                                break;
                            }
                            ++i;
                        }
                        continue;
                    }
                    if (c == QLatin1Char('(')) ++parenDepth;
                    else if (c == QLatin1Char(')')) { if (--parenDepth < 0) return false; }
                    else if (c == QLatin1Char('{')) ++braceDepth;
                    else if (c == QLatin1Char('}')) { if (--braceDepth < 0) return false; }
                    else if (c == QLatin1Char('[')) ++bracketDepth;
                    else if (c == QLatin1Char(']')) { if (--bracketDepth < 0) return false; }
                    else if (c == QLatin1Char(',') && parenDepth == 0 && braceDepth == 0 && bracketDepth == 0) {
                        outArgs.append(inside.mid(segStart, i - segStart).trimmed());
                        segStart = i + 1;
                    }
                }
                if (parenDepth != 0 || braceDepth != 0 || bracketDepth != 0)
                    return false;
                outArgs.append(inside.mid(segStart).trimmed());
                return true;
            }
        } // namespace

        ParsedFindQuery parseFindQueryChain(const QString &script)
        {
            ParsedFindQuery invalid;
            QString cleaned = stripJsCommentsPreservingStrings(script).trimmed();
            while (cleaned.endsWith(QLatin1Char(';')))
                cleaned = cleaned.left(cleaned.size() - 1).trimmed();

            if (cleaned.isEmpty())
                return invalid;

            int pos = 0;
            if (readIdent(cleaned, pos) != QLatin1String("db"))
                return invalid;

            ParsedFindQuery res;
            skipWs(cleaned, pos);

            // Optional db.explain(...) prefix before collection accessor
            {
                const int saved = pos;
                if (pos < cleaned.size() && cleaned[pos] == QLatin1Char('.')) {
                    ++pos;
                    skipWs(cleaned, pos);
                    if (readIdent(cleaned, pos) == QLatin1String("explain")) {
                        QString expArg;
                        if (!readBalancedParens(cleaned, pos, expArg))
                            return invalid;
                        res.explain = true;
                        res.explainArg = expArg.trimmed();
                        skipWs(cleaned, pos);
                    } else {
                        pos = saved;
                    }
                }
            }

            // Collection accessor: .getCollection('...'), ['...'], or .ident[.subident]
            if (pos < cleaned.size() && cleaned[pos] == QLatin1Char('[')) {
                ++pos;
                QString coll;
                if (!parseJsStringLiteral(cleaned, pos, coll))
                    return invalid;
                skipWs(cleaned, pos);
                if (pos >= cleaned.size() || cleaned[pos] != QLatin1Char(']'))
                    return invalid;
                ++pos;
                res.collection = coll.trimmed();
                res.useGetCollection = true;
            } else if (pos < cleaned.size() && cleaned[pos] == QLatin1Char('.')) {
                ++pos;
                skipWs(cleaned, pos);
                const QString firstIdent = readIdent(cleaned, pos);
                if (firstIdent.isEmpty())
                    return invalid;

                if (firstIdent == QLatin1String("getCollection")) {
                    QString argInside;
                    if (!readBalancedParens(cleaned, pos, argInside))
                        return invalid;
                    int p2 = 0;
                    QString coll;
                    if (!parseJsStringLiteral(argInside, p2, coll))
                        return invalid;
                    skipWs(argInside, p2);
                    if (p2 != argInside.size())
                        return invalid;
                    res.collection = coll.trimmed();
                    res.useGetCollection = true;
                } else {
                    res.collection = firstIdent;
                    res.useGetCollection = false;
                    // Support dotted collection names like db.system.users.find(...)
                    while (true) {
                        skipWs(cleaned, pos);
                        const int dotPos = pos;
                        if (pos >= cleaned.size() || cleaned[pos] != QLatin1Char('.'))
                            break;
                        ++pos;
                        skipWs(cleaned, pos);
                        const QString nextId = readIdent(cleaned, pos);
                        if (nextId.isEmpty())
                            return invalid;
                        if (nextId == QLatin1String("find") || nextId == QLatin1String("explain")) {
                            pos = dotPos;
                            break;
                        }
                        res.collection += QLatin1Char('.') + nextId;
                    }
                }
            } else {
                return invalid;
            }

            if (res.collection.isEmpty())
                return invalid;

            // Optional .explain(...) between collection and .find(...)
            skipWs(cleaned, pos);
            {
                const int saved = pos;
                if (pos < cleaned.size() && cleaned[pos] == QLatin1Char('.')) {
                    ++pos;
                    skipWs(cleaned, pos);
                    if (readIdent(cleaned, pos) == QLatin1String("explain")) {
                        QString expArg;
                        if (!readBalancedParens(cleaned, pos, expArg))
                            return invalid;
                        res.explain = true;
                        res.explainArg = expArg.trimmed();
                    } else {
                        pos = saved;
                    }
                }
            }

            // Required .find(...)
            skipWs(cleaned, pos);
            if (pos >= cleaned.size() || cleaned[pos] != QLatin1Char('.'))
                return invalid;
            ++pos;
            skipWs(cleaned, pos);
            if (readIdent(cleaned, pos) != QLatin1String("find"))
                return invalid;

            QString findInside;
            if (!readBalancedParens(cleaned, pos, findInside))
                return invalid;

            QStringList findArgs;
            if (!splitTopLevelArgs(findInside, findArgs) || findArgs.size() > 2)
                return invalid;

            if (findArgs.size() >= 1 && !findArgs[0].isEmpty())
                res.filter = findArgs[0];
            else
                res.filter = QStringLiteral("{}");

            if (findArgs.size() == 2 && findArgs[1] != QLatin1String("{}"))
                res.projection = findArgs[1];

            // Optional chained cursor modifiers: .sort(...), .projection(...), .limit(...), .skip(...), .explain(...), .pretty()
            while (true) {
                skipWs(cleaned, pos);
                if (pos >= cleaned.size())
                    break;
                if (cleaned[pos] != QLatin1Char('.'))
                    return invalid;
                ++pos;
                skipWs(cleaned, pos);
                const QString method = readIdent(cleaned, pos);
                if (method.isEmpty())
                    return invalid;

                QString modInside;
                if (!readBalancedParens(cleaned, pos, modInside))
                    return invalid;
                const QString arg = modInside.trimmed();

                if (method == QLatin1String("sort")) {
                    res.sort = (arg == QLatin1String("{}")) ? QString() : arg;
                } else if (method == QLatin1String("projection") || method == QLatin1String("project")) {
                    res.projection = (arg == QLatin1String("{}")) ? QString() : arg;
                } else if (method == QLatin1String("limit")) {
                    bool ok = false;
                    const int v = arg.toInt(&ok);
                    if (!ok || v < 0)
                        return invalid;
                    res.limit = v;
                } else if (method == QLatin1String("skip")) {
                    bool ok = false;
                    const int v = arg.toInt(&ok);
                    if (!ok || v < 0)
                        return invalid;
                    res.skip = v;
                } else if (method == QLatin1String("explain")) {
                    res.explain = true;
                    res.explainArg = arg;
                } else if (method == QLatin1String("pretty")) {
                    if (!arg.isEmpty())
                        return invalid;
                } else {
                    return invalid;
                }
            }

            res.valid = true;
            return res;
        }

        ParsedAggregateQuery parseAggregateChain(const QString &script)
        {
            ParsedAggregateQuery invalid;
            QString cleaned = stripJsCommentsPreservingStrings(script).trimmed();
            while (cleaned.endsWith(QLatin1Char(';')))
                cleaned = cleaned.left(cleaned.size() - 1).trimmed();

            int pos = 0;
            if (readIdent(cleaned, pos) != QLatin1String("db"))
                return invalid;
            skipWs(cleaned, pos);

            ParsedAggregateQuery res;
            // Collection accessor: ['...'], .getCollection('...') or .name[.sub...]
            if (pos < cleaned.size() && cleaned[pos] == QLatin1Char('[')) {
                ++pos;
                if (!parseJsStringLiteral(cleaned, pos, res.collection))
                    return invalid;
                skipWs(cleaned, pos);
                if (pos >= cleaned.size() || cleaned[pos] != QLatin1Char(']'))
                    return invalid;
                ++pos;
            } else if (pos < cleaned.size() && cleaned[pos] == QLatin1Char('.')) {
                ++pos;
                skipWs(cleaned, pos);
                const QString first = readIdent(cleaned, pos);
                if (first.isEmpty())
                    return invalid;
                if (first == QLatin1String("getCollection")) {
                    QString argInside;
                    if (!readBalancedParens(cleaned, pos, argInside))
                        return invalid;
                    int p2 = 0;
                    if (!parseJsStringLiteral(argInside, p2, res.collection))
                        return invalid;
                    skipWs(argInside, p2);
                    if (p2 != argInside.size())
                        return invalid;
                } else {
                    res.collection = first;
                    // Dotted names (db.system.users) until ".aggregate".
                    while (true) {
                        skipWs(cleaned, pos);
                        const int dotPos = pos;
                        if (pos >= cleaned.size() || cleaned[pos] != QLatin1Char('.'))
                            break;
                        ++pos;
                        skipWs(cleaned, pos);
                        const QString next = readIdent(cleaned, pos);
                        if (next.isEmpty())
                            return invalid;
                        if (next == QLatin1String("aggregate")) {
                            pos = dotPos;
                            break;
                        }
                        res.collection += QLatin1Char('.') + next;
                    }
                }
            } else {
                return invalid;
            }
            res.collection = res.collection.trimmed();
            if (res.collection.isEmpty())
                return invalid;

            skipWs(cleaned, pos);
            if (pos >= cleaned.size() || cleaned[pos] != QLatin1Char('.'))
                return invalid;
            ++pos;
            skipWs(cleaned, pos);
            if (readIdent(cleaned, pos) != QLatin1String("aggregate"))
                return invalid;
            QString inside;
            if (!readBalancedParens(cleaned, pos, inside))
                return invalid;
            skipWs(cleaned, pos);
            if (pos != cleaned.size())
                return invalid;   // chained cursor methods are not supported

            QStringList args;
            if (!splitTopLevelArgs(inside, args) || args.isEmpty() || args.size() > 2)
                return invalid;
            res.pipeline = args[0].trimmed();
            if (!res.pipeline.startsWith(QLatin1Char('[')))
                return invalid;
            res.valid = true;
            return res;
        }

        QString buildFindQueryChain(const ParsedFindQuery &query)
        {
            const QString coll = query.collection.trimmed();
            if (coll.isEmpty())
                return QString();

            static const QRegularExpression reSimpleIdent(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
            static const QSet<QString> reservedNames = {
                QStringLiteral("getCollection"), QStringLiteral("stats"), QStringLiteral("serverStatus"),
                QStringLiteral("hostInfo"), QStringLiteral("version"), QStringLiteral("currentOp"),
                QStringLiteral("killOp"), QStringLiteral("runCommand"), QStringLiteral("adminCommand"),
                QStringLiteral("createCollection"), QStringLiteral("dropDatabase")
            };

            const bool forceGetColl = query.useGetCollection ||
                                      !reSimpleIdent.match(coll).hasMatch() ||
                                      reservedNames.contains(coll);

            QString prefix;
            if (forceGetColl) {
                QString escaped = coll;
                escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
                escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
                prefix = QStringLiteral("db.getCollection('%1')").arg(escaped);
            } else {
                prefix = QStringLiteral("db.%1").arg(coll);
            }

            const QString f = query.filter.trimmed().isEmpty()
                ? QStringLiteral("{}")
                : query.filter.trimmed();
            const QString p = query.projection.trimmed();
            const QString s = query.sort.trimmed();

            QString out = prefix + QStringLiteral(".find(") + f;
            if (!p.isEmpty() && p != QLatin1String("{}"))
                out += QStringLiteral(", ") + p;
            out += QLatin1Char(')');

            if (!s.isEmpty() && s != QLatin1String("{}"))
                out += QStringLiteral(".sort(") + s + QLatin1Char(')');
            if (query.skip > 0)
                out += QStringLiteral(".skip(%1)").arg(query.skip);
            if (query.limit > 0)
                out += QStringLiteral(".limit(%1)").arg(query.limit);
            if (query.explain) {
                const QString ea = query.explainArg.trimmed().isEmpty()
                    ? QStringLiteral("'executionStats'")
                    : query.explainArg.trimmed();
                out += QStringLiteral(".explain(%1)").arg(ea);
            }
            return out;
        }

        QStringList detectDestructiveScriptOperations(const QString &script)
        {
            const QString noComments = stripJsCommentsPreservingStrings(script);
            if (noComments.trimmed().isEmpty())
                return {};

            // Strip JS string/template literal contents so values like
            // db.logs.find({ msg: "db.users.drop()" }) never trigger false positives.
            QString codeOnly;
            codeOnly.reserve(noComments.size());
            const int n = noComments.size();
            for (int i = 0; i < n; ++i) {
                const QChar c = noComments[i];
                if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
                    const QChar quote = c;
                    codeOnly.append(quote);
                    ++i;
                    while (i < n) {
                        if (noComments[i] == QLatin1Char('\\') && i + 1 < n) {
                            ++i;
                        } else if (noComments[i] == quote) {
                            break;
                        }
                        ++i;
                    }
                    codeOnly.append(quote);
                } else {
                    codeOnly.append(c);
                }
            }

            // Neutralize explainable collection wrappers so query-plan inspections stay non-blocking:
            // e.g. db.coll.explain('executionStats').remove(...), db.coll.explain().update(...),
            // db.explain().coll.aggregate([...]), or db.coll.aggregate([...], { explain: true }).
            QString sanitized = codeOnly;
            static const QRegularExpression reExplainChain(
                QStringLiteral("\\.\\s*explain\\s*\\([^)]*\\)(?:\\s*\\.\\s*(?:getCollection\\s*\\([^)]*\\)|[A-Za-z_$][\\w$]*))?\\s*\\.\\s*"
                               "(?:remove|deleteOne|deleteMany|update|updateOne|updateMany|replaceOne|findAndModify|findOneAndDelete|findOneAndUpdate|findOneAndReplace|aggregate)\\s*\\("));
            sanitized.replace(reExplainChain, QStringLiteral(".explainPlan("));

            QStringList detected;
            auto addUnique = [&detected](const QString &label) {
                if (!detected.contains(label))
                    detected.append(label);
            };

            struct Rule {
                QRegularExpression pattern;
                QString label;
            };
            static const std::vector<Rule> rules = {
                // --- Delete / Drop (Silme) ---
                { QRegularExpression(QStringLiteral("\\.\\s*dropDatabase\\s*\\(")),        QStringLiteral("dropDatabase()") },
                { QRegularExpression(QStringLiteral("\\.\\s*drop\\s*\\(")),                QStringLiteral("drop()") },
                { QRegularExpression(QStringLiteral("\\.\\s*deleteMany\\s*\\(")),          QStringLiteral("deleteMany()") },
                { QRegularExpression(QStringLiteral("\\.\\s*deleteOne\\s*\\(")),           QStringLiteral("deleteOne()") },
                { QRegularExpression(QStringLiteral("\\.\\s*remove\\s*\\(")),              QStringLiteral("remove()") },
                { QRegularExpression(QStringLiteral("\\.\\s*findOneAndDelete\\s*\\(")),    QStringLiteral("findOneAndDelete()") },
                { QRegularExpression(QStringLiteral("\\.\\s*dropIndexes\\s*\\(")),         QStringLiteral("dropIndexes()") },
                { QRegularExpression(QStringLiteral("\\.\\s*dropIndex\\s*\\(")),           QStringLiteral("dropIndex()") },
                { QRegularExpression(QStringLiteral("\\.\\s*dropAllUsers\\s*\\(")),        QStringLiteral("dropAllUsers()") },
                { QRegularExpression(QStringLiteral("\\.\\s*dropUser\\s*\\(")),            QStringLiteral("dropUser()") },
                { QRegularExpression(QStringLiteral("\\.\\s*dropAllRoles\\s*\\(")),        QStringLiteral("dropAllRoles()") },
                { QRegularExpression(QStringLiteral("\\.\\s*dropRole\\s*\\(")),            QStringLiteral("dropRole()") },
                { QRegularExpression(QStringLiteral("\\.\\s*killOp\\s*\\(")),              QStringLiteral("killOp()") },

                // --- Update / Modify (Güncelleme) ---
                { QRegularExpression(QStringLiteral("\\.\\s*updateOne\\s*\\(")),           QStringLiteral("updateOne()") },
                { QRegularExpression(QStringLiteral("\\.\\s*updateMany\\s*\\(")),          QStringLiteral("updateMany()") },
                { QRegularExpression(QStringLiteral("\\.\\s*update\\s*\\(")),              QStringLiteral("update()") },
                { QRegularExpression(QStringLiteral("\\.\\s*replaceOne\\s*\\(")),          QStringLiteral("replaceOne()") },
                { QRegularExpression(QStringLiteral("\\.\\s*findOneAndUpdate\\s*\\(")),    QStringLiteral("findOneAndUpdate()") },
                { QRegularExpression(QStringLiteral("\\.\\s*findOneAndReplace\\s*\\(")),   QStringLiteral("findOneAndReplace()") },
                { QRegularExpression(QStringLiteral("\\.\\s*bulkWrite\\s*\\(")),           QStringLiteral("bulkWrite()") },
                { QRegularExpression(QStringLiteral("\\.\\s*renameCollection\\s*\\(")),    QStringLiteral("renameCollection()") },
                { QRegularExpression(QStringLiteral("\\.\\s*convertToCapped\\s*\\(")),     QStringLiteral("convertToCapped()") },
                { QRegularExpression(QStringLiteral("\\.\\s*hideIndex\\s*\\(")),           QStringLiteral("hideIndex()") },
                { QRegularExpression(QStringLiteral("\\.\\s*unhideIndex\\s*\\(")),         QStringLiteral("unhideIndex()") },
                { QRegularExpression(QStringLiteral("\\.\\s*reIndex\\s*\\(")),             QStringLiteral("reIndex()") },
                { QRegularExpression(QStringLiteral("\\.\\s*compact\\s*\\(")),             QStringLiteral("compact()") },
                { QRegularExpression(QStringLiteral("\\.\\s*updateUser\\s*\\(")),          QStringLiteral("updateUser()") },
                { QRegularExpression(QStringLiteral("\\.\\s*changeUserPassword\\s*\\(")),  QStringLiteral("changeUserPassword()") },
                { QRegularExpression(QStringLiteral("\\.\\s*grantRolesToUser\\s*\\(")),    QStringLiteral("grantRolesToUser()") },
                { QRegularExpression(QStringLiteral("\\.\\s*revokeRolesFromUser\\s*\\(")), QStringLiteral("revokeRolesFromUser()") },
                { QRegularExpression(QStringLiteral("\\.\\s*updateRole\\s*\\(")),          QStringLiteral("updateRole()") },

                // --- Insert / Create (Ekleme) ---
                { QRegularExpression(QStringLiteral("\\.\\s*insertOne\\s*\\(")),           QStringLiteral("insertOne()") },
                { QRegularExpression(QStringLiteral("\\.\\s*insertMany\\s*\\(")),          QStringLiteral("insertMany()") },
                { QRegularExpression(QStringLiteral("\\.\\s*insert\\s*\\(")),              QStringLiteral("insert()") },
                { QRegularExpression(QStringLiteral("\\.\\s*save\\s*\\(")),                QStringLiteral("save()") },
                { QRegularExpression(QStringLiteral("\\.\\s*createCollection\\s*\\(")),    QStringLiteral("createCollection()") },
                { QRegularExpression(QStringLiteral("\\.\\s*createView\\s*\\(")),          QStringLiteral("createView()") },
                { QRegularExpression(QStringLiteral("\\.\\s*createIndexes\\s*\\(")),       QStringLiteral("createIndexes()") },
                { QRegularExpression(QStringLiteral("\\.\\s*createIndex\\s*\\(")),         QStringLiteral("createIndex()") },
                { QRegularExpression(QStringLiteral("\\.\\s*ensureIndex\\s*\\(")),         QStringLiteral("ensureIndex()") },
                { QRegularExpression(QStringLiteral("\\.\\s*createUser\\s*\\(")),          QStringLiteral("createUser()") },
                { QRegularExpression(QStringLiteral("\\.\\s*createRole\\s*\\(")),          QStringLiteral("createRole()") },
            };

            for (const auto &rule : rules) {
                if (rule.pattern.match(sanitized).hasMatch())
                    addUnique(rule.label);
            }

            // Detect findAndModify: distinguish remove:true (Delete) vs update/upsert (Update)
            static const QRegularExpression reFindAndModifyAny(
                QStringLiteral("\\.\\s*findAndModify\\s*\\("));
            if (reFindAndModifyAny.match(sanitized).hasMatch()) {
                static const QRegularExpression reFindAndModifyRemove(
                    QStringLiteral("\\.\\s*findAndModify\\s*\\([^;]*\\bremove\\s*:\\s*true\\b"));
                if (reFindAndModifyRemove.match(sanitized).hasMatch())
                    addUnique(QStringLiteral("findAndModify(remove: true)"));
                else
                    addUnique(QStringLiteral("findAndModify()"));
            }

            // Detect aggregation pipelines that write to a collection via $out or $merge
            static const QRegularExpression reAggregateCall(
                QStringLiteral("\\.\\s*aggregate\\s*\\("));
            static const QRegularExpression reAggregateExplainOpt(
                QStringLiteral("\\.\\s*aggregate\\s*\\([^;]*\\bexplain\\s*:\\s*true\\b"));
            if (reAggregateCall.match(sanitized).hasMatch() &&
                !reAggregateExplainOpt.match(sanitized).hasMatch()) {
                static const QRegularExpression reAggWriteStage(
                    QStringLiteral("[{\\s,][\"']?(\\$out|\\$merge)[\"']?\\s*:"));
                auto it = reAggWriteStage.globalMatch(noComments);
                while (it.hasNext()) {
                    const auto m = it.next();
                    addUnique(QStringLiteral("aggregate(%1)").arg(m.captured(1)));
                }
            }

            // Detect raw db.runCommand / db.adminCommand write or destructive wire commands
            static const QRegularExpression reRunCommandCall(
                QStringLiteral("\\.\\s*(?:runCommand|adminCommand)\\s*\\("));
            if (reRunCommandCall.match(sanitized).hasMatch()) {
                static const QRegularExpression reRawCmdKey(
                    QStringLiteral("[{\\s,][\"']?(dropDatabase|drop|delete|update|insert|findAndModify|create|createIndexes|dropIndexes|renameCollection|collMod|convertToCapped|compact|reIndex|killOp|createUser|updateUser|dropUser|dropAllUsersFromDatabase|grantRolesToUser|revokeRolesFromUser)[\"']?\\s*:"));
                auto it = reRawCmdKey.globalMatch(noComments);
                while (it.hasNext()) {
                    const auto m = it.next();
                    addUnique(QStringLiteral("runCommand(%1)").arg(m.captured(1)));
                }
            }

            return detected;
        }
    } // namespace MongoJsonUtils
} // namespace RagoShell
