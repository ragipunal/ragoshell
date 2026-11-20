#include "ragoshell/core/utils/FieldCompletion.h"

#include <map>

#include <QRegularExpression>
#include <QSet>

namespace RagoShell
{
    namespace FieldCompletion
    {
        namespace
        {
            constexpr int kMaxScan = 8000;
            /// Fields seen in fewer documents go to the end of the list.
            constexpr double kRarePresence = 0.05;

            /// What the keys of an object (or the elements of an array) are.
            enum class Role
            {
                None,
                Filter,       ///< query filter: field keys and operators
                FilterList,   ///< $and / $or / $nor array of filters
                Operators,    ///< a field's condition in a filter: { $gt: 1, $elemMatch: ... }
                Fields,       ///< projection, sort, index keys, update operator body
                Update,       ///< update document: operator keys ($set, ...)
                Pipeline,     ///< aggregate() stage array
                Stage,        ///< one stage: { $match: ... }
                Expression,   ///< aggregation expression: "$field" references
            };

            struct Frame
            {
                QChar open;
                int pos = 0;
                Role role = Role::None;
                bool inAggregate = false;
                // '(' frames
                QString callName;
                QString collection;
                int arg = 0;          ///< top-level commas so far (also array elements)
                // '{' frames
                bool keyPos = true;   ///< the caret would type a key
                QString keyToken;     ///< the key being written
                QString lastKey;      ///< key of the entry whose value is being written
            };

            bool isIdentChar(QChar c)
            {
                return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
            }

            void skipSpaceBack(const QString &t, int &p)
            {
                while (p >= 0 && t[p].isSpace())
                    --p;
            }

            /// The identifier ending at p; p ends up before it.
            QString identifierBack(const QString &t, int &p)
            {
                const int end = p;
                while (p >= 0 && isIdentChar(t[p]))
                    --p;
                return t.mid(p + 1, end - p);
            }

            QString unquote(const QString &literal)
            {
                const QString s = literal.trimmed();
                if (s.size() >= 2 && (s[0] == QLatin1Char('\'') || s[0] == QLatin1Char('"') || s[0] == QLatin1Char('`'))
                    && s.back() == s[0])
                    return s.mid(1, s.size() - 2);
                return {};
            }

            /// The collection a method call is made on: db.c.m(, db.getCollection('c').m(,
            /// db.c.find(...).m(. Empty when it is not one.
            QString receiverCollection(const QString &t, int nameStart, const std::map<int, int> &closeToOpen)
            {
                int p = nameStart - 1;
                skipSpaceBack(t, p);
                if (p < 0 || t[p] != QLatin1Char('.'))
                    return {};
                --p;
                for (int guard = 0; guard < 64; ++guard) {
                    skipSpaceBack(t, p);
                    if (p < 0)
                        return {};
                    if (t[p] == QLatin1Char(')')) {
                        const auto it = closeToOpen.find(p);
                        if (it == closeToOpen.end())
                            return {};
                        const int open = it->second;
                        int q = open - 1;
                        skipSpaceBack(t, q);
                        const QString ident = identifierBack(t, q);
                        if (ident.isEmpty())
                            return {};
                        skipSpaceBack(t, q);
                        if (q < 0 || t[q] != QLatin1Char('.'))
                            return {};
                        if (ident == QLatin1String("getCollection")) {
                            int r = q - 1;
                            skipSpaceBack(t, r);
                            return identifierBack(t, r) == QLatin1String("db") ? unquote(t.mid(open + 1, p - open - 1))
                                                                               : QString();
                        }
                        p = q - 1;   // an earlier call of the chain: find(...).sort(
                        continue;
                    }
                    const QString ident = identifierBack(t, p);
                    if (ident.isEmpty())
                        return {};
                    skipSpaceBack(t, p);
                    if (p < 0 || t[p] != QLatin1Char('.'))
                        return {};
                    int q = p - 1;
                    skipSpaceBack(t, q);
                    return identifierBack(t, q) == QLatin1String("db") ? ident : QString();
                }
                return {};
            }

            Role argumentRole(const QString &method, int arg, QChar open)
            {
                static const QSet<QString> filterFirst = {
                    "find", "findOne", "count", "countDocuments", "deleteOne", "deleteMany", "remove",
                    "findOneAndDelete", "findOneAndUpdate", "findOneAndReplace", "updateOne", "updateMany",
                    "update", "replaceOne"};
                static const QSet<QString> updates = {"updateOne", "updateMany", "update", "findOneAndUpdate"};
                static const QSet<QString> fieldsFirst = {"sort", "projection", "hint", "createIndex"};
                if (open == QLatin1Char('['))
                    return method == QLatin1String("aggregate") && arg == 0 ? Role::Pipeline : Role::None;
                if (arg == 0 && filterFirst.contains(method))
                    return Role::Filter;
                if (arg == 0 && fieldsFirst.contains(method))
                    return Role::Fields;
                if (arg == 1 && (method == QLatin1String("find") || method == QLatin1String("findOne")))
                    return Role::Fields;
                if (arg == 1 && updates.contains(method))
                    return Role::Update;
                if (arg == 1 && method == QLatin1String("distinct"))
                    return Role::Filter;
                return Role::None;
            }

            Role valueRole(Role parent, const QString &key, QChar open)
            {
                const bool object = open == QLatin1Char('{');
                switch (parent) {
                case Role::Filter:
                    if (!object && (key == QLatin1String("$and") || key == QLatin1String("$or")
                                    || key == QLatin1String("$nor")))
                        return Role::FilterList;
                    if (object && key == QLatin1String("$elemMatch"))
                        return Role::Filter;
                    if (key == QLatin1String("$expr"))
                        return Role::Expression;
                    return object && !key.startsWith(QLatin1Char('$')) ? Role::Operators : Role::None;
                case Role::Operators:
                    // { items: { $elemMatch: { qty: ... } } }: fields of the array's documents.
                    return object && key == QLatin1String("$elemMatch") ? Role::Filter : Role::None;
                case Role::Update:
                    return object && key.startsWith(QLatin1Char('$')) ? Role::Fields : Role::None;
                case Role::Stage:
                    if (key == QLatin1String("$match"))
                        return object ? Role::Filter : Role::None;
                    if (key == QLatin1String("$sort") || key == QLatin1String("$project"))
                        return object ? Role::Fields : Role::None;
                    return Role::Expression;
                case Role::Fields:
                case Role::Expression:
                    return Role::Expression;
                default:
                    return Role::None;
                }
            }

            Role elementRole(Role parent)
            {
                switch (parent) {
                case Role::Pipeline:   return Role::Stage;
                case Role::FilterList: return Role::Filter;
                case Role::Expression: return Role::Expression;
                default:               return Role::None;
                }
            }

            bool takesFieldKeys(Role role)
            {
                return role == Role::Filter || role == Role::Fields;
            }

            QString detailText(const SchemaField &f)
            {
                QStringList types;
                for (const auto &[type, n] : f.types) {
                    Q_UNUSED(n)
                    if (type == QLatin1String("Array") && !f.itemTypes.empty())
                        types << QStringLiteral("Array<%1>").arg(f.itemTypes.front().first);
                    else
                        types << type;
                    if (types.size() == 2)
                        break;
                }
                const int pct = qRound(f.presence * 100);
                const QString presence = pct == 0 && f.presence > 0 ? QStringLiteral("<1%")
                                                                    : QStringLiteral("%1%").arg(pct);
                return types.isEmpty() ? presence : types.join(QStringLiteral(" | ")) + QStringLiteral(" · ") + presence;
            }
        }

        bool isPlainKey(const QString &path)
        {
            static const QRegularExpression plain(QStringLiteral(R"(^[A-Za-z_$][A-Za-z0-9_$]*$)"));
            return plain.match(path).hasMatch();
        }

        Context analyze(const QString &textBeforeCaret)
        {
            const QString t = textBeforeCaret.right(kMaxScan);
            const int n = t.size();
            std::vector<Frame> stack;
            std::map<int, int> closeToOpen;
            int stringStart = -1;
            QChar quote;
            QChar lastSignificant;
            QChar lastNonIdentifier;   ///< last significant character outside identifiers and comments
            auto significant = [&](QChar ch) {
                lastSignificant = ch;
                if (!isIdentChar(ch))
                    lastNonIdentifier = ch;
            };

            for (int i = 0; i < n; ++i) {
                const QChar c = t[i];
                const QChar next = i + 1 < n ? t[i + 1] : QChar();

                if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
                    const int end = t.indexOf(QLatin1Char('\n'), i);
                    if (end < 0)
                        return {};   // the caret is in a comment
                    i = end;
                    continue;
                }
                if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
                    const int end = t.indexOf(QStringLiteral("*/"), i + 2);
                    if (end < 0)
                        return {};
                    i = end + 1;
                    continue;
                }
                if (c == QLatin1Char('/')
                    && (lastSignificant.isNull() || QStringLiteral("(,:[{=!&|?;").contains(lastSignificant))) {
                    // A regex literal: /^ab[c/]d/i
                    int j = i + 1;
                    bool inClass = false;
                    for (; j < n; ++j) {
                        const QChar d = t[j];
                        if (d == QLatin1Char('\\')) {
                            ++j;
                        } else if (d == QLatin1Char('[')) {
                            inClass = true;
                        } else if (d == QLatin1Char(']')) {
                            inClass = false;
                        } else if ((d == QLatin1Char('/') && !inClass) || d == QLatin1Char('\n')) {
                            break;
                        }
                    }
                    if (j >= n || t[j] != QLatin1Char('/'))
                        return {};
                    i = j;
                    while (i + 1 < n && t[i + 1].isLetter())
                        ++i;   // flags
                    significant(QLatin1Char('/'));
                    continue;
                }
                if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')) {
                    int j = i + 1;
                    for (; j < n; ++j) {
                        if (t[j] == QLatin1Char('\\'))
                            ++j;
                        else if (t[j] == c)
                            break;
                    }
                    if (j >= n) {   // the caret is inside this string
                        stringStart = i;
                        quote = c;
                        break;
                    }
                    if (!stack.empty() && stack.back().open == QLatin1Char('{') && stack.back().keyPos)
                        stack.back().keyToken = t.mid(i + 1, j - i - 1);
                    i = j;
                    significant(c);
                    continue;
                }
                if (c.isSpace())
                    continue;

                Frame *top = stack.empty() ? nullptr : &stack.back();
                if (c == QLatin1Char('(') || c == QLatin1Char('{') || c == QLatin1Char('[')) {
                    Frame f;
                    f.open = c;
                    f.pos = i;
                    f.inAggregate = top && top->inAggregate;
                    if (c == QLatin1Char('(')) {
                        int p = i - 1;
                        skipSpaceBack(t, p);
                        f.callName = identifierBack(t, p);
                        if (!f.callName.isEmpty())
                            f.collection = receiverCollection(t, p + 1, closeToOpen);
                        if (f.callName == QLatin1String("aggregate"))
                            f.inAggregate = true;
                    } else if (top) {
                        if (top->open == QLatin1Char('('))
                            f.role = argumentRole(top->callName, top->arg, c);
                        else if (top->open == QLatin1Char('{'))
                            f.role = valueRole(top->role, top->lastKey, c);
                        else
                            f.role = elementRole(top->role);
                    }
                    stack.push_back(f);
                    significant(c);
                    continue;
                }
                if (c == QLatin1Char(')') || c == QLatin1Char('}') || c == QLatin1Char(']')) {
                    if (!stack.empty()) {
                        closeToOpen[i] = stack.back().pos;
                        stack.pop_back();
                    }
                    significant(c);
                    continue;
                }
                if (top && top->open == QLatin1Char('{')) {
                    if (c == QLatin1Char(':') && top->keyPos) {
                        top->lastKey = top->keyToken;
                        top->keyToken.clear();
                        top->keyPos = false;
                    } else if (c == QLatin1Char(',')) {
                        top->keyPos = true;
                        top->keyToken.clear();
                        top->lastKey.clear();
                    } else if (top->keyPos && isIdentChar(c)) {
                        const bool continues = i > 0 && isIdentChar(t[i - 1]) && !top->keyToken.isEmpty();
                        top->keyToken = continues ? top->keyToken + c : QString(c);
                    }
                } else if (top && c == QLatin1Char(',')) {
                    ++top->arg;
                }
                significant(c);
            }

            if (stack.empty())
                return {};
            const Frame &top = stack.back();
            QString collection;
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                if (it->open == QLatin1Char('(') && !it->collection.isEmpty()) {
                    collection = it->collection;
                    break;
                }
            }
            if (collection.isEmpty())
                return {};

            Context context;
            context.collection = collection;
            if (stringStart >= 0) {
                const QString content = t.mid(stringStart + 1);
                if (content.contains(QLatin1Char('\n')) || quote == QLatin1Char('`'))
                    return {};
                if (top.open == QLatin1Char('{') && top.keyPos) {
                    if (!takesFieldKeys(top.role) || content.startsWith(QLatin1Char('$')))
                        return {};
                    context.kind = Kind::QuotedKey;
                    context.prefix = content;
                    context.replaceLength = content.size();
                    return context;
                }
                // "$field" in a value of an aggregation expression or projection.
                if (content.startsWith(QLatin1Char('$')) && !content.startsWith(QStringLiteral("$$"))
                    && top.inAggregate && (top.role == Role::Expression || top.role == Role::Fields)) {
                    context.kind = Kind::Reference;
                    context.prefix = content.mid(1);
                    context.replaceLength = content.size();
                    return context;
                }
                if (top.open == QLatin1Char('(') && top.callName == QLatin1String("distinct") && top.arg == 0) {
                    context.kind = Kind::Argument;
                    context.prefix = content;
                    context.replaceLength = content.size();
                    return context;
                }
                return {};
            }

            if (top.open == QLatin1Char('{') && top.keyPos && takesFieldKeys(top.role)) {
                int p = n - 1;
                const QString token = identifierBack(t, p);
                // Right after '{' or ',' (comments aside): a key is being typed,
                // not after a complete key ("status" ▮ or status ▮).
                if (lastNonIdentifier != QLatin1Char('{') && lastNonIdentifier != QLatin1Char(','))
                    return {};
                if (token != top.keyToken)
                    return {};
                if (token.startsWith(QLatin1Char('$')))
                    return {};   // an operator ($or, $set): the shell's own completions
                context.kind = Kind::Key;
                context.prefix = token;
                context.replaceLength = token.size();
                return context;
            }
            return {};
        }

        std::vector<Item> complete(const Context &context, const std::vector<SchemaField> &fields, int maxItems)
        {
            std::vector<Item> common;
            std::vector<Item> rare;
            if (context.kind == Kind::None)
                return common;
            for (const SchemaField &f : fields) {
                if (!context.prefix.isEmpty() && !f.path.startsWith(context.prefix, Qt::CaseInsensitive))
                    continue;
                Item item;
                item.label = f.path;
                switch (context.kind) {
                case Kind::Key:
                    item.insert = isPlainKey(f.path) ? f.path : QStringLiteral("\"%1\"").arg(f.path);
                    break;
                case Kind::Reference:
                    item.insert = QLatin1Char('$') + f.path;
                    break;
                default:
                    item.insert = f.path;
                    break;
                }
                item.detail = detailText(f);
                (f.presence < kRarePresence ? rare : common).push_back(item);
            }
            common.insert(common.end(), rare.begin(), rare.end());
            if (static_cast<int>(common.size()) > maxItems)
                common.resize(maxItems);
            return common;
        }
    }
}
